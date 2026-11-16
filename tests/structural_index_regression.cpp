#include "../src/engine.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
using namespace mvdb;
size_t rows(Engine& e){size_t n=0;e.execute("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d,id LIMIT 1000").each([&](const auto&){++n;});return n;}
void seed(Engine& e){e.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(2))");std::string q="INSERT INTO t VALUES ";for(int i=0;i<160;++i){if(i)q+=",";q+="("+std::to_string(i)+",'["+std::to_string(i)+",0]')";}e.execute(q);}
void legacy_fixture(const std::filesystem::path& root){
 struct Prefix {uint32_t magic,slots;uint64_t blob_end;uint32_t nlist,ivector,schema_size;char schema[4096];uint32_t active_count,recommended_nprobe,calibration_stale;char index_region[128];};
 struct List{uint64_t offset;uint32_t count,capacity;};struct Directory{uint64_t magic,next;List lists[64];};
 Page header{};{std::ifstream in(root/"data/t.header",std::ios::binary);in.read(reinterpret_cast<char*>(header.data()),PAGE);}Prefix prefix{};std::memcpy(&prefix,header.data(),sizeof(prefix));auto base=std::string(prefix.index_region);auto old=std::string("t.index.r1");
 std::ifstream in(root/"data"/(base+".ivf"),std::ios::binary);Directory d{};in.read(reinterpret_cast<char*>(&d),sizeof(d));
 for(uint32_t b=0;b<prefix.nlist;++b){Page p{};std::memcpy(p.data(),&d.lists[b].count,4);in.seekg(d.lists[b].offset*4);in.read(reinterpret_cast<char*>(p.data()+16),d.lists[b].count*4);std::ofstream out(root/"data"/(old+".ivf"+std::to_string(b)),std::ios::binary);out.write(reinterpret_cast<const char*>(p.data()),PAGE);}
 for(auto suffix:{".centers",".buckets"})std::filesystem::rename(root/"data"/(base+suffix),root/"data"/(old+suffix));
 std::memset(header.data()+offsetof(Prefix,index_region),0,128);std::memcpy(header.data()+offsetof(Prefix,index_region),old.c_str(),old.size());{std::ofstream out(root/"data/t.header",std::ios::binary);out.write(reinterpret_cast<const char*>(header.data()),PAGE);}in.close();std::filesystem::remove(root/"data"/(base+".ivf"));
}
}
void structural_index_regressions(){
 const auto root=std::filesystem::temp_directory_path()/"mvdb_structural_index";std::filesystem::remove_all(root);
 {Engine e(root,1<<20);seed(e);e.execute("CREATE VECTOR INDEX vi ON t(v)");e.execute("SET search_mode=ann");e.execute("SET nprobe=8");assert(rows(e)==160);
  size_t packed=0,legacy=0;for(auto& f:std::filesystem::directory_iterator(root/"data")){auto n=f.path().filename().string();if(n.ends_with(".ivf"))++packed;else if(n.find(".ivf")!=std::string::npos)++legacy;}assert(packed==1&&legacy==0);
  e.execute("BEGIN");e.execute("DELETE FROM t WHERE id=0");e.execute("UPDATE t SET v='[0,0]' WHERE id=1");e.execute("INSERT INTO t VALUES(1000,'[0,0]')");e.execute("ROLLBACK");assert(rows(e)==160);
  // Relocating a full packed list must preserve all other shared-page lists.
  for(int i=0;i<80;++i)e.execute("INSERT INTO t VALUES("+std::to_string(1000+i)+",'[0,0]')");
  assert(rows(e)==240);
  e.execute("BEGIN");bool fail=false;try{e.execute("INSERT INTO t VALUES(2000,'[0,0]'),(0,'[1,1]')");}catch(...){fail=true;}assert(fail);assert(rows(e)==240);e.execute("COMMIT");e.close();}
 {Engine e(root,65536);e.execute("SET search_mode=ann");e.execute("SET nprobe=8");assert(rows(e)==240);assert(e.budget_peak()<=65536);e.close();}
 // A committed packed baseline must survive repeated interrupted WAL replay.
 pid_t pid=::fork();assert(pid>=0);if(!pid){Engine e(root,1<<20);e.execute("UPDATE t SET v='[2,2]' WHERE id=1");e.execute("DELETE FROM t WHERE id=2");e.execute("INSERT INTO t VALUES(3000,'[0,0]')");::kill(::getpid(),SIGKILL);_exit(1);}int status;assert(::waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));
 pid=::fork();assert(pid>=0);if(!pid){Engine e(root,1<<20,[](size_t n){if(n==2)::kill(::getpid(),SIGKILL);});_exit(0);}assert(::waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));
 {Engine e(root,1<<20);e.execute("SET search_mode=ann");e.execute("SET nprobe=8");assert(rows(e)==240);e.close();}
 // A crash at any pre-header publication boundary leaves the table usable;
 // rebuilding allocates a different persistent namespace.
 for(size_t stage=1;stage<=4;++stage){auto p=root/("crash"+std::to_string(stage));{Engine e(p,1<<20);seed(e);e.close();}
  pid=::fork();assert(pid>=0);if(!pid){Engine e(p,1<<20);e.index_publication_progress([&](size_t n){if(n==stage)::kill(::getpid(),SIGKILL);});e.execute("CREATE VECTOR INDEX vi ON t(v)");_exit(1);}assert(::waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));
  pid=::fork();assert(pid>=0);if(!pid){Engine e(p,1<<20,[](size_t n){if(n==1)::kill(::getpid(),SIGKILL);});_exit(1);}assert(::waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));
  {Engine e(p,1<<20);for(auto& f:std::filesystem::directory_iterator(p/"data"))assert(f.path().filename().string().find(".index.p.")==std::string::npos);assert(rows(e)==160);e.execute("BEGIN");e.execute("CREATE VECTOR INDEX vi ON t(v)");e.execute("INSERT INTO t VALUES(1000,'[0,0]')");e.execute("ROLLBACK");assert(rows(e)==160);e.execute("CREATE VECTOR INDEX vi ON t(v)");e.close();}
 }
 // Publish and commit without checkpoint, then interrupt recovery after its
 // publication sentinel. Subsequent recovery must retain the baseline.
 auto committed=root/"committed";{Engine e(committed,1<<20);seed(e);e.close();}
 pid=::fork();assert(pid>=0);if(!pid){Engine e(committed,1<<20);e.execute("CREATE VECTOR INDEX vi ON t(v)");::kill(::getpid(),SIGKILL);_exit(1);}assert(::waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));
 pid=::fork();assert(pid>=0);if(!pid){Engine e(committed,1<<20,[](size_t n){if(n==1)::kill(::getpid(),SIGKILL);});_exit(1);}assert(::waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));
 {Engine e(committed,65536);e.execute("SET search_mode=ann");e.execute("SET nprobe=8");assert(rows(e)==160);e.close();}
 legacy_fixture(committed);
 {Engine e(committed,65536);e.execute("SET search_mode=ann");e.execute("SET nprobe=8");assert(rows(e)==160);e.execute("INSERT INTO t VALUES(5000,'[0,0]')");e.execute("DELETE FROM t WHERE id=0");assert(rows(e)==160);e.close();}
 auto retired=root/"retired";{Engine e(retired,1<<20);seed(e);e.close();}
 pid=::fork();assert(pid>=0);if(!pid){Engine e(retired,65536);e.execute("BEGIN");e.execute("CREATE VECTOR INDEX vi ON t(v)");for(int i=0;i<40;++i)e.execute("INSERT INTO t VALUES("+std::to_string(1000+i)+",'[0,0]')");e.execute("ROLLBACK");::kill(::getpid(),SIGKILL);_exit(1);}assert(::waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));
 {Engine e(retired,65536);assert(rows(e)==160);for(auto& f:std::filesystem::directory_iterator(retired/"data"))assert(f.path().filename().string().find(".index.p.")==std::string::npos);e.execute("CREATE VECTOR INDEX vi ON t(v)");e.close();}
 std::cout<<"packed IVF growth, DML, rollback, statement failure, checkpoint and interrupted replay passed\n";
}
#ifdef MVDB_STRUCTURAL_INDEX_STANDALONE
int main(){structural_index_regressions();}
#endif
