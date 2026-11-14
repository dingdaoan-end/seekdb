#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/engine.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using mvdb::Engine;
using Ranked=std::vector<std::pair<int64_t,double>>;
using Snapshot=std::vector<std::pair<int64_t,std::vector<float>>>;
std::string radial_vector(int dim,const std::string& first="0",const std::string& second="0",bool fill=false){
  std::string out="["+first;
  for(int j=1;j<dim;++j)out+=","+(fill?first:j==1?second:std::string("0"));
  return out+"]";
}
Ranked radial_rank(Engine& e,const std::string& q,bool generic,uint32_t limit=29,const std::string& filter=""){
  Ranked out;
  e.execute("SELECT id,L2_DISTANCE(v,'"+q+"') AS d"+(generic?",id AS again":"")+
            " FROM t"+filter+" ORDER BY d,id LIMIT "+std::to_string(limit)+";").each([&](const auto& row){
    out.emplace_back(std::get<int64_t>(row[0]),std::get<double>(row[1]));
  });
  return out;
}
Snapshot radial_snapshot(Engine& e){
  Snapshot out;e.execute("SELECT id,v FROM t ORDER BY id;").each([&](const auto& row){
    out.emplace_back(std::get<int64_t>(row[0]),std::get<std::vector<float>>(row[1]));
  });return out;
}
void radial_agree(Engine& e,const std::vector<std::string>& queries,uint32_t limit=29){
  // Identical probes on each side: the third projection invokes generic SQL.
  // Compare displayed double distances as well as the complete PK ordering.
  for(const int probes:{0,3,64}){
    e.execute(probes?"SET search_mode=ann;":"SET search_mode=exact;");
    if(probes)e.execute("SET nprobe="+std::to_string(probes)+";");
    for(const auto& q:queries){
      const auto reference=radial_rank(e,q,true,limit);
      assert(radial_rank(e,q,false,limit)==reference);
      assert(radial_rank(e,q,false,limit)==reference);
    }
  }
}
void radial_resident(const std::filesystem::path& path,int dim,size_t budget){
  Engine e(path,budget);const auto zero=radial_vector(dim),one=radial_vector(dim,"1");
  const auto huge=radial_vector(dim,"3e38","0",true);
  const std::vector<std::string> near_queries={zero,one,radial_vector(dim,"1","1.5e-8"),
    radial_vector(dim,"1.00000011920928955078125"),radial_vector(dim,"2.0000002384185791015625"),radial_vector(dim,"-1","-1e-30")};
  e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(8),v VECTOR("+std::to_string(dim)+"));");
  e.execute("BEGIN;");
  for(int start=0;start<353;start+=50){
    std::string sql="INSERT INTO t VALUES ";
    for(int i=start;i<std::min(start+50,353);++i){
      if(i!=start)sql+=",";
      auto vector=i==0?radial_vector(dim,"1","1.5e-8"):i==1?one:i==2?zero:
        i==4?radial_vector(dim,"1e-40"):i==5?radial_vector(dim,"1e-30"):
        radial_vector(dim,std::to_string(2+i%13));
      sql+="("+std::to_string(i-500)+",'"+(i%3?"other":"same")+"','"+vector+"')";
    }e.execute(sql+";");
  }
  e.execute("COMMIT;");e.execute("CREATE VECTOR INDEX vi ON t(v);");
  // Sparse page boundaries and an active partial tail remain in every scan.
  e.execute("DELETE FROM t WHERE id=-469;");e.execute("DELETE FROM t WHERE id=-437;");e.execute("DELETE FROM t WHERE id=-149;");
  radial_agree(e,near_queries);
  e.execute("SET search_mode=exact;");
  const auto tied=radial_rank(e,zero,false);auto a=std::find_if(tied.begin(),tied.end(),[](const auto& row){return row.first==-500;});
  auto b=std::find_if(tied.begin(),tied.end(),[](const auto& row){return row.first==-499;});
  assert(a!=tied.end()&&b!=tied.end()&&a<b&&a->second==1&&b->second==1);

  // Warm a radius cache, change its row count and vectors, and check results
  // immediately after every invalidating operation.
  e.execute("INSERT INTO t VALUES (-90000,'same','"+zero+"');");
  radial_agree(e,{zero});e.execute("SET search_mode=exact;");assert(radial_rank(e,zero,false).front().first==-90000);
  e.execute("UPDATE t SET v='"+huge+"' WHERE id=-490;");
  radial_agree(e,{zero,huge});
  e.execute("DELETE FROM t WHERE id=-90000;");radial_agree(e,{zero,huge});
  const auto before_failure=radial_snapshot(e);
  bool failed=false;
  try{e.execute("INSERT INTO t VALUES (-90001,'same','"+zero+"'),(-500,'same','"+zero+"');");}
  catch(const std::exception&){failed=true;}
  assert(failed&&radial_snapshot(e)==before_failure);radial_agree(e,{zero,huge});
  const auto before_tx=radial_snapshot(e);
  e.execute("BEGIN;");e.execute("INSERT INTO t VALUES (-90002,'same','"+zero+"');");
  e.execute("UPDATE t SET v='"+radial_vector(dim,"-2")+"' WHERE id=-499;");
  e.execute("DELETE FROM t WHERE id=-498;");radial_agree(e,{zero,huge,radial_vector(dim,"-2")});
  e.execute("ROLLBACK;");assert(radial_snapshot(e)==before_tx);
  radial_agree(e,near_queries);radial_agree(e,{huge});
  // Equality filters also preserve radius association after dictionary rebuild.
  e.execute("SET search_mode=exact;");assert(radial_rank(e,zero,false,29," WHERE category='same'")==radial_rank(e,zero,true,29," WHERE category='same'"));
  assert(e.budget_peak()<=budget);
}
void radial_overflow(const std::filesystem::path& path){
  constexpr int dim=129;constexpr size_t budget=2*1024*1024;
  Engine e(path,budget);const auto huge=radial_vector(dim,"3e38","0",true);
  e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(129));");e.execute("BEGIN;");
  for(int start=0;start<353;start+=50){std::string sql="INSERT INTO t VALUES ";
    for(int i=start;i<std::min(start+50,353);++i){if(i!=start)sql+=",";sql+="("+std::to_string(i-500)+",'"+huge+"')";}
    e.execute(sql+";");}
  e.execute("COMMIT;");e.execute("CREATE VECTOR INDEX vi ON t(v);");
  // The centroid's mathematical norm exceeds FLT_MAX, despite every component
  // being finite. Overflowed cached bounds must never remove these candidates.
  radial_agree(e,{huge,radial_vector(dim),radial_vector(dim,"-3e38","0",true)});
  // Centers remain huge after DML. In that same bucket the unchanged rows
  // have finite zero radii, while these tail rows have overflowing float
  // radii. A finite-range stop must still examine the overflow tail; the
  // zero query's distance from its centroid also exceeds FLT_MAX.
  const auto zero=radial_vector(dim);
  e.execute("UPDATE t SET v='"+zero+"' WHERE id>=-179;");
  radial_agree(e,{zero,huge,radial_vector(dim,"1")});
  e.execute("SET search_mode=exact;");const auto nearest=radial_rank(e,zero,false);
  assert(nearest.size()==29&&nearest.front().first==-179&&nearest.back().first==-151);
  for(const auto& row:nearest)assert(row.second==0);
  assert(e.budget_peak()<=budget);
}
void radial_paged(const std::filesystem::path& path){
  constexpr int dim=128;constexpr int count=5000;constexpr size_t budget=4*1024*1024;
  Engine e(path,budget);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(128));");e.execute("BEGIN;");
  for(int start=0;start<count;start+=100){std::string sql="INSERT INTO t VALUES ";
    for(int i=start;i<std::min(start+100,count);++i){if(i!=start)sql+=",";
      const auto x=i<64?1.0+(i%11)*.001:1000.0*(1+i%3)+(i%37);
      sql+="("+std::to_string(i-2500)+",'"+radial_vector(dim,std::to_string(x))+"')";
    }e.execute(sql+";");}
  e.execute("COMMIT;");e.execute("CREATE VECTOR INDEX vi ON t(v);");
  assert(uint64_t(count)*dim*sizeof(float)>e.frame_capacity_bytes());assert(e.lifetime_evictions()>0);
  const auto zero=radial_vector(dim);e.execute("SET search_mode=exact;");
  const auto exact=radial_rank(e,zero,true,10);
  // Generic SQL scans first, so the near rows at the beginning are cold.
  // Fast candidates occur in physical order: establish top10, then discard
  // distant shells while their payload pages are still absent from the pool.
  e.reset_pruning_counters();assert(radial_rank(e,zero,false,10)==exact);
  const auto proof=e.radial_counters();
  assert(proof.tested>0&&proof.rejected>0);
  assert(proof.late_rejected>0||proof.pages_skipped>0);
  radial_agree(e,{zero,radial_vector(dim,"1.00000011920928955078125"),radial_vector(dim,"1000.00006103515625")},10);
  e.reset_pruning_counters();const auto reset=e.radial_counters();
  assert(reset.tested==0&&reset.rejected==0&&reset.late_rejected==0&&reset.pages_skipped==0);
  assert(e.budget_peak()<=budget);
  std::cout<<"{\"radial_paged\":{\"rows\":"<<count<<",\"dim\":"<<dim
           <<",\"budget\":"<<budget<<",\"tested\":"<<proof.tested
           <<",\"rejected\":"<<proof.rejected<<",\"late_rejected\":"<<proof.late_rejected
           <<",\"pages_skipped\":"<<proof.pages_skipped<<"}}\n";
}
}

void radial_cache_regressions(){
  const auto root=std::filesystem::path("/tmp")/("mvdb_radial_regression_"+std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  for(const size_t budget:{size_t(2*1024*1024),size_t(10*1024*1024)})
    for(const int dim:{32,64,127,128,129})radial_resident(root/(std::to_string(budget)+"_"+std::to_string(dim)),dim,budget);
  radial_overflow(root/"overflow");radial_paged(root/"paged");
  std::filesystem::remove_all(root);
}
