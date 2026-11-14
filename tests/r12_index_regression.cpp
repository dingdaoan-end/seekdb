#include "../src/engine.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <fstream>
void r12_index_regressions(){
 const std::filesystem::path path="/tmp/mvdb_r12_index";std::filesystem::remove_all(path);
 mvdb::Engine e(path,1<<20);e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2))");
 std::string sql="INSERT INTO t VALUES ";for(int i=0;i<40;++i){if(i)sql+=",";int x=i%5==0?0:i;sql+="("+std::to_string(i)+",'["+std::to_string(x)+","+std::to_string(x*3)+"]')";}e.execute(sql);
 e.execute("CREATE VECTOR INDEX vi ON t(v)");assert(e.empty_cluster_reseed_count()>0);
 auto query=[&](const std::string& mode){e.execute("SET search_mode="+mode);std::vector<std::pair<int64_t,double>> out;e.execute("SELECT id,L2_DISTANCE(v,'[13,39]') AS d FROM t ORDER BY d,id LIMIT 10").each([&](const auto& r){out.emplace_back(std::get<int64_t>(r[0]),std::get<double>(r[1]));});return out;};
 const auto expected=query("exact");e.execute("SET nprobe=8");assert(query("ann")==expected);
 // Keep the declared L2 SQL contract: unsupported metrics are errors, not
 // silently interpreted using Euclidean centroids/calibration.
 for(const auto& metric:{"COSINE_DISTANCE","INNER_PRODUCT"}){bool failed=false;try{e.execute(std::string("SELECT id,")+metric+"(v,'[13,39]') AS d FROM t ORDER BY d LIMIT 10");}catch(const std::exception&){failed=true;}assert(failed);}
 e.execute("CREATE TABLE z(id BIGINT PRIMARY KEY,v VECTOR(2))");e.execute("INSERT INTO z VALUES(1,'[0,0]'),(2,'[0,0]'),(3,'[0,0]')");e.execute("CREATE VECTOR INDEX zi ON z(v)");
 size_t rows=0;e.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM z ORDER BY d LIMIT 10").each([&](const auto& r){assert(std::get<double>(r[1])==0);++rows;});assert(rows==3);
 std::cout<<"r12 deterministic empty-centroid reseeds="<<e.empty_cluster_reseed_count()<<", small-N/zero vectors and unsupported metric guards passed\n";
 std::vector<char> reference_centers;
 for(int run=0;run<2;++run){auto root=path/("narrow"+std::to_string(run));
  {mvdb::Engine narrow(root,196608);narrow.execute("CREATE TABLE n(id BIGINT PRIMARY KEY,v VECTOR(1024))");
   for(int i=0;i<40;++i){int x=i%5==0?0:i;std::string v="["+std::to_string(x);for(int j=1;j<1024;++j)v+=",0";v+="]";narrow.execute("INSERT INTO n VALUES("+std::to_string(i)+",'"+v+"')");}
   narrow.execute("CREATE VECTOR INDEX ni ON n(v)");assert(narrow.empty_cluster_reseed_count()>0);assert(narrow.budget_peak()<=196608);narrow.close();
  }
  std::vector<char> centers;for(const auto& f:std::filesystem::directory_iterator(root/"data"))if(f.path().extension()==".centers"){std::ifstream in(f.path(),std::ios::binary);centers.assign(std::istreambuf_iterator<char>(in),{});}
  assert(!centers.empty());if(run==0)reference_centers=centers;else assert(centers==reference_centers);
  {mvdb::Engine reopened(root,196608);assert(reopened.active_rows("n")==40);assert(reopened.recommended_nprobe("n")>0);}
 }
}
