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
  std::cout<<"result lifetime regression passed\n";
}
