#include "../src/engine.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
namespace {
using Rows=std::vector<std::vector<mvdb::Value>>;
Rows rows(mvdb::Engine& e,const std::string& sql){Rows r;e.execute(sql).each([&](const auto& v){r.push_back(v);});return r;}
template<class F>void fails(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}assert(threw);}
}
void r12_point_regressions(){
 auto dir=std::filesystem::temp_directory_path()/"mvdb_r12_point";std::filesystem::remove_all(dir);
 {
 mvdb::Engine e(dir,1<<20);e.execute("CREATE TABLE t (code BIGINT PRIMARY KEY,tag VARCHAR(12),n BIGINT);");
 e.execute("INSERT INTO t VALUES (-9223372036854775808,'low',1),(9223372036854775807,'high',2),(-1,'neg',3),(0,'zero',4),(1,'one',5),(2,'two',6);");
 auto look=e.point_lookup_count(),blocks=e.candidate_bitmap_blocks(),slots=e.candidate_slots_examined(),spill=e.spill_bytes();
 auto r=rows(e,"SELECT tag,n FROM t WHERE code=-1 AND n=3 ORDER BY tag DESC;");assert(r.size()==1&&std::get<std::string>(r[0][0])=="neg");
 assert(e.point_lookup_count()==look+1&&e.candidate_bitmap_blocks()==blocks+1&&e.candidate_slots_examined()==slots+1);assert(e.spill_bytes()==spill);
 for(auto key:{"-9223372036854775808","9223372036854775807","-1","0"})assert(rows(e,std::string("SELECT code FROM t WHERE code=")+key+";").size()==1);
 assert(rows(e,"SELECT * FROM t WHERE code=99;").empty());
 assert(rows(e,"SELECT * FROM t WHERE code=1 AND code=2;").empty());
 assert(rows(e,"SELECT * FROM t WHERE code=1 AND tag='two';").empty());
 assert(rows(e,"SELECT * FROM t WHERE code=1 LIMIT 0;").empty());
 fails([&]{e.execute("SELECT missing FROM t WHERE code=99;");});
 fails([&]{e.execute("SELECT tag FROM t WHERE code=99 ORDER BY missing;");});
 fails([&]{e.execute("SELECT L2_DISTANCE(n,'[0,0]') FROM t WHERE code=99;");});
 fails([&]{e.execute("UPDATE t SET missing=2 WHERE code=99;");});
 fails([&]{e.execute("UPDATE t SET n='bad' WHERE code=99;");});
 e.execute("BEGIN;");assert(e.execute("UPDATE t SET code=10,tag='changed' WHERE code=1;").affected_rows==1);assert(rows(e,"SELECT * FROM t WHERE code=10;").size()==1);e.execute("ROLLBACK;");assert(rows(e,"SELECT * FROM t WHERE code=1;").size()==1);assert(rows(e,"SELECT * FROM t WHERE code=10;").empty());
 e.execute("BEGIN;");fails([&]{e.execute("UPDATE t SET code=2,tag='bad' WHERE code=1;");});assert(std::get<std::string>(rows(e,"SELECT tag FROM t WHERE code=1;")[0][0])=="one");e.execute("COMMIT;");
 assert(e.execute("DELETE FROM t WHERE code=2 AND tag='wrong';").affected_rows==0);
 assert(e.execute("DELETE FROM t WHERE code=2;").affected_rows==1);assert(rows(e,"SELECT * FROM t WHERE code=2;").empty());
 auto before=e.candidate_slots_examined();look=e.point_lookup_count();blocks=e.candidate_bitmap_blocks();
 assert(e.execute("UPDATE t SET n=8 WHERE code=-1 AND tag='wrong';").affected_rows==0);
 assert(e.execute("UPDATE t SET n=8 WHERE code=1 AND code=2;").affected_rows==0);
 assert(e.execute("DELETE FROM t WHERE code=1 AND code=2;").affected_rows==0);
 assert(e.point_lookup_count()==look+1&&e.candidate_bitmap_blocks()==blocks+1&&e.candidate_slots_examined()==before+1);
 auto rejected=e.signature_rejected_rows();assert(rows(e,"SELECT code FROM t WHERE tag='neg' AND n=3;").size()==1);assert(e.signature_rejected_rows()>rejected);
 e.execute("CREATE TABLE v (k BIGINT PRIMARY KEY,tag VARCHAR(8),v VECTOR(2));");e.execute("INSERT INTO v VALUES (1,'x','[3,4]'),(2,'y','[0,0]');");e.execute("CREATE VECTOR INDEX ix ON v(v);");
 auto vr=rows(e,"SELECT tag,L2_DISTANCE(v,'[0,0]') AS d,v FROM v WHERE k=1 ORDER BY d;");assert(vr.size()==1&&std::get<double>(vr[0][1])==5.0);assert(e.execute("DELETE FROM v WHERE k=1;").affected_rows==1);assert(rows(e,"SELECT * FROM v WHERE k=1;").empty());
 e.execute("CREATE TABLE bench (id BIGINT PRIMARY KEY,tag VARCHAR(8));");
 for(int start=0;start<10000;start+=200){std::string sql="INSERT INTO bench VALUES ";for(int j=start;j<start+200;++j){if(j!=start)sql+=",";sql+="("+std::to_string(j)+",'x')";}e.execute(sql+";");}
 struct Sample{uint64_t look,blocks,slots,reads,pins;long long us;};
 auto sample=[&](bool point){auto a=e.point_lookup_count(),b=e.candidate_bitmap_blocks(),c=e.candidate_slots_examined(),d=e.buffer_page_load_count();auto pins=e.page_access_count();auto begin=std::chrono::steady_clock::now();auto got=rows(e,point?"SELECT id,tag FROM bench WHERE id=7777;":"SELECT id,tag FROM bench WHERE id>=7777 AND id<=7777;");assert(got.size()==1&&std::get<int64_t>(got[0][0])==7777);return Sample{e.point_lookup_count()-a,e.candidate_bitmap_blocks()-b,e.candidate_slots_examined()-c,e.buffer_page_load_count()-d,e.page_access_count()-pins,std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count()};};
 auto p=sample(true),q=sample(false);assert(p.look==1&&p.blocks==1&&p.slots==1);assert(q.look==0&&q.blocks==3&&q.slots==10000);assert(p.pins<q.pins);
 std::cout<<"r12_point 10000 rows point lookups="<<p.look<<" bitmap_blocks="<<p.blocks<<" candidates="<<p.slots<<" pool_page_loads="<<p.reads<<" pin_requests="<<p.pins<<" us="<<p.us<<"; range bitmap_blocks="<<q.blocks<<" candidates="<<q.slots<<" pool_page_loads="<<q.reads<<" pin_requests="<<q.pins<<" us="<<q.us<<"\n";
 for(bool erase:{false,true}){
  uint64_t counts[2]{};long long micros[2]{};for(int range=0;range<2;++range){e.execute("BEGIN;");auto pins=e.page_access_count(),candidates=e.candidate_slots_examined();
   auto sql=std::string(erase?"DELETE FROM bench":"UPDATE bench SET tag='z'")+" WHERE "+(range?"id>=7777 AND id<=7777;":"id=7777;");auto begin=std::chrono::steady_clock::now();assert(e.execute(sql).affected_rows==1);
   micros[range]=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
   counts[range]=e.page_access_count()-pins;assert(e.candidate_slots_examined()-candidates==(range?10000:1));e.execute("ROLLBACK;");}
  assert(counts[0]<counts[1]);std::cout<<"r12_point "<<(erase?"DELETE":"UPDATE")<<" point pin_requests="<<counts[0]<<" us="<<micros[0]<<" range pin_requests="<<counts[1]<<" us="<<micros[1]<<"\n";
 }
 auto warm_point=sample(true),warm_range=sample(false);assert(warm_point.pins<warm_range.pins);
 std::cout<<"r12_point warm point pin_requests="<<warm_point.pins<<" pool_page_loads="<<warm_point.reads<<" us="<<warm_point.us<<"; range pin_requests="<<warm_range.pins<<" pool_page_loads="<<warm_range.reads<<" us="<<warm_range.us<<"\n";
 }
 {mvdb::Engine e(dir,1<<20);
 auto vector_reads=e.vector_page_reads();auto scalar=rows(e,"SELECT k,tag FROM v WHERE k=2;");
 assert(scalar.size()==1&&std::get<std::string>(scalar[0][1])=="y");assert(e.vector_page_reads()==vector_reads);
 auto vector=rows(e,"SELECT v,L2_DISTANCE(v,'[3,4]') AS d FROM v WHERE k=2;");
 assert(vector.size()==1&&std::get<std::vector<float>>(vector[0][0])==std::vector<float>({0,0})&&std::get<double>(vector[0][1])==5.0);assert(e.vector_page_reads()>vector_reads);
 assert(rows(e,"SELECT tag FROM t WHERE code=-9223372036854775808;").size()==1);assert(rows(e,"SELECT * FROM t WHERE code=2;").empty());assert(rows(e,"SELECT * FROM bench WHERE id=7777;").size()==1);}
 // A cached vector ranking must release its reservation before a large
 // scalar point projection, just as the ordinary scalar executor does.
 {mvdb::Engine e(dir/"cache_admission",262144);
  e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,s VARCHAR(10000),v VECTOR(128));");
  std::string zero="'[0";for(int i=1;i<128;++i)zero+=",0";zero+="]'";
  e.execute("INSERT INTO t VALUES (1,'"+std::string(9000,'x')+"',"+zero+");");
  assert(rows(e,"SELECT id,L2_DISTANCE(v,"+zero+") AS d FROM t ORDER BY d,id LIMIT 1;").size()==1);
  assert(e.read_cache_slots()==1);
  std::string query="SELECT s";for(int i=1;i<9;++i)query+=",s";query+=" FROM t WHERE id=1;";
  auto wide=rows(e,query);assert(wide.size()==1&&wide[0].size()==9);
  for(const auto& value:wide[0])assert(std::get<std::string>(value)==std::string(9000,'x'));
  assert(e.read_cache_slots()==0&&e.budget_peak()<=262144);
 }
 // Deliberately stale index entries exercise visibility/key guards separately.
 auto stale=dir/"stale";
 {mvdb::Engine e(stale,1<<20);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,tag VARCHAR(8));");e.execute("INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');");e.execute("DELETE FROM t WHERE id=2;");e.execute("DELETE FROM t WHERE id=3;");}
 {mvdb::Storage storage(stale,1<<20);storage.begin();mvdb::PrimaryIndex pk(storage,"t.pk");
  pk.insert(90,0);pk.insert(91,1);pk.insert(92,2);pk.insert(93,999999);
  auto valid=storage.read("t.valid",0);valid[0]|=std::byte(4);storage.write("t.valid",0,valid);
  // Signature collision simulation: the tag='b' signature also points at 'a'.
  auto sketch=storage.read("t.s1",0);std::memcpy(sketch.data(),sketch.data()+8,8);storage.write("t.s1",0,sketch);storage.commit();}
 {mvdb::Engine e(stale,1<<20);for(int k:{90,91,92,93}){
  auto query=" WHERE id="+std::to_string(k)+";";assert(rows(e,"SELECT * FROM t"+query).empty());
  assert(e.execute("UPDATE t SET tag='bad'"+query).affected_rows==0);assert(e.execute("DELETE FROM t"+query).affected_rows==0);}
  assert(rows(e,"SELECT id FROM t WHERE tag='b';").empty());assert(rows(e,"SELECT id FROM t WHERE id=1 AND tag='b';").empty());assert(rows(e,"SELECT id FROM t WHERE id=1;").size()==1);}
 std::filesystem::remove_all(dir);
}
