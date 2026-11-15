#include "../src/engine.hpp"
#include <algorithm>
#include <cassert>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <limits>
#include <unistd.h>
namespace {
using Rows=std::vector<std::vector<mvdb::Value>>;
Rows rows(mvdb::Engine& e,const std::string& sql){Rows r;e.execute(sql).each([&](const auto& v){r.push_back(v);});return r;}
template<class F>void fails(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}assert(threw);}
bool accepts(int n,const std::string& op){return op=="="?n==0:op=="!="||op=="<>"?n!=0:op=="<"?n<0:op==">"?n>0:op=="<="?n<=0:n>=0;}
void semantics(const std::filesystem::path& dir){
 mvdb::Engine e(dir,8<<20);
 e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,payload VARCHAR(70000),n BIGINT,v VECTOR(2));");
 // The long string comes before n; skipping it must handle several pages and
 // field headers split across page boundaries, without interpreting its bytes.
 std::string payload(32749,'x');
 e.execute("INSERT INTO t VALUES (-9,'"+payload+"',-9223372036854775808,'[0,0]'),(-2,'z',-3,'[1,0]'),(-1,'a',-1,'[2,0]'),(0,'m',0,'[3,0]'),(1,'y',4,'[4,0]'),(9,'b',9223372036854775807,'[5,0]');");
 for(const std::string op:{"=","!=","<>","<",">","<=",">="}){
  auto got=rows(e,"SELECT id FROM t WHERE payload>='' AND n"+op+"0 ORDER BY id;");
  auto reversed=rows(e,"SELECT id FROM t WHERE n"+op+"0 AND payload>='' ORDER BY id;");assert(got==reversed);
  auto all=rows(e,"SELECT id,n FROM t ORDER BY id;");Rows expected;
  for(auto& r:all){auto n=std::get<int64_t>(r[1]);if(accepts((n>0)-(n<0),op))expected.push_back({r[0]});}assert(got==expected);
 }
 assert(rows(e,"SELECT id FROM t WHERE payload>='' AND n<0 AND n>=0;").empty());
 assert(rows(e,"SELECT id FROM t WHERE payload>='' AND id<0 AND n<0;").size()==3);
 assert(rows(e,"SELECT id FROM t WHERE payload>='' AND n=-9223372036854775808;").size()==1);
 assert(rows(e,"SELECT id FROM t WHERE payload>='' AND n=9223372036854775807;").size()==1);
 // Both eager SQL validation and source-order unsupported comparison behavior
 // must survive short circuiting, including predicates behind a false scalar.
 fails([&]{e.execute("SELECT id FROM t WHERE n< -100 AND missing=1;");});
 fails([&]{e.execute("SELECT id FROM t WHERE n< -100 AND n='bad';");});
 fails([&]{e.execute("SELECT id FROM t WHERE v='[0,0]' AND id>100;");});
 assert(rows(e,"SELECT id FROM t WHERE id>100 AND v='[0,0]';").empty());
 fails([&]{e.execute("UPDATE t SET n='bad' WHERE payload>='' AND id>100;");});
 auto original=rows(e,"SELECT id,n FROM t ORDER BY id;");
 e.execute("BEGIN;");assert(e.execute("UPDATE t SET n=5 WHERE payload>='' AND n<0;").affected_rows==3);e.execute("ROLLBACK;");assert(rows(e,"SELECT id,n FROM t ORDER BY id;")==original);
 e.execute("BEGIN;");assert(e.execute("DELETE FROM t WHERE payload>='' AND n<0;").affected_rows==3);e.execute("ROLLBACK;");assert(rows(e,"SELECT id,n FROM t ORDER BY id;")==original);
 // A failure after an earlier matching write still rolls back the statement.
 fails([&]{e.execute("UPDATE t SET id=8 WHERE payload>='' AND n<0;");});assert(rows(e,"SELECT id,n FROM t ORDER BY id;")==original);
 assert(e.execute("UPDATE t SET n=5 WHERE id=-2 AND payload>='' AND n<0;").affected_rows==1);
 assert(e.execute("DELETE FROM t WHERE id=-1 AND payload>='' AND n>0;").affected_rows==0);
 assert(rows(e,"SELECT id FROM t WHERE payload>='' AND n<0 LIMIT 0;").empty());
 // Vary packed offsets through a page boundary: both the BIGINT tag and
 // its eight-byte value must work across pages, alongside repeated numeric
 // conditions and a primary key that is not the first schema column.
 e.execute("CREATE TABLE edge (s VARCHAR(40000),k BIGINT PRIMARY KEY,n BIGINT,x BIGINT);");
 for(int i=0;i<32;++i)e.execute("INSERT INTO edge VALUES ('"+std::string(32735+i,'x')+"',"+std::to_string(i)+","+std::to_string(i)+",-1);");
 auto edge=rows(e,"SELECT k FROM edge WHERE s>='' AND n>=0 AND x<0 AND n<32 ORDER BY k;");assert(edge.size()==32);
 for(size_t i=0;i<edge.size();++i)assert(std::get<int64_t>(edge[i][0])==int64_t(i));
 assert(rows(e,"SELECT k FROM edge WHERE s>='' AND n<0 AND x<0;").empty());
 // No and single predicates stay on their original decode path.
 assert(rows(e,"SELECT id FROM t;").size()==6);assert(rows(e,"SELECT id FROM t WHERE n>0;").size()==3);
}
void benchmark(const std::filesystem::path& dir,bool control){
 mvdb::Engine e(dir,8<<20);e.execute("CREATE TABLE bench (id BIGINT PRIMARY KEY,payload VARCHAR(40000),n BIGINT);");
 const std::string text(32768,'x');
 for(int start=0;start<10000;start+=50){std::string sql="INSERT INTO bench VALUES ";for(int i=start;i<start+50;++i){if(i!=start)sql+=',';sql+='('+std::to_string(i-5000)+",'"+text+"',"+std::to_string(i-5000)+')';}e.execute(sql+';');}
 struct Sample{uint64_t pins,loads,blob;double cpu_ms;};
 auto sample=[&](bool lazy){auto pins=e.page_access_count(),loads=e.buffer_page_load_count(),candidates=e.candidate_slots_examined();auto begin=std::clock();
  assert(rows(e,lazy?"SELECT id FROM bench WHERE payload>='' AND n< -10000;":"SELECT id FROM bench WHERE n< -10000;").empty());
  auto count=e.candidate_slots_examined()-candidates;assert(count==10000);auto accesses=e.page_access_count()-pins;
  // This key-only query returns no rows: all pins apart from one header,
  // three validity blocks, and one Meta per candidate are .blob requests.
  return Sample{accesses,e.buffer_page_load_count()-loads,accesses-count-4,1000.0*(std::clock()-begin)/CLOCKS_PER_SEC};};
 Sample original{},lazy{};std::vector<double> original_cpu,lazy_cpu;
 for(int i=0;i<3;++i){original=sample(false);lazy=sample(true);original_cpu.push_back(original.cpu_ms);lazy_cpu.push_back(lazy.cpu_ms);}
 if(!control){assert(lazy.pins<original.pins);assert(lazy.blob<original.blob);}
 std::sort(original_cpu.begin(),original_cpu.end());std::sort(lazy_cpu.begin(),lazy_cpu.end());
 std::cout<<"predicate_10000 long_varchar_bytes=32768 original_single pins="<<original.pins<<" blob_requests="<<original.blob<<" page_loads="<<original.loads<<" median_cpu_ms="<<original_cpu[1]<<"; multi_AND pins="<<lazy.pins<<" blob_requests="<<lazy.blob<<" page_loads="<<lazy.loads<<" median_cpu_ms="<<lazy_cpu[1]<<'\n';
}
}
int main(int argc,char** argv){auto dir=std::filesystem::temp_directory_path()/("mvdb_predicate_regression_"+std::to_string(::getpid()));if(argc>1&&std::string(argv[1])=="--control")dir+="_control";std::filesystem::remove_all(dir);
 if(argc>1)benchmark(dir,std::string(argv[1])=="--control");else semantics(dir);
 std::filesystem::remove_all(dir);std::cout<<"predicate regression ok\n";
}
