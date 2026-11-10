#include "../src/engine.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <iostream>
int main(){
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
  std::cout<<"result lifetime regression passed\n";
}
