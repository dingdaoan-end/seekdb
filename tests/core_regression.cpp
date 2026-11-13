#include "../src/engine.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <iostream>
void sql_stream_regressions();
int main(){
  sql_stream_regressions();
  const std::filesystem::path root="/tmp/mvdb_core_phase2";
  std::filesystem::remove_all(root);
  mvdb::Result result;
  {mvdb::Engine engine(root,65536);
   engine.execute("CREATE TABLE t (id BIGINT PRIMARY KEY);");
   engine.execute("INSERT INTO t VALUES (7);");
   result=engine.execute("SELECT id FROM t ORDER BY id LIMIT 1;");}
  size_t count=0;result.each([&](const auto& row){assert(std::get<int64_t>(row[0])==7);++count;});assert(count==1);
  result={};
  {mvdb::Engine engine(root,65536);
   engine.execute("CREATE TABLE implicit_key (id INT, embedding VECTOR(3));");
   engine.execute("INSERT INTO implicit_key VALUES (2, '[1,2,3]');");
   auto projected=engine.execute(" SELECT id, embedding FROM implicit_key;");
   size_t rows=0;projected.each([&](const auto& row){assert(std::get<int64_t>(row[0])==2);assert(mvdb::json(row[1])=="[1,2,3]");++rows;});assert(rows==1);
   bool duplicate=false;try{engine.execute("INSERT INTO implicit_key VALUES (2, '[0,0,0]');");}catch(const std::exception&){duplicate=true;}assert(duplicate);
   for(const auto& sql:{"CREATE TABLE ambiguous (a INT,b BIGINT);", "CREATE TABLE two_keys (a INT PRIMARY KEY,b BIGINT PRIMARY KEY);", "CREATE TABLE two_vectors (id INT,v VECTOR(2),w VECTOR(2));"}){
     bool rejected=false;try{engine.execute(sql);}catch(const std::exception&){rejected=true;}assert(rejected);
   }
   engine.execute("CREATE VECTOR INDEX implicit_vec ON implicit_key(embedding);");
   auto mutations=engine.mutation_count("implicit_key");
   engine.execute("BEGIN;");
   engine.execute("DELETE FROM implicit_key WHERE id=2;");
   assert(engine.active_rows("implicit_key")==0);
   assert(engine.mutation_count("implicit_key")==mutations+1);
   engine.close();engine.close();
  }
  {mvdb::Engine engine(root,65536);
   assert(engine.active_rows("implicit_key")==1);
   assert(engine.mutation_count("implicit_key")==0);
   auto projected=engine.execute("SELECT id, embedding FROM implicit_key;");
   size_t rows=0;projected.each([&](const auto& row){assert(std::get<int64_t>(row[0])==2);assert(mvdb::json(row[1])=="[1,2,3]");++rows;});assert(rows==1);
   engine.execute("BEGIN;");engine.execute("UPDATE implicit_key SET embedding='[9,9,9]' WHERE id=2;");
   // EOF uses the same destructor path without an explicit close request.
  }
  {mvdb::Engine engine(root,65536);
   auto projected=engine.execute("SELECT embedding FROM implicit_key;");
   size_t rows=0;projected.each([&](const auto& row){assert(mvdb::json(row[0])=="[1,2,3]");++rows;});assert(rows==1);
   assert(engine.active_rows("implicit_key")==1&&engine.mutation_count("implicit_key")==0);
  }
  // A warmed read cache must not survive writes, transaction boundaries, or
  // a failed statement. Both exact dictionaries and PK tie ordering matter.
  {mvdb::Engine e(root/"read_cache",1<<20);
   e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(2));");
   e.execute("INSERT INTO t VALUES (7,'\xe4\xb8\xad\xe6\x96\x87','[0,0]'),(2,'\xe4\xb8\xad\xe6\x96\x87','[0,0]'),(9,'other','[2,0]');");
   const std::string q="SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE category='\xe4\xb8\xad\xe6\x96\x87' ORDER BY d LIMIT 2;";
   auto ids=[&](){std::vector<int64_t> out;e.execute(q).each([&](const auto& r){out.push_back(std::get<int64_t>(r[0]));});return out;};
   assert((ids()==std::vector<int64_t>{2,7}));assert(e.read_cache_slots()==3);assert(e.last_route()=="exact_filter");
   size_t missing=0;auto reads=e.vector_page_reads();e.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE category='absent' ORDER BY d LIMIT 2;").each([&](const auto&){++missing;});assert(missing==0&&e.last_route()=="exact_filter"&&e.vector_page_reads()==reads);
   e.execute("BEGIN;");assert(e.read_cache_slots()==0);
   e.execute("UPDATE t SET category='other' WHERE id=2;");assert((ids()==std::vector<int64_t>{7}));
   e.execute("ROLLBACK;");assert(e.read_cache_slots()==0);assert((ids()==std::vector<int64_t>{2,7}));
   bool failed=false;try{e.execute("INSERT INTO t VALUES (10,'\xe4\xb8\xad\xe6\x96\x87','[0,0]'),(2,'\xe4\xb8\xad\xe6\x96\x87','[0,0]');");}catch(const std::exception&){failed=true;}
   assert(failed&&e.read_cache_slots()==0);assert((ids()==std::vector<int64_t>{2,7}));
   e.execute("BEGIN;");e.execute("DELETE FROM t WHERE id=2;");assert((ids()==std::vector<int64_t>{7}));e.execute("COMMIT;");assert(e.read_cache_slots()==0);assert((ids()==std::vector<int64_t>{7}));
   e.execute("DELETE FROM t WHERE id=7;");assert(ids().empty()&&e.last_route()=="exact_filter");
   assert(e.budget_peak()<=(1U<<20));
  }
  {mvdb::Engine e(root/"cache_fallback",1<<20);
   e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(40),v VECTOR(2));");
   e.execute("BEGIN;");for(int i=0;i<140;++i)e.execute("INSERT INTO t VALUES ("+std::to_string(i)+",'unique"+std::to_string(i)+"','["+std::to_string(i)+",0]');");e.execute("COMMIT;");
   auto r=e.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE category='unique139' ORDER BY d LIMIT 1;");
   size_t count=0;r.each([&](const auto& values){assert(std::get<int64_t>(values[0])==139);++count;});assert(count==1&&e.read_cache_slots()==0);
   e.execute("CREATE TABLE extremes (id BIGINT PRIMARY KEY,v VECTOR(2));");
   e.execute("INSERT INTO extremes VALUES (9007199254740993,'[3e38,3e38]'),(9007199254740992,'[3e38,3e38]'),(3,'[1e-30,0]'),(2,'[0,0]');");
   auto ranked=[&](bool generic){std::vector<std::pair<int64_t,double>> out;e.execute(std::string("SELECT id,L2_DISTANCE(v,'[0,0]') AS d")+(generic?",id AS again":"")+" FROM extremes ORDER BY d LIMIT 4;").each([&](const auto& values){out.emplace_back(std::get<int64_t>(values[0]),std::get<double>(values[1]));});return out;};
   auto fast=ranked(false);assert(fast==ranked(true));assert(fast[2].first==9007199254740992LL&&fast[3].first==9007199254740993LL);
   e.execute("CREATE TABLE rounded (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(2));");
   e.execute("INSERT INTO rounded VALUES (1,'same','[1,1.5e-8]'),(2,'same','[1,0]');");
   std::vector<int64_t> tied;e.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM rounded WHERE category='same' ORDER BY d LIMIT 2;").each([&](const auto& r){assert(std::get<double>(r[1])==1.0);tied.push_back(std::get<int64_t>(r[0]));});assert((tied==std::vector<int64_t>{1,2}));
   size_t none=0;e.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM extremes WHERE id=2 AND id=3 ORDER BY d LIMIT 4;").each([&](const auto&){++none;});assert(none==0);
   assert(e.budget_peak()<=(1U<<20));
  }
  // The measured workload specifies an explicit ascending PK tiebreaker.
  // Both the projected alias and the underlying column must use the cache.
  {mvdb::Engine e(root/"cache_order_shape",12<<20);
   e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),content VARCHAR(128),v VECTOR(128));");
   std::string zero="[";for(int i=0;i<128;++i){if(i)zero+=",";zero+="0";}zero+="]";
   e.execute("BEGIN;");for(int i=139;i>=0;--i)e.execute("INSERT INTO t VALUES ("+std::to_string(i)+",'same','unique"+std::to_string(i)+"','"+zero+"');");e.execute("COMMIT;");
   for(const auto& suffix:{"id","pk"}){
    std::vector<int64_t> ids;e.execute("SELECT id AS pk,L2_DISTANCE(v,'"+zero+"') AS d FROM t WHERE category='same' ORDER BY d,"+suffix+" LIMIT 10;").each([&](const auto& r){ids.push_back(std::get<int64_t>(r[0]));});
    assert(ids.size()==10&&ids.front()==0&&ids.back()==9);assert(e.read_cache_slots()==140);
   }
   std::vector<int64_t> descending;e.execute("SELECT id,L2_DISTANCE(v,'"+zero+"') AS d FROM t ORDER BY d,id DESC LIMIT 10;").each([&](const auto& r){descending.push_back(std::get<int64_t>(r[0]));});
   assert(descending.front()==139&&descending.back()==130&&e.read_cache_slots()==0);
  }
  // Resident four-candidate scans agree with generic SQL through a full scan,
  // dictionary range and repeated equality filters. Deleted slots, tails,
  // near ties, extreme floats and underflow all participate in the ordering.
  for(int dim:{127,128,129}){mvdb::Engine e(root/("batch4_"+std::to_string(dim)),2<<20);
   e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR("+std::to_string(dim)+"));");
   std::string zero="[";for(int j=0;j<dim;++j){if(j)zero+=",";zero+="0";}zero+="]";
   e.execute("BEGIN;");
   for(int i=0;i<83;++i){std::string v="[";for(int j=0;j<dim;++j){if(j)v+=",";v+=(j==0?(i%11==0?"3e38":i%7==0?"1e-30":std::to_string(i%5)):j==dim-1?"1e-30":"0");}v+="]";
    e.execute("INSERT INTO t VALUES ("+std::to_string(100-i)+",'"+(i%3?"same":"other")+"','"+v+"');");}
   e.execute("COMMIT;");e.execute("DELETE FROM t WHERE id=60;");
   for(const auto& filter:{"", " WHERE category='same'", " WHERE category='same' AND category='same'"}){
    auto ranked=[&](bool generic){std::vector<int64_t> ids;e.execute("SELECT id,L2_DISTANCE(v,'"+zero+"') AS d"+(generic?",id AS again":"")+" FROM t"+filter+" ORDER BY d,id LIMIT 29;").each([&](const auto& r){ids.push_back(std::get<int64_t>(r[0]));});return ids;};
    auto expected=ranked(true);for(int pass=0;pass<3;++pass)assert(ranked(false)==expected);
   }
   assert(e.budget_peak()<=(2U<<20));
  }
  // A failed streamed batch can leave unreachable append-only Meta bytes.
  // Reopen with too little workspace for batching, then reuse that slot through
  // write_row: invisible bytes must not be treated as an active old row.
  {auto path=root/"failed_append_reuse";std::string zero="[";for(int j=0;j<128;++j){if(j)zero+=",";zero+="0";}zero+="]";
   {mvdb::Engine e(path,1<<20);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(128));");e.execute("INSERT INTO t VALUES (1,'old','"+zero+"');");e.execute("CREATE VECTOR INDEX vi ON t(v);");
    std::string sql="INSERT INTO t VALUES ";for(int i=0;i<400;++i){if(i)sql+=",";sql+="("+std::to_string(100+i)+",'failed','"+zero+"')";}bool failed=false;try{e.execute(sql+" trailing;");}catch(const std::exception&){failed=true;}assert(failed&&e.active_rows("t")==1);e.close();}
   {mvdb::Engine e(path,65536);auto one=zero;one[1]='1';e.execute("INSERT INTO t VALUES (9999,'new','"+one+"');");assert(e.active_rows("t")==2);assert(e.mutation_count("t")==1);size_t rows=0;e.execute("SELECT id,v FROM t WHERE category='new' ORDER BY id LIMIT 1;").each([&](const auto& row){assert(std::get<int64_t>(row[0])==9999);auto& v=std::get<std::vector<float>>(row[1]);assert(v.size()==128&&v[0]==1&&v[1]==0);++rows;});assert(rows==1);e.execute("DELETE FROM t WHERE id=9999;");assert(e.active_rows("t")==1);e.close();}
  }
  // More vector pages than frames: warming some handles must never make a
  // later miss reuse a stale borrowed address captured for an earlier page.
  {mvdb::Engine e(root/"cache_mixed_resident",1<<20);
   e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(128));");
   e.execute("BEGIN;");
   std::string zero="[";for(int j=0;j<128;++j){if(j)zero+=",";zero+="0";}zero+="]";
   for(int start=0;start<1100;start+=100){std::string sql="INSERT INTO t VALUES ";for(int i=start;i<start+100;++i){if(i!=start)sql+=",";std::string v="["+std::to_string(i%17);for(int j=1;j<128;++j)v+=",0";v+="]";sql+="("+std::to_string(2000-i)+",'"+(i%2?"other":"same")+"','"+v+"')";}e.execute(sql+";");}
   e.execute("COMMIT;");
   auto ranked=[&](bool generic){std::vector<int64_t> out;e.execute("SELECT id,L2_DISTANCE(v,'"+zero+"') AS d"+(generic?",id AS again":"")+" FROM t WHERE category='same' ORDER BY d,id LIMIT 20;").each([&](const auto& values){out.push_back(std::get<int64_t>(values[0]));});return out;};
   auto expected=ranked(true);for(int pass=0;pass<3;++pass){assert(ranked(false)==expected);assert(e.read_cache_slots()==1100&&e.last_route()=="exact_filter");}
   e.execute("BEGIN;");e.execute("INSERT INTO t VALUES (0,'same','"+zero+"');");
   bool failed=false;try{e.execute("INSERT INTO t VALUES (-1,'same','"+zero+"'),(-2,'same','"+zero+"') trailing;");}catch(const std::exception&){failed=true;}assert(failed);
   assert(e.active_rows("t")==1101);e.execute("ROLLBACK;");assert(ranked(false)==expected);assert(e.budget_peak()<=(1U<<20));
  }
  std::cout<<"result lifetime regression passed\n";
}
