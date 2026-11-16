#include "../src/engine.hpp"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
namespace {
std::vector<int64_t> ids(mvdb::Engine& e,const std::string& sql){std::vector<int64_t> out;e.execute(sql).each([&](const auto& r){out.push_back(std::get<int64_t>(r[0]));});return out;}
uint64_t signature(const std::string& s){uint64_t h=1469598103934665603ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}return h;}
void borrowed_insert_boundary(const std::filesystem::path& path,size_t rows,bool extra_column){
 std::vector<int64_t> input(rows),ordinals(rows);
 {mvdb::Engine e(path,1<<20);
  e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(32),z BIGINT,v VECTOR(2)"+std::string(extra_column?",u VARCHAR(32)":"")+")");
  const std::string names=extra_column?"s,v,id,z,u":"s,v,id,z";
  auto tuple=[&](int64_t key,int64_t ordinal,const std::string& vector){
   return "('it''s中文','"+vector+"',"+std::to_string(key)+","+std::to_string(ordinal)+(extra_column?",'extra'":"")+")";
  };
  auto borrowed=e.borrowed_insert_batch_count(),ordered=e.ordered_pk_batch_count();
  std::string sql="INSERT INTO t("+names+") VALUES ";
  for(size_t i=0;i<rows;++i){auto key=static_cast<int64_t>((i*37)%rows);input[i]=key;ordinals[key]=static_cast<int64_t>(i);if(i)sql+=",";sql+=tuple(key,i,"[1,-0]");}
  assert(e.execute(sql).affected_rows==static_cast<int64_t>(rows));
  // 128 four-column rows exactly fill the 512-view staging capacity. The
  // 129-row and five-column cases exercise its two independent fallbacks.
  const bool retained=rows==128&&!extra_column;
  assert(e.borrowed_insert_batch_count()==borrowed+(retained?1:0));
  assert(e.ordered_pk_batch_count()==ordered+(rows<=128?1:0));
  size_t seen=0;
  e.execute("SELECT id,s,z FROM t ORDER BY id LIMIT 200").each([&](const auto& row){
   assert(seen<rows&&std::get<int64_t>(row[0])==static_cast<int64_t>(seen));
   assert(std::get<std::string>(row[1])=="it's中文");assert(std::get<int64_t>(row[2])==ordinals[seen]);++seen;
  });assert(seen==rows);
  if(extra_column)assert(ids(e,"SELECT id FROM t WHERE u='extra' ORDER BY id LIMIT 200").size()==rows);
  e.execute("BEGIN");
  e.execute("INSERT INTO t("+names+") VALUES "+tuple(1000,1000,"[2,2]"));
  bool failed=false;
  try{e.execute("INSERT INTO t("+names+") VALUES "+tuple(1001,1001,"[3,3]")+","+tuple(1002,1002,"[3]"));}
  catch(const std::exception&){failed=true;}
  assert(failed&&e.active_rows("t")==rows+1);
  assert(ids(e,"SELECT id FROM t WHERE id>=1000 ORDER BY id LIMIT 10")==std::vector<int64_t>{1000});
  // Commit the outer transaction: its earlier row must survive the failed
  // statement, while neither preinserted failed-statement key may survive.
  e.execute("COMMIT");e.close();
 }
 {std::ifstream file(path/"data/t.rows",std::ios::binary);assert(file);
  for(size_t i=0;i<rows;++i){int64_t key=0;file.seekg(static_cast<std::streamoff>(i*32));file.read(reinterpret_cast<char*>(&key),8);assert(file&&key==input[i]);}
 }
 {mvdb::Engine e(path,1<<20);assert(e.active_rows("t")==rows+1);
  assert(ids(e,"SELECT id FROM t WHERE id>=1000 ORDER BY id LIMIT 10")==std::vector<int64_t>{1000});
 }
}
}
void r12_engine_regressions(){
 const std::filesystem::path root="/tmp/mvdb_r12_engine";std::filesystem::remove_all(root);
 // Detect order before any writes: late disorder must not duplicate wide
 // payload writes by speculatively inserting and undoing a sorted prefix.
 {std::array<uint64_t,2> writes{};
  for(int disorder=0;disorder<2;++disorder){mvdb::Engine e(root/("preflight_writes"+std::to_string(disorder)),1<<20);
   e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(4096),v VECTOR(2))");auto before=e.data_page_write_bytes();
   for(int batch=0;batch<8;++batch){std::string q="INSERT INTO t VALUES ";
    for(int i=0;i<100;++i){int ordinal=disorder&&i>=98?197-i:i;if(i)q+=",";q+="("+std::to_string(batch*100+ordinal)+",'"+std::string(3000,'x')+"','[1,2]')";}
    assert(e.execute(q).affected_rows==100);
   }
   writes[disorder]=e.data_page_write_bytes()-before;
  }
  assert(writes[1]==writes[0]);
 }
 // Strictly ascending input must use the original sequential writer without
 // batch keys/views, including full-width keys and a column permutation.
 {mvdb::Engine e(root/"ascending",1<<20);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(16))");
  auto ordered=e.ordered_pk_batch_count(),borrowed=e.borrowed_insert_batch_count();
  std::string q="INSERT INTO t(s,id) VALUES ";
  for(int i=0;i<100;++i){if(i)q+=",";q+="('x',"+std::to_string(-100+i)+")";}
  assert(e.execute(q).affected_rows==100);
  e.execute("INSERT INTO t VALUES(-9223372036854775808,'min'),(9007199254740993,'large'),(9223372036854775807,'max')");
  assert(e.ordered_pk_batch_count()==ordered&&e.borrowed_insert_batch_count()==borrowed);
  assert(e.active_rows("t")==103);
 }
 // A late inversion would flush more than eight pages if written speculatively.
 // Preflight must preserve input slots and the row preceding the statement in an
 // outer transaction; a subsequent duplicate and syntax error undo just SQL.
 {mvdb::Engine e(root/"late_inversion",1<<20);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(4096))");
  e.execute("BEGIN");e.execute("INSERT INTO t VALUES(10000,'prior')");
  std::string q="INSERT INTO t VALUES ";
  for(int i=0;i<100;++i){if(i)q+=",";q+="("+std::to_string(i==99?-1:i)+",'"+std::string(3000,'x')+"')";}
  assert(e.execute(q).affected_rows==100);assert(e.active_rows("t")==101);
  auto before=e.ordered_pk_batch_count();
  q="INSERT INTO t VALUES ";
  for(int i=0;i<100;++i){if(i)q+=",";q+="("+std::to_string(i==99?199:200+i)+",'"+std::string(3000,'y')+"')";}
  for(const auto& bad:{q+",(200,'duplicate')",q+",(400,'late') garbage"}){
   bool failed=false;try{e.execute(bad);}catch(const std::exception&){failed=true;}assert(failed&&e.active_rows("t")==101);
  }
  assert(e.ordered_pk_batch_count()==before);
  // Reuse every failed-statement key: no failed PK entry may survive.
  assert(e.execute(q).affected_rows==100&&e.active_rows("t")==201);
  e.execute("COMMIT");e.close();
  std::ifstream f(root/"late_inversion/data/t.rows",std::ios::binary);assert(f);
  for(int i=0;i<201;++i){int64_t key=0;f.read(reinterpret_cast<char*>(&key),8);f.seekg(24,std::ios::cur);assert(f&&key==(i==0?10000:i<100?i-1:i==100?-1:i<200?i+99:199));}
  mvdb::Engine reopened(root/"late_inversion",1<<20);
  assert(reopened.active_rows("t")==201);
  assert(ids(reopened,"SELECT id FROM t WHERE id=199 LIMIT 1")==std::vector<int64_t>{199});
 }
 {mvdb::Engine e(root/"wide",262144);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(100000))");
  assert(e.execute("INSERT INTO t VALUES(1,'"+std::string(14500,'x')+"')").affected_rows==1);
 }
 {mvdb::Engine e(root/"tiny",65536);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(16))");
  e.execute("INSERT INTO t VALUES(-3,'a'),(-2,'b'),(-1,'c')");
  e.execute("INSERT INTO t VALUES(3,'d'),(2,'e'),(1,'f')");
  assert(ids(e,"SELECT id FROM t ORDER BY id LIMIT 10")==std::vector<int64_t>({-3,-2,-1,1,2,3}));
 }
 borrowed_insert_boundary(root/"borrowed_128_4",128,false);
 borrowed_insert_boundary(root/"borrowed_129_4",129,false);
 borrowed_insert_boundary(root/"borrowed_128_5",128,true);
 std::vector<int64_t> input;
 {mvdb::Engine e(root/"pk",1<<20);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(16),v VECTOR(2));");
  for(int base=0;base<2500;base+=100){std::string q="INSERT INTO t VALUES ";for(int i=base;i<base+100;++i){if(i!=base)q+=",";q+="("+std::to_string(i*4)+",'old','[0,0]')";}e.execute(q);}
  auto batches=e.ordered_pk_batch_count();auto borrowed=e.borrowed_insert_batch_count();std::string q="INSERT INTO t VALUES ";
  for(int i=0;i<100;++i){auto key=int64_t(((i*37)%100)*100+1);input.push_back(key);if(i)q+=",";q+="("+std::to_string(key)+",'new','[1,1]')";}
  assert(e.execute(q).affected_rows==100);assert(e.ordered_pk_batch_count()==batches+1);
  assert(e.borrowed_insert_batch_count()==borrowed+1);
  e.execute("BEGIN;");e.execute("INSERT INTO t VALUES(20001,'prior','[2,2]')");
  for(const auto& bad:{"INSERT INTO t VALUES(20002,'x','[2,2]'),(20002,'y','[3,3]')", "INSERT INTO t VALUES(20002,'x','[2,2]'),(20003,'exceeds_sixteen_characters','[3,3]')", "INSERT INTO t VALUES(20002,'x','[2,2]'),(0,'y','[3,3]')"}){
   bool failed=false;try{e.execute(bad);}catch(const std::exception&){failed=true;}assert(failed);assert(e.active_rows("t")==2601);assert(ids(e,"SELECT id FROM t WHERE id>=20000 ORDER BY id LIMIT 10")==std::vector<int64_t>{20001});
  }
  e.execute("ROLLBACK;");assert(e.active_rows("t")==2600);e.close();
 }
 // Sorted PK access must not reorder physical row slots.
 {std::ifstream f(root/"pk/data/t.rows",std::ios::binary);assert(f);for(size_t i=0;i<input.size();++i){int64_t key=0;f.seekg((2500+i)*32);f.read(reinterpret_cast<char*>(&key),8);assert(key==input[i]);}}
 // Force a stored signature collision: this simulates equal hash outputs,
 // without pretending that a shared string prefix is a 64-bit FNV collision.
 {mvdb::Engine e(root/"hash",1<<20);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(64),v VECTOR(2));");std::string q="INSERT INTO t VALUES ";for(int i=0;i<200;++i){if(i)q+=",";q+="("+std::to_string(i)+",'common-prefix-"+std::to_string(i)+"','[0,0]')";}e.execute(q);e.close();}
 {std::fstream f(root/"hash/data/t.s1",std::ios::binary|std::ios::in|std::ios::out);assert(f);auto h=signature("common-prefix-17");f.seekp(18*8);f.write(reinterpret_cast<const char*>(&h),8);assert(f);}
 {mvdb::Engine e(root/"hash",1<<20);auto before=e.signature_rejected_rows();
  auto result=e.execute("UPDATE t SET v='[1,1]' WHERE s='common-prefix-17'");assert(result.affected_rows==1);assert(e.signature_rejected_rows()-before==198);
  assert(ids(e,"SELECT id FROM t WHERE s='common-prefix-17' ORDER BY id LIMIT 10")==std::vector<int64_t>{17});
  assert(e.execute("DELETE FROM t WHERE s='common-prefix-17'").affected_rows==1);
  assert(ids(e,"SELECT id FROM t WHERE id=18 ORDER BY id LIMIT 10")==std::vector<int64_t>{18});
  assert(e.execute("UPDATE t SET s='changed' WHERE s='absent'").affected_rows==0);e.close();
 }
 std::cout<<"r12 ordered PK/input slots, late-failure savepoints and forced signature collision passed\n";
}
