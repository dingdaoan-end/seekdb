#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/engine.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using mvdb::Engine;
namespace fs=std::filesystem;

[[noreturn]] void compact_kill(){
  assert(::kill(::getpid(),SIGKILL)==0);
  ::_exit(91);
}
template<class F> void compact_child(F action,bool killed=false){
  const auto pid=::fork();assert(pid>=0);
  if(pid==0){
    try{action();::_exit(0);}
    catch(const std::exception& error){std::cerr<<"compact child exception: "<<error.what()<<"\n";::_exit(92);}
  }
  int status=0;pid_t done;
  do{done=::waitpid(pid,&status,0);}while(done<0&&errno==EINTR);
  assert(done==pid);
  if(killed)assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
  else {
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)std::cerr<<"compact fixture child status="<<status<<"\n";
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
  }
}
void compact_fixture(const fs::path& path,size_t budget,bool baseline_checkpoint){
  auto e=std::make_unique<Engine>(path,budget);
  e->execute("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(2));");
  e->execute("BEGIN;");
  // A single wide list crosses the first 508-entry block and many subsequent
  // 512-entry blocks, including the logical boundaries around slot 4092.
  for(int start=0;start<4200;start+=100){
    std::string sql="INSERT INTO t VALUES ";
    for(int i=start;i<std::min(start+100,4200);++i){
      if(i!=start)sql+=",";
      sql+="("+std::to_string(i)+",'[0,0]')";
    }
    e->execute(sql+";");
  }
  e->execute("COMMIT;");
  e->execute("CREATE VECTOR INDEX vi ON t(v);");
  bool packed=false;
  for(const auto& entry:fs::directory_iterator(path/"data")){
    const auto file=entry.path().filename().string();
    if(file.find(".cidx.r")!=std::string::npos&&file.ends_with(".lists")){
      assert(entry.file_size()>mvdb::PAGE);packed=true;
    }
  }
  assert(packed);
  // The small-budget fixture retains the original index-construction WAL.
  // The other fixture covers DML against an already published packed epoch.
  if(baseline_checkpoint){e->close();e.reset();e=std::make_unique<Engine>(path,budget);}
  auto mutate=[](Engine& db){
    db.execute("BEGIN;");
    db.execute("UPDATE t SET v='[1,1]' WHERE id=4092;");
    db.execute("DELETE FROM t WHERE id=4091;");
    db.execute("DELETE FROM t WHERE id=4199;");
    db.execute("INSERT INTO t VALUES (4200,'[2,2]');");
    db.execute("COMMIT;");
    assert(db.active_rows("t")==4199);
  };
  mutate(*e);
  // Deliberately bypass both Engine and Storage destructors/checkpoints.
  ::_exit(0);
}
void compact_verify(const fs::path& path,size_t budget){
  Engine e(path,budget);assert(e.active_rows("t")==4199);
  int64_t expected=0;size_t seen=0;
  e.execute("SELECT id,v FROM t ORDER BY id;").each([&](const auto& row){
    while(expected==4091||expected==4199)++expected;
    const auto id=std::get<int64_t>(row[0]);assert(id==expected++);
    const auto& v=std::get<std::vector<float>>(row[1]);assert(v.size()==2);
    const float value=id==4092?1.f:id==4200?2.f:0.f;
    assert(v[0]==value&&v[1]==value);++seen;
  });
  assert(seen==4199&&expected==4201);
  auto ranked=[&](const std::string& query,const std::string& filter,bool generic){
    std::vector<std::pair<int64_t,double>> rows;
    e.execute("SELECT id,L2_DISTANCE(v,'"+query+"') AS d"+
              (generic?",id AS again":"")+" FROM t"+filter+
              " ORDER BY d,id LIMIT 10;").each([&](const auto& row){
      rows.emplace_back(std::get<int64_t>(row[0]),std::get<double>(row[1]));
    });
    return rows;
  };
  for(const std::string query:{"[0,0]","[1,1]","[2,2]"}){
    for(const std::string filter:{""," WHERE id>=4090"}){
      e.execute("SET search_mode=exact;");const auto exact=ranked(query,filter,true);
      e.execute("SET search_mode=ann;");e.execute("SET nprobe=64;");
      assert(ranked(query,filter,false)==exact);
      assert(e.last_route()=="ivf"&&e.effective_nprobe()==64);
      assert(ranked(query,filter,true)==exact);
      assert(e.last_route()=="ivf"&&e.effective_nprobe()==64);
    }
  }
  assert(e.budget_peak()<=budget);
}
}

void compact_crash_regressions(){
  const auto root=fs::path("/tmp")/("mvdb_compact_recovery_"+std::to_string(::getpid()));
  fs::remove_all(root);fs::create_directories(root);
  for(const size_t budget:{size_t(65536),size_t(1<<20)}){
    const auto fixture=root/("fixture_"+std::to_string(budget));
    compact_child([&]{compact_fixture(fixture,budget,budget>65536);});
    // A committed WAL must remain for interrupted replay to exercise.
    assert(fs::file_size(fixture/"wal")>0);
    for(size_t stage=1;stage<=4;++stage){
      const auto path=root/("case_"+std::to_string(budget)+"_"+std::to_string(stage));
      fs::copy(fixture,path,fs::copy_options::recursive|fs::copy_options::copy_symlinks);
      for(int retry=0;retry<2;++retry){
        compact_child([&]{Engine e(path,budget,[](size_t step){if(step==1)compact_kill();});},true);
      }
      compact_child([&]{mvdb::Storage s(path,budget);s.checkpoint([&](size_t point){if(point==stage)compact_kill();});},true);
      compact_verify(path,budget);
    }
  }
  fs::remove_all(root);
}
