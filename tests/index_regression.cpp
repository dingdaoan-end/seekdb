#include "../src/engine.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <iostream>
#include <set>
#include <unistd.h>
using namespace mvdb;
static std::vector<int64_t> ids(Engine& e,const std::string& q){std::vector<int64_t> out;e.execute(q).each([&](const auto& r){out.push_back(std::get<int64_t>(r[0]));});return out;}
static std::string query(const std::string& vector,const std::string& filter="") {return "SELECT id,L2_distance(v,'"+vector+"') AS d FROM t "+filter+" ORDER BY d,id LIMIT 10;";}
int main(){
 // Keep both strict boundaries: 25,000 active rows still calibrate regardless
 // of payload, and a larger table skips only above half its configured budget.
 assert(!detail::skip_ivf_calibration_for_payload(25000,12800000,1048576));
 assert(detail::skip_ivf_calibration_for_payload(25001,12800512,1048576));
 assert(!detail::skip_ivf_calibration_for_payload(25001,524288,1048576));
 assert(detail::skip_ivf_calibration_for_payload(25001,524289,1048576));
 const std::filesystem::path root="/tmp/mvdb_index_phase3_"+std::to_string(::getpid());std::filesystem::remove_all(root);
 uint32_t recommendation=0;
 {
  Engine e(root,1<<20);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(2));");
  e.execute("BEGIN;");
  for(int start=0;start<4200;start+=100){std::string sql="INSERT INTO t VALUES ";for(int i=start;i<std::min(start+100,4200);++i){if(i!=start)sql+=",";auto x=(i%64)*1000+(i/64)*.001;sql+="("+std::to_string(i)+",'"+(i%2?"red":"blue")+"','["+std::to_string(x)+","+std::to_string(x)+"]')";}e.execute(sql+";");}e.execute("COMMIT;");
  auto undo=e.undo_spill_bytes();e.execute("CREATE VECTOR INDEX vi ON t(v);");assert(e.undo_spill_bytes()-undo<8192);
  recommendation=e.recommended_nprobe("t");assert(recommendation>=1&&recommendation<=32);assert(e.active_rows("t")==4200);
  assert(!e.calibration_skipped("t")&&e.calibration_query_count("t")==16);
  assert(e.mutation_count("t")==0&&e.rows_at_calibration("t")==4200&&e.drift_threshold("t")==840);
  e.execute("SET search_mode=ann;");auto q=query("[17000.0215,17000.0195]");auto automatic=ids(e,q);assert(e.effective_nprobe()==(e.last_route()=="ivf"?recommendation:0));
  e.execute("SET nprobe=64;");auto all=ids(e,q);assert(e.last_route()=="ivf"&&e.effective_nprobe()==64);
  assert(e.read_cache_slots()==4200);assert(ids(e,q)==all);assert(ids(e,q)==all);
  e.execute("SET search_mode=exact;");assert(ids(e,q)==all);
  e.execute("SET search_mode=ann;");e.execute("BEGIN;");e.execute("UPDATE t SET v='[999999,999999]' WHERE id=17;");e.execute("DELETE FROM t WHERE id=81;");e.execute("INSERT INTO t VALUES (99999,'red','[17000,17000]');");assert(!e.calibration_stale("t"));assert(e.mutation_count("t")==3&&e.active_rows("t")==4200);e.execute("SET nprobe=0;");auto changed=ids(e,q);assert(e.last_route()=="ivf"&&e.effective_nprobe()==recommendation);e.execute("SET search_mode=exact;");assert(ids(e,q)==changed);e.execute("SET search_mode=ann;");e.execute("ROLLBACK;");assert(!e.calibration_stale("t"));assert(ids(e,q)==all);
  assert(e.mutation_count("t")==0);
  e.execute("SET nprobe=0;");e.execute("DELETE FROM t WHERE id=4199;");ids(e,q);assert(e.last_route()=="ivf"&&e.effective_nprobe()==recommendation&&e.active_rows("t")==4199);
  assert(e.mutation_count("t")==1&&!e.calibration_stale("t"));
  // A failed multi-row statement must undo its earlier row and counter writes.
  bool duplicate=false;try{e.execute("UPDATE t SET id=100000;");}catch(const std::exception&){duplicate=true;}assert(duplicate);assert(e.mutation_count("t")==1);
  e.execute("DELETE FROM t WHERE id=999999;");assert(e.mutation_count("t")==1);
  e.close();
 }
 {
  Engine e(root,1<<20);assert(e.recommended_nprobe("t")==recommendation);assert(e.active_rows("t")==4199);
  assert(e.mutation_count("t")==1&&e.rows_at_calibration("t")==4200&&e.drift_threshold("t")==840);
  e.execute("SET search_mode=ann;");ids(e,query("[17000,17000]"));assert(e.last_route()=="ivf");
  // Exactly the threshold remains calibrated; the next affected row falls back.
  e.execute("UPDATE t SET category='changed' WHERE id<839;");assert(e.mutation_count("t")==840&&!e.calibration_stale("t"));
  ids(e,query("[17000,17000]"));assert(e.last_route()=="ivf"&&e.effective_nprobe()==recommendation);
  e.execute("BEGIN;");e.execute("UPDATE t SET category='rolled' WHERE id=1000;");assert(e.calibration_stale("t"));ids(e,query("[17000,17000]"));assert(e.last_route()=="exact_fallback");e.execute("ROLLBACK;");
  assert(e.mutation_count("t")==840&&!e.calibration_stale("t"));ids(e,query("[17000,17000]"));assert(e.last_route()=="ivf");
  e.execute("UPDATE t SET category='committed' WHERE id=1000;");assert(e.mutation_count("t")==841&&e.calibration_stale("t"));
  ids(e,query("[17000,17000]"));assert(e.last_route()=="exact_fallback"&&e.effective_nprobe()==0);e.close();
 }
 {
  Engine e(root,1<<20);assert(e.mutation_count("t")==841&&e.rows_at_calibration("t")==4200&&e.calibration_stale("t"));
  e.execute("SET search_mode=ann;");ids(e,query("[17000,17000]"));assert(e.last_route()=="exact_fallback");
 }
 // Single dictionary equality can scan its sorted active slots exactly,
 // including under manual probes; large postings retain the IVF planner.
 {
  // Two MiB leaves room for the 8400-row cache after its metadata grows.
  Engine e(root/"dictionary_postings",2<<20);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(2));");
  e.execute("BEGIN;");
  for(int start=0;start<4200;start+=100){std::string sql="INSERT INTO t VALUES ";for(int i=start;i<start+100;++i){if(i!=start)sql+=",";auto cat=i<64?"small":i<129?"large":i<211?"boundary":i<294?"over":"rest";sql+="("+std::to_string(5000-i)+",'"+cat+"','["+std::to_string(i%17)+",0]')";}e.execute(sql+";");}e.execute("COMMIT;");
  e.execute("CREATE VECTOR INDEX vi ON t(v);");
  const auto small=query("[0,0]","WHERE category='small'");
  auto exact=ids(e,small);assert(exact.size()==10&&e.last_route()=="exact_filter");
  e.execute("SET search_mode=ann;");e.execute("SET nprobe=5;");
  for(int pass=0;pass<3;++pass){assert(ids(e,small)==exact);assert(e.last_route()=="ann_filter_exact"&&e.effective_nprobe()==0);}
  auto reads=e.vector_page_reads();assert(ids(e,query("[0,0]","WHERE category='absent'")).empty());assert(e.last_route()=="ann_filter_exact"&&e.effective_nprobe()==0&&reads==e.vector_page_reads());
  // 5*4200/(64*4)=82: multiply before dividing. Exactly82 uses postings,
  // 83 uses IVF. With one probe the64-row floor applies, then65 uses IVF.
  ids(e,query("[0,0]","WHERE category='boundary'"));assert(e.last_route()=="ann_filter_exact"&&e.effective_nprobe()==0);
  ids(e,query("[0,0]","WHERE category='over'"));assert(e.last_route()=="ivf"&&e.effective_nprobe()==5);
  e.execute("SET nprobe=1;");assert(ids(e,small)==exact);assert(e.last_route()=="ann_filter_exact");
  ids(e,query("[0,0]","WHERE category='large'"));assert(e.last_route()=="ivf"&&e.effective_nprobe()==1);e.execute("SET nprobe=5;");
  e.execute("SET nprobe=64;");ids(e,query("[0,0]","WHERE category='large'"));assert(e.last_route()=="ann_filter_exact"&&e.effective_nprobe()==0);e.execute("SET nprobe=5;");
  ids(e,query("[0,0]","WHERE category='small' AND category='small'"));assert(e.last_route()=="ivf"&&e.effective_nprobe()==5);
  ids(e,query("[0,0]"));assert(e.last_route()=="ivf"&&e.effective_nprobe()==5);
  e.execute("BEGIN;");e.execute("UPDATE t SET category='changed' WHERE category='small';");assert(ids(e,small).empty()&&e.last_route()=="ann_filter_exact");e.execute("ROLLBACK;");assert(ids(e,small)==exact);
  bool failed=false;try{e.execute("INSERT INTO t VALUES (99999,'small','[0,0]'),(5000,'small','[0,0]');");}catch(const std::exception&){failed=true;}assert(failed);assert(ids(e,small)==exact);
  // A larger table/probe count raises the cost threshold to262.
  e.execute("BEGIN;");for(int start=4200;start<8400;start+=100){std::string sql="INSERT INTO t VALUES ";for(int i=start;i<start+100;++i){if(i!=start)sql+=",";sql+="("+std::to_string(5000-i)+",'rest','[0,0]')";}e.execute(sql+";");}e.execute("COMMIT;");
  e.execute("SET nprobe=8;");ids(e,query("[0,0]","WHERE category='large'"));assert(e.read_cache_slots()==8400);assert(e.last_route()=="ann_filter_exact"&&e.effective_nprobe()==0);
  e.execute("DELETE FROM t WHERE category='small';");assert(ids(e,small).empty()&&e.last_route()=="ann_filter_exact");assert(e.budget_peak()<=(2U<<20));
 }
 // Build inside an explicit transaction, then rollback its fresh namespace.
 {
  Engine e(root/"rollback",1<<20);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(2));");e.execute("INSERT INTO t VALUES (1,'red','[0,0]'),(2,'blue','[1,1]');");
  e.execute("BEGIN;");e.execute("INSERT INTO t VALUES (3,'red','[2,2]');");e.execute("CREATE VECTOR INDEX vi ON t(v);");assert(e.recommended_nprobe("t")>0);
  assert(e.drift_threshold("t")==64&&e.rows_at_calibration("t")==3&&e.mutation_count("t")==0);
  bool duplicate=false;try{e.execute("UPDATE t SET id=10;");}catch(const std::exception&){duplicate=true;}assert(duplicate);assert(ids(e,"SELECT id FROM t ORDER BY id LIMIT 10;")==std::vector<int64_t>({1,2,3}));assert(!e.calibration_stale("t")&&e.mutation_count("t")==0);
  e.execute("ROLLBACK;");assert(e.recommended_nprobe("t")==0&&e.active_rows("t")==2);
  e.execute("CREATE VECTOR INDEX vi ON t(v);");e.execute("SET search_mode=ann;");ids(e,query("[0,0]"));assert(e.last_route()=="exact_fallback");
  assert(ids(e,query("[0,0]","WHERE category='red'"))==std::vector<int64_t>{1});assert(e.last_route()=="ann_filter_exact"&&e.effective_nprobe()==0);
  e.execute("SET nprobe=1;");ids(e,query("[0,0]"));assert(e.last_route()=="ivf");
 }
 // A degenerate training set puts >4092 slots in bucket zero. Its slots
 // cross the compact page-zero header boundary and a following page.
 {
  Engine e(root/"wide_bucket",1<<20);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(2));");e.execute("BEGIN;");
  for(int start=0;start<4200;start+=100){std::string sql="INSERT INTO t VALUES ";for(int i=start;i<std::min(start+100,4200);++i){if(i!=start)sql+=",";sql+="("+std::to_string(i)+",'same','[0,0]')";}e.execute(sql+";");}e.execute("COMMIT;");
  auto undo=e.undo_spill_bytes();e.execute("CREATE VECTOR INDEX vi ON t(v);");assert(e.undo_spill_bytes()==undo);
  e.execute("SET search_mode=ann;");e.execute("SET nprobe=64;");
  const auto boundary=query("[0,0]","WHERE id>=4090"),tail=query("[0,0]","WHERE id>=4190");
  auto before=ids(e,boundary),last=ids(e,tail);assert(before.front()==4090&&before.back()==4099);assert(last.front()==4190&&last.back()==4199);
  // Warm twice so every vector handle validates before the direct bucket scan.
  auto hot=query("[0,0]","WHERE category='same'");auto hot_ids=ids(e,hot);assert(e.read_cache_slots()==4200);assert(ids(e,hot)==hot_ids);assert(ids(e,hot)==hot_ids);
  e.execute("BEGIN;");e.execute("DELETE FROM t WHERE id=4091;");e.execute("DELETE FROM t WHERE id=4199;");e.execute("UPDATE t SET v='[1,1]' WHERE id=4092;");
  auto changed=ids(e,boundary),changed_tail=ids(e,tail);e.execute("SET search_mode=exact;");assert(ids(e,boundary)==changed&&ids(e,tail)==changed_tail);e.execute("ROLLBACK;");e.execute("SET search_mode=ann;");assert(ids(e,boundary)==before&&ids(e,tail)==last);
  e.close();
 }
 // Bounded tables still measure recall when their payload exceeds half the
 // budget. Calibration retains only samples and Top10s, not the full payload.
 {
  auto path=root/"wide_calibration";std::string zero="[";for(int j=0;j<128;++j){if(j)zero+=",";zero+="0";}zero+="]";
  {
   Engine e(path,1<<20);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(128));");e.execute("BEGIN;");
   for(int start=0;start<4200;start+=100){std::string sql="INSERT INTO t VALUES ";for(int i=start;i<std::min(start+100,4200);++i){if(i!=start)sql+=",";sql+="("+std::to_string(i)+",'same','"+zero+"')";}e.execute(sql+";");}e.execute("COMMIT;");
   auto spills=e.spill_bytes();e.execute("CREATE VECTOR INDEX vi ON t(v);");assert(e.recommended_nprobe("t")==1&&!e.calibration_skipped("t")&&e.calibration_query_count("t")==16);
   e.execute("SET search_mode=ann;");auto result=ids(e,query(zero));assert(e.last_route()=="ivf"&&e.effective_nprobe()==1&&result.size()==10);
   e.execute("SET search_mode=exact;");assert(ids(e,query(zero))==result);e.execute("SET search_mode=ann;");
   e.execute("SET nprobe=64;");assert(ids(e,query(zero))==result&&e.last_route()=="ivf");assert(e.spill_bytes()==spills);assert(e.budget_peak()<=(1U<<20));e.close();
  }
  Engine e(path,1<<20);assert(!e.calibration_skipped("t")&&e.calibration_query_count("t")==16&&e.recommended_nprobe("t")==1);
  e.execute("SET search_mode=ann;");ids(e,query(zero));assert(e.last_route()=="ivf"&&e.effective_nprobe()==1);
 }
 // Even a small table must skip calibration when its fixed sample/Top10
 // workspace cannot fit. Index construction still succeeds and persists this
 // distinction; automatic search remains exact and an explicit probe works.
 {
  auto path=root/"workspace_skip";
  {
   Engine e(path,65536);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,category VARCHAR(16),v VECTOR(2));");
   e.execute("BEGIN;");for(int i=0;i<10;++i)e.execute("INSERT INTO t VALUES ("+std::to_string(i)+",'same','[0,0]');");e.execute("COMMIT;");
   e.execute("CREATE VECTOR INDEX vi ON t(v);");assert(e.calibration_skipped("t")&&e.calibration_query_count("t")==0&&e.recommended_nprobe("t")==4);
   e.execute("SET search_mode=ann;");auto result=ids(e,query("[0,0]"));assert(e.last_route()=="exact_fallback"&&e.effective_nprobe()==0&&result.size()==10);
   assert(ids(e,query("[0,0]","WHERE category='same'"))==result&&e.read_cache_slots()==0);
   e.execute("SET nprobe=8;");assert(ids(e,query("[0,0]"))==result&&e.last_route()=="ivf");assert(e.budget_peak()<=65536);e.close();
  }
  Engine e(path,65536);assert(e.calibration_skipped("t")&&e.calibration_query_count("t")==0&&e.recommended_nprobe("t")==4);
  e.execute("SET search_mode=ann;");ids(e,query("[0,0]"));assert(e.last_route()=="exact_fallback");
 }
 // Workspace exhaustion happens after namespace allocation. A failed build
 // must remove that namespace and retain earlier writes in the transaction.
 {
  Engine e(root/"failure",65536);e.execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(1024));");std::string vector="[";for(int i=0;i<1024;++i){if(i)vector+=",";vector+="0";}vector+="]";
  e.execute("BEGIN;");for(int i=0;i<5;++i)e.execute("INSERT INTO t VALUES ("+std::to_string(i)+",'"+vector+"');");bool failed=false;try{e.execute("CREATE VECTOR INDEX vi ON t(v);");}catch(const std::exception&){failed=true;}assert(failed);assert(e.recommended_nprobe("t")==0);assert(ids(e,"SELECT id FROM t ORDER BY id LIMIT 10;").size()==5);e.execute("COMMIT;");
 }
 std::cout<<"index calibration/region regression passed\n";
}
