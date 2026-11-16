#include "../src/engine.hpp"
#include <cassert>
#include <filesystem>
#include <vector>
#include <string>
#include <iostream>

namespace {
std::string vector128(const std::string& first){std::string v="'["+first;for(int j=1;j<128;++j)v+=",0";return v+"]'";}
using Ranking=std::vector<std::pair<int64_t,double>>;
Ranking ranked(mvdb::Engine& e,const std::string& table,bool generic=false){
  Ranking out;e.execute("SELECT id,L2_DISTANCE(v,"+vector128("0")+") AS d"+(generic?",id AS again":"")+" FROM "+table+" ORDER BY d,id LIMIT 40;").each([&](const auto& r){out.emplace_back(std::get<int64_t>(r[0]),std::get<double>(r[1]));});return out;
}
}
void r12_query_regressions(){
  const auto root=std::filesystem::path("/tmp/mvdb_r12_query");std::filesystem::remove_all(root);
  for(size_t budget:{size_t(1<<20),size_t(4<<20)}){
    mvdb::Engine e(root/std::to_string(budget),budget);
    for(int n:{31,32,33,97}){
      auto t="t"+std::to_string(n);e.execute("CREATE TABLE "+t+" (id BIGINT PRIMARY KEY,category VARCHAR(8),v VECTOR(128));");
      std::string sql="INSERT INTO "+t+" VALUES ";for(int i=0;i<n;++i){if(i)sql+=",";sql+="("+std::to_string(n-i)+",'x',"+vector128(i%9==0?"3e38":i%7==0?"1e-30":std::to_string(i%5))+")";}e.execute(sql+";");
      e.execute("CREATE VECTOR INDEX ix ON "+t+"(v);");
      assert(e.read_cache_slots()==0);
      auto builds=e.read_cache_build_count();
      auto first=ranked(e,t);assert(e.read_cache_build_count()==builds+1);assert(first==ranked(e,t));assert(first==ranked(e,t,true));
      e.execute("BEGIN;");e.execute("DELETE FROM "+t+" WHERE id=2;");assert(e.read_cache_slots()==0);
      auto holes=ranked(e,t);assert(holes==ranked(e,t));assert(holes==ranked(e,t,true));
      e.execute("ROLLBACK;");assert(e.read_cache_slots()==0);assert(ranked(e,t)==first);
    }
    e.execute("CREATE TABLE tx (id BIGINT PRIMARY KEY,v VECTOR(128));");e.execute("INSERT INTO tx VALUES (1,"+vector128("0")+");");
    auto builds=e.read_cache_build_count();e.execute("BEGIN;");e.execute("CREATE VECTOR INDEX txix ON tx(v);");assert(e.read_cache_slots()==0);assert(e.read_cache_build_count()==builds);
    e.execute("COMMIT;");assert(e.read_cache_slots()==0&&e.read_cache_build_count()==builds);ranked(e,"tx");assert(e.read_cache_slots()==1);
    e.execute("UPDATE tx SET v="+vector128("1")+" WHERE id=1;");assert(e.read_cache_slots()==0);assert(ranked(e,"tx")==ranked(e,"tx",true));
    e.execute("CREATE TABLE abort_index (id BIGINT PRIMARY KEY,v VECTOR(128));");e.execute("INSERT INTO abort_index VALUES (1,"+vector128("0")+");");
    builds=e.read_cache_build_count();e.execute("BEGIN;");e.execute("CREATE VECTOR INDEX rollbackix ON abort_index(v);");e.execute("ROLLBACK;");
    assert(e.read_cache_slots()==0&&e.read_cache_build_count()==builds);e.execute("BEGIN;");e.execute("COMMIT;");assert(e.read_cache_build_count()==builds);
    assert(e.budget_peak()<=budget);
    std::cout<<"r12_query budget="<<budget<<" builds="<<e.read_cache_build_count()<<" peak="<<e.budget_peak()<<"\n";
  }
  {mvdb::Engine e(root/"small",65536);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(8),v VECTOR(128));");e.execute("INSERT INTO t VALUES (1,'x',"+vector128("0")+");");
    e.execute("CREATE VECTOR INDEX ix ON t(v);");assert(e.active_rows("t")==1&&e.read_cache_slots()==0);assert(e.budget_peak()<=65536);}
  std::filesystem::remove_all(root);
}
