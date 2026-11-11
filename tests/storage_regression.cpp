#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/storage.hpp"
#include <filesystem>
#include <cassert>
#include <iostream>
#include <fstream>
#include <random>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <cstdarg>
#include <cerrno>
using namespace mvdb;
// Deterministically emulate filesystems rejecting O_DIRECT at open or first
// I/O. The kernel handles all operations outside each narrowly scoped test.
namespace {
int reject_direct=0;unsigned direct_rejections=0;
int kill_append_write_stage=0;
bool audit_io=false;
unsigned direct_opens=0,data_syncs=0,full_syncs=0,cache_drops=0,data_file_syncs=0,wal_file_syncs=0;
unsigned sync_interruptions=0,index_read_interruptions=0,index_write_interruptions=0;
std::string descriptor_path(int fd){
 char path[512];auto link="/proc/self/fd/"+std::to_string(fd);auto n=::readlink(link.c_str(),path,sizeof(path));
 return n>0?std::string(path,static_cast<size_t>(n)):std::string{};
}
bool shadow_index(int fd){return descriptor_path(fd).find("/index/")!=std::string::npos;}
void reset_audit(){direct_opens=data_syncs=full_syncs=cache_drops=data_file_syncs=wal_file_syncs=0;}
}
extern "C" int open(const char* path,int flags,...){
 if(audit_io&&(flags&O_DIRECT))++direct_opens;
 mode_t mode=0;if(flags&O_CREAT){va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
 if(reject_direct==1&&(flags&O_DIRECT)){++direct_rejections;errno=EINVAL;return -1;}
 return static_cast<int>(::syscall(SYS_openat,AT_FDCWD,path,flags,mode));
}
extern "C" ssize_t pread(int fd,void* data,size_t n,off_t offset){
 if(index_read_interruptions&&shadow_index(fd)){--index_read_interruptions;errno=EINTR;return -1;}
 if(reject_direct==2&&(::fcntl(fd,F_GETFL)&O_DIRECT)){++direct_rejections;errno=EINVAL;return -1;}
 return ::syscall(SYS_pread64,fd,data,n,offset);
}
extern "C" ssize_t pwrite(int fd,const void* data,size_t n,off_t offset){
 if(kill_append_write_stage&&descriptor_path(fd).ends_with("/data/t.v1")){
  if(kill_append_write_stage==1)::kill(::getpid(),SIGKILL);
  auto result=::syscall(SYS_pwrite64,fd,data,n,offset);::kill(::getpid(),SIGKILL);return result;
 }
 if(index_write_interruptions&&shadow_index(fd)){--index_write_interruptions;errno=EINTR;return -1;}
 if(reject_direct==3&&(::fcntl(fd,F_GETFL)&O_DIRECT)){++direct_rejections;errno=EINVAL;return -1;}
 return ::syscall(SYS_pwrite64,fd,data,n,offset);
}
extern "C" int fdatasync(int fd){if(audit_io){++data_syncs;if(descriptor_path(fd).find("/wal")!=std::string::npos)++wal_file_syncs;}if(sync_interruptions){--sync_interruptions;errno=EINTR;return -1;}return static_cast<int>(::syscall(SYS_fdatasync,fd));}
extern "C" int fsync(int fd){if(audit_io){++full_syncs;if(descriptor_path(fd).find("/data/")!=std::string::npos)++data_file_syncs;}if(sync_interruptions){--sync_interruptions;errno=EINTR;return -1;}return static_cast<int>(::syscall(SYS_fsync,fd));}
extern "C" int posix_fadvise(int fd,off_t offset,off_t length,int advice) noexcept {
 if(audit_io&&advice==POSIX_FADV_DONTNEED)++cache_drops;
 auto result=::syscall(SYS_fadvise64,fd,offset,length,advice);return result<0?errno:static_cast<int>(result);
}
const std::filesystem::path base="/tmp/mvdb_storage_phase3";
template<class F> void child(F action,bool killed=false){auto c=fork();assert(c>=0);if(!c){action();_exit(0);}int status;assert(waitpid(c,&status,0)==c);assert(killed?WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL:WIFEXITED(status)&&WEXITSTATUS(status)==0);}
void kill_now(){::kill(getpid(),SIGKILL);}
// Returning to implicit direct I/O would make default reads fail this audit.
void buffered_default(){
 for(const char* value:{static_cast<const char*>(nullptr),"0","true"}){
  if(value)::setenv("MVDB_USE_ODIRECT",value,1);else ::unsetenv("MVDB_USE_ODIRECT");
  auto path=base/("buffered_"+std::string(value?value:"unset"));Storage s(path,65536);
  reset_audit();audit_io=true;s.read("t.v1",0);audit_io=false;
  assert(direct_opens==0&&cache_drops==0);
 }
 ::unsetenv("MVDB_USE_ODIRECT");
}
// A three-frame pool must retain hot table and primary metadata across a
// streaming payload scan, while still admitting payload and dirty evictions.
void metadata_scan(){
 Storage s(base/"metadata_scan",65536);s.read("t.header",0);s.read("t.pk",0);
 auto reads=s.pool.physical_reads;
 for(int i=0;i<100;i++)s.read("t.v1",i);
 assert(s.pool.physical_reads==reads+100);reads=s.pool.physical_reads;
 s.read("t.header",0);s.read("t.pk",0);assert(s.pool.physical_reads==reads);
 s.begin();Page p{};p[0]=std::byte(31);
 for(int i=0;i<40;i++){s.write("table"+std::to_string(i%8)+".pk",i,p);s.write("t.v1",i,p);}
 assert(s.shadow_bytes_written>0);s.rollback();
 for(int i=0;i<40;i++)assert(s.read("t.v1",i)[0]==std::byte(0));
 assert(s.budget.peak<=s.budget.limit);
}
// Observe real syscalls: a multi-window transaction steals original pages
// and spills undo, yet performs only its single COMMIT sync until checkpoint.
void transaction_sync_boundary(){
 ::unsetenv("MVDB_USE_ODIRECT");Storage s(base/"sync_boundary",65536);std::mt19937 random(51);Page p{};
 auto fill=[&]{for(size_t j=0;j<PAGE;j+=4){uint32_t v=random();std::memcpy(p.data()+j,&v,4);}};
 s.begin();for(int i=0;i<40;i++){fill();s.write("t.v1",i,p);}s.commit();s.checkpoint();
 auto syncs=s.wal_syncs,spill=s.undo_spill_bytes;reset_audit();audit_io=true;s.begin();
 for(int i=0;i<40;i++){fill();s.write("t.v1",i,p);}
 assert(s.undo_spill_bytes-spill>64*1024&&s.shadow_bytes_written>0);
 assert(data_syncs==0&&full_syncs==0&&cache_drops==0&&direct_opens==0);
 s.commit();audit_io=false;assert(data_syncs==1&&wal_file_syncs==1&&full_syncs==0&&cache_drops==1);
 assert(s.wal_syncs-syncs==1);
 reset_audit();audit_io=true;
 s.checkpoint([&](size_t stage){if(stage==1)assert(data_file_syncs>0);});audit_io=false;
 assert(data_file_syncs>0&&full_syncs>0&&cache_drops>0);
}
// Graceful shutdown signals may interrupt a checkpoint or shadow operation.
// Retry these syscall boundaries rather than turning a signal into I/O failure.
void interrupted_io(){
 Storage s(base/"interrupted_io",65536);s.begin();Page p{};p[0]=std::byte(21);
 index_read_interruptions=2;index_write_interruptions=2;
 for(int i=0;i<12;i++)s.write("v",i,p);
 assert(index_read_interruptions==0&&index_write_interruptions==0);
 sync_interruptions=2;s.commit();assert(sync_interruptions==0);
 sync_interruptions=2;s.checkpoint();assert(sync_interruptions==0);
 for(int i=0;i<12;i++)assert(s.read("v",i)[0]==std::byte(21));
}
// Invisible extents publish only through normally logged metadata. Both the
// no-C and C boundaries, later shadow capture, and file-wide fallback matter.
void invisible_append(){
 auto check=[](const std::filesystem::path& d,unsigned count,unsigned tail){Storage s(d,65536);assert(s.read("t.header",0)[0]==std::byte(count));auto p=s.read("t.v1",0);assert(p[16]==std::byte(tail));};
 for(int stage=1;stage<=2;stage++){
  auto d=base/("append_mid_write"+std::to_string(stage));
  child([&]{Storage s(d,65536);s.begin();Page p{};p[16]=std::byte(42);kill_append_write_stage=stage;s.append("t.v1",0,p,16,4);},true);
  check(d,0,stage==1?0:42);
 }
 for(bool committed:{false,true}){
  auto d=base/(committed?"append_C":"append_no_C");
  child([&]{Storage s(d,65536);s.begin();Page p{},h{};p[16]=std::byte(42);auto undo=s.savepoint();auto wal=s.wal_bytes_written;
   s.append("t.v1",0,p,16,4);assert(s.savepoint()==undo&&s.wal_bytes_written==wal);h[0]=std::byte(1);s.write("t.header",0,h);if(committed)s.commit();kill_now();},true);
  check(d,committed?1:0,42);
 }
 auto d=base/"append_future_shadow";
 child([&]{Storage s(d,65536);s.begin();Page p{},h{};p[16]=std::byte(42);s.append("t.v1",0,p,16,4);h[0]=std::byte(1);s.write("t.header",0,h);s.commit();
  s.begin();p[16]=std::byte(99);s.write("t.v1",0,p);s.pool.flush_all();assert(s.shadow_bytes_written>0);kill_now();},true);
 for(int i=0;i<2;i++)child([&]{Storage s(d,65536,[](size_t n){if(n==1)kill_now();});},true);
 check(d,1,42);
 for(bool same_page:{false,true}){
  d=base/(same_page?"append_same_shadow":"append_file_shadow");
  {Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(1);s.write("t.v1",0,p);s.commit();}
  child([&]{Storage s(d,65536);s.begin();auto p=s.read("t.v1",0);p[0]=std::byte(2);s.write("t.v1",0,p);s.pool.flush_all();s.commit();
   s.begin();auto page=same_page?0:1;p=s.read("t.v1",page);p[16]=std::byte(42);auto undo=s.savepoint();s.append("t.v1",page,p,16,4);assert(s.savepoint()>undo);s.commit();kill_now();},true);
  {Storage s(d,65536);assert(s.read("t.v1",0)[0]==std::byte(2));assert(s.read("t.v1",same_page?0:1)[16]==std::byte(42));}
 }
 for(size_t stage=1;stage<=4;stage++){
  d=base/("append_checkpoint"+std::to_string(stage));
  child([&]{Storage s(d,65536);s.begin();Page p{},h{};p[16]=std::byte(42);s.append("t.v1",0,p,16,4);h[0]=std::byte(1);s.write("t.header",0,h);s.commit();s.checkpoint([&](size_t n){if(n==stage)kill_now();});},true);
  check(d,1,42);
 }
 d=base/"append_dirty_prefix";
 child([&]{Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(7);s.write("t.v1",0,p);s.commit();s.begin();p[16]=std::byte(42);s.append("t.v1",0,p,16,4);s.commit();kill_now();},true);
 {Storage s(d,65536);auto p=s.read("t.v1",0);assert(p[0]==std::byte(7)&&p[16]==std::byte(42));}
 // Earlier physical redo includes inverse writes from savepoint rollback.
 // Such redo would erase a later WAL-free reuse even without any shadow page.
 for(bool same_transaction:{false,true}){
  d=base/(same_transaction?"append_prior_redo_tx":"append_prior_redo_commit");
  child([&]{Storage s(d,32<<20);s.begin();Page p{};auto mark=s.savepoint();p[16]=std::byte(17);s.write("t.v1",0,p);s.rollback_to(mark);
   if(!same_transaction){s.commit();s.begin();}p=s.read("t.v1",0);p[16]=std::byte(42);mark=s.savepoint();s.append("t.v1",0,p,16,4);assert(s.savepoint()>mark);if(!same_transaction)assert(s.shadow_bytes_written==0);s.commit();kill_now();},true);
  {Storage s(d,65536);assert(s.read("t.v1",0)[16]==std::byte(42));}
 }
 // Compatibility: recover old WAL without guard markers, then guard later
 // appends against those earlier absolute redo runs on every subsequent restart.
 d=base/"append_rebuild_guard";
 child([&]{Storage s(d,32<<20);s.begin();Page p{};p[16]=std::byte(17);s.write("t.v1",0,p);s.commit();kill_now();},true);
 std::filesystem::remove_all(d/"shadow.0"/"written");
 child([&]{Storage s(d,32<<20);s.begin();auto p=s.read("t.v1",0);p[16]=std::byte(42);auto mark=s.savepoint();s.append("t.v1",0,p,16,4);assert(s.savepoint()>mark);s.commit();kill_now();},true);
 {Storage s(d,65536);assert(s.read("t.v1",0)[16]==std::byte(42));}
 // Reusing rolled-back garbage must remain safe under a later normal update.
 d=base/"append_savepoint";
 child([&]{Storage s(d,65536);s.begin();Page p{},h{};auto mark=s.savepoint();p[16]=std::byte(41);s.append("t.v1",0,p,16,4);h[0]=std::byte(1);s.write("t.header",0,h);s.rollback_to(mark);p[16]=std::byte(42);s.append("t.v1",0,p,16,4);s.write("t.header",0,h);s.commit();s.begin();p[16]=std::byte(99);s.write("t.v1",0,p);s.pool.flush_all();kill_now();},true);
 check(d,1,42);
}
void resident_handles(){
 Storage s(base/"resident_handles",65536);auto h=s.pool.handle("v",0);assert(s.pool.resident(h));
 for(unsigned i=1;i<10;i++)s.read("v",i);
 assert(!s.pool.resident(h));
 auto current=s.pool.handle("v",0);assert(s.pool.resident(current));s.pool.invalidate_prefix("v");assert(!s.pool.resident(current));
 Storage large(base/"larger_pool",12<<20);auto reads=large.pool.physical_reads;
 for(int repeat=0;repeat<2;repeat++)for(int i=0;i<480;i++)large.read("v",i);
 assert(large.pool.physical_reads==reads+480&&large.budget.peak<=large.budget.limit);
}
int main(int argc,char** argv){
 ::unsetenv("MVDB_USE_ODIRECT");
 if(argc==2){std::filesystem::remove_all(base);std::string test=argv[1];
  if(test=="buffered")buffered_default();else if(test=="metadata")metadata_scan();else if(test=="sync")transaction_sync_boundary();else if(test=="interrupt")interrupted_io();else if(test=="append")invisible_append();else if(test=="resident")resident_handles();else return 2;return 0;}
 std::filesystem::remove_all(base);buffered_default();metadata_scan();transaction_sync_boundary();interrupted_io();invisible_append();resident_handles();
 for(int mode=1;mode<=3;mode++){
  ::setenv("MVDB_USE_ODIRECT","1",1);
  auto fallback=base/("direct_fallback"+std::to_string(mode));reject_direct=mode;direct_rejections=0;
  {Storage s(fallback,65536);s.begin();Page p{};p[0]=std::byte(41);for(int i=0;i<12;i++)s.write("v",i,p);s.commit();
   s.begin();p[0]=std::byte(99);for(int i=0;i<12;i++)s.write("v",i,p);s.rollback();
   for(int i=0;i<12;i++)assert(s.read("v",i)[0]==std::byte(41));
   auto spill=s.temporary();assert(std::fwrite("spill",1,5,spill.get())==5);std::rewind(spill.get());char bytes[5];assert(std::fread(bytes,1,5,spill.get())==5&&std::memcmp(bytes,"spill",5)==0);
   auto target=std::filesystem::read_symlink("/proc/self/fd/"+std::to_string(::fileno(spill.get()))).string();assert(target.rfind(fallback.string()+"/spill.",0)==0&&target.ends_with(" (deleted)"));
   s.close();}
  assert(direct_rejections>0);
  {Storage s(fallback,65536);for(int i=0;i<12;i++)assert(s.read("v",i)[0]==std::byte(41));}
  child([&]{Storage s(fallback,65536);s.begin();Page p{};p[0]=std::byte(42);for(int i=0;i<12;i++)s.write("v",i,p);s.commit();
   s.begin();p[0]=std::byte(99);for(int i=0;i<12;i++)s.write("v",i,p);kill_now();},true);
  {Storage s(fallback,65536);for(int i=0;i<12;i++)assert(s.read("v",i)[0]==std::byte(42));}
  reject_direct=0;
 }
 ::unsetenv("MVDB_USE_ODIRECT");
 auto d=base/"compact";
 {Storage s(d,32<<20);s.begin();std::mt19937 random(7);Page p{},meta{};
  for(int i=0;i<100;i++){for(int j=0;j<128;j++){float v=float(random()%100000)/100000;std::memcpy(p.data()+j*4,&v,4);}s.write("t.v1",i,p);meta[0]=std::byte(i);s.write("t.meta",0,meta);}
  s.commit();for(auto& e:std::filesystem::directory_iterator(d))assert(e.path().filename().string().rfind("committed.",0)!=0);auto n=std::filesystem::file_size(d/"wal");std::cout<<"128D row WAL bytes="<<n/100.0<<" undo spill="<<s.undo_spill_bytes<<"\n";assert(n<=60000);assert(s.shadow_bytes_written==0);assert(s.undo_spill_bytes==0);assert(s.budget.peak<=s.budget.limit);s.close();assert(s.shadow_bytes_written==0);}
 // Every COMMIT has exactly one WAL fdatasync, regardless of absolute file
 // offset or the number of 64KiB windows in its transaction.
 d=base/"single_sync";
 {Storage s(d,32<<20);std::mt19937 random(17);
  for(int batch=0;batch<4;batch++){auto syncs=s.wal_syncs,bytes=s.wal_bytes_written;s.begin();Page p{},meta{};
   for(int i=0;i<100;i++){for(int j=0;j<128;j++){float v=float(random()%100000)/100000;std::memcpy(p.data()+j*4,&v,4);}s.write("t.v1",batch*100+i,p);meta[0]=std::byte(i);for(int j=8;j<104;j++)meta[j]=std::byte(random()&255);s.write("t.meta",0,meta);}
   s.commit();auto written=s.wal_bytes_written-bytes;std::cout<<"100-row WAL bytes="<<written<<" syncs="<<s.wal_syncs-syncs<<"\n";assert(written>54*1024&&written<64*1024);assert(s.wal_syncs-syncs==1);
  }
  // Large transactions also sync only at COMMIT; kernel cache is reclaimable
  // and charged to the external memory controller, not this userspace pool.
  auto syncs=s.wal_syncs,bytes=s.wal_bytes_written;s.begin();Page p{};
  for(int i=0;i<1600;i++){for(int j=0;j<128;j++){auto v=random();std::memcpy(p.data()+j*4,&v,4);}s.write("large",i,p);}
  s.commit();auto written=s.wal_bytes_written-bytes;assert(written>512*1024);assert(s.wal_syncs-syncs==1);
 }
 d=base/"compact";
 {Storage s(d,65536);s.begin();auto mark=s.savepoint();Page p{};for(int i=0;i<300;i++){p[0]=std::byte(i%255);s.write("tiny",i,p);}assert(s.undo_spill_bytes>0);s.rollback_to(mark);for(int i=0;i<300;i++)assert(s.read("tiny",i)[0]==std::byte(0));s.commit();assert(s.budget.peak<=s.budget.limit);}
 // Cold committed dirty frames must remain ordinary LRU candidates, so a
 // hot clean working set can use the whole pool after commit.
 d=base/"lru";
 {Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(1);for(int i=0;i<3;i++)s.write("cold",i,p);s.commit();
   for(int i=0;i<3;i++)s.read("hot",i);
   auto reads=s.pool.physical_reads;
   for(int repeat=0;repeat<10;repeat++)for(int i=0;i<3;i++)s.read("hot",i);
   assert(s.pool.physical_reads==reads);assert(s.shadow_bytes_written==0);}
 // No-Steal is a bounded preference: two cold uncommitted pages cannot
 // reserve two frames forever while a three-page clean working set thrashes.
 d=base/"active_lru";
 {Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(7);for(int i=0;i<2;i++)s.write("cold",i,p);
   for(int repeat=0;repeat<5;repeat++)for(int i=0;i<3;i++)s.read("hot",i);
   auto reads=s.pool.physical_reads;
   for(int repeat=0;repeat<10;repeat++)for(int i=0;i<3;i++)s.read("hot",i);
   assert(s.pool.physical_reads==reads);assert(s.shadow_bytes_written>0);
   s.rollback();for(int i=0;i<2;i++)assert(s.read("cold",i)[0]==std::byte(0));}
 d=base/"crash";
 child([&]{Storage s(d,32<<20);s.begin();Page p{};p[0]=std::byte(99);for(int i=0;i<12;i++)s.write("committed",i,p);s.commit();s.begin();p[0]=std::byte(77);for(int i=0;i<20;i++)s.write("loser",i,p);kill_now();},true);
 for(int i=0;i<3;i++)child([&]{Storage s(d,65536,[](size_t n){if(n==3)kill_now();});},true);
 {Storage s(d,65536);for(int i=0;i<12;i++)assert(s.read("committed",i)[0]==std::byte(99));assert(s.read("loser",0)[0]==std::byte(0));}
 for(size_t stage=1;stage<=4;stage++){
  d=base/("checkpoint"+std::to_string(stage));
  {Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(1);s.write("value",0,p);s.commit();}
  child([&]{Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(9);s.write("value",0,p);
    for(int i=0;i<20;i++)s.write("checkpoint_pressure",i,p);
    assert(s.shadow_bytes_written>0);s.rollback();s.begin();p[0]=std::byte(2);s.write("value",0,p);s.commit();s.checkpoint([&](size_t point){if(point==stage)kill_now();});},true);
  {Storage s(d,65536);assert(s.read("value",0)[0]==std::byte(2));s.begin();Page p{};p[0]=std::byte(3);s.write("value",0,p);s.commit();}
  {Storage s(d,65536);assert(s.read("value",0)[0]==std::byte(3));assert(std::filesystem::file_size(d/"wal")==0);}
 }
 // Cumulative epochs: a stolen page must retain its checkpoint original,
 // even when an earlier committed transaction has already changed disk.
 d=base/"epoch";
 {Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(1);p[1]=std::byte(9);s.write("v",0,p);s.commit();}
 for(auto& e:std::filesystem::directory_iterator(d))assert(e.path().filename().string().find("snapshot.")!=0);
 child([&]{Storage s(d,65536);Page p=s.read("v",0);s.begin();p[0]=std::byte(2);s.write("v",0,p);s.commit();
   s.begin();p[1]=std::byte(8);s.write("v",0,p);s.commit();
   s.begin();p[0]=std::byte(3);p[1]=std::byte(7);s.write("v",0,p);
   for(int i=0;i<30;i++)s.write("pressure",i,p);
   kill_now();},true);
 for(int i=0;i<2;i++)child([&]{Storage s(d,65536,[](size_t n){if(n==1)kill_now();});},true);
 {Storage s(d,65536);auto p=s.read("v",0);assert(p[0]==std::byte(2)&&p[1]==std::byte(8));}
 // Fresh index allocation never appends page byte undo, including repeated
 // updates; sealing enables statement rollback for later DML in the same tx.
 d=base/"regions";
 child([&]{Storage s(d,65536);s.begin();auto mark=s.savepoint();auto region=s.begin_region("index");Page p{};
   for(int i=0;i<50;i++){p[0]=std::byte(1);s.write_new(region+".p",i,p);p[0]=std::byte(2);s.write(region+".p",i,p);}
   assert(s.undo_spill_bytes==0);s.seal_region(region);auto after=s.savepoint();p[0]=std::byte(3);s.write(region+".p",0,p);s.rollback_to(after);assert(s.read(region+".p",0)[0]==std::byte(2));
   s.rollback_to(mark);assert(!std::filesystem::exists(d/"data"/(region+".p")));s.commit();kill_now();},true);
 {Storage s(d,65536);for(auto& e:std::filesystem::directory_iterator(d/"data"))assert(e.path().extension()!=".p"||std::filesystem::file_size(e.path())==0);}
 // Rollback across compressed chunks must survive committed restart.
 d=base/"savepoint";child([&]{Storage s(d,32<<20);s.begin();Page p{};p[0]=std::byte(1);s.write("v",0,p);auto mark=s.savepoint();for(int i=0;i<100;i++){p[0]=std::byte(2);s.write("v",i,p);}s.rollback_to(mark);s.commit();kill_now();},true);
 {Storage s(d,65536);assert(s.read("v",0)[0]==std::byte(1));assert(s.read("v",99)[0]==std::byte(0));}
 // A complete frame with a bad CRC is rejected. This diagnostic test
 // deliberately edits the log; arbitrary post-ack external truncation or
 // corruption is outside the SIGKILL contract and is not repairable here.
 d=base/"crc";
 child([&]{Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(2);s.write("v",0,p);s.commit();kill_now();},true);
 {auto path=d/"wal";auto n=std::filesystem::file_size(path);
  std::fstream stream(path,std::ios::in|std::ios::out|std::ios::binary);stream.seekg(n-1);char b;stream.get(b);b^=1;stream.seekp(n-1);stream.put(b);}
 {bool rejected=false;try{Storage s(d,65536);}catch(const std::runtime_error&){rejected=true;}assert(rejected);}
 // A partial in-flight frame after a complete WAL COMMIT is discarded:
 // restore the stolen pages and replay only committed transactions.
 d=base/"unacknowledged_tail";
 child([&]{Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(4);s.write("v",0,p);s.commit();s.begin();p[0]=std::byte(9);s.write("v",0,p);for(int i=0;i<20;i++)s.write("loser",i,p);kill_now();},true);
 std::filesystem::resize_file(d/"wal",std::filesystem::file_size(d/"wal")-3);
 {Storage s(d,65536);assert(s.read("v",0)[0]==std::byte(4));assert(s.read("loser",0)[0]==std::byte(0));}
 // A short newly appended committed page is repaired by intact physical redo.
 d=base/"short_page";
 child([&]{Storage s(d,65536);s.begin();Page p{};p[0]=std::byte(12);p[PAGE-1]=std::byte(34);s.write("v",0,p);s.commit();for(int i=0;i<10;i++)s.read("pressure",i);assert(std::filesystem::file_size(d/"data"/"v")==PAGE);kill_now();},true);
 std::filesystem::resize_file(d/"data"/"v",512);
 {Storage s(d,65536);auto p=s.read("v",0);assert(p[0]==std::byte(12)&&p[PAGE-1]==std::byte(34));}
 // Dense primary pages still split and preserve lookup/erase/rollback with
 // the minimum three-frame pool.
 d=base/"primary";
 {Storage s(d,65536);PrimaryIndex index(s,"primary");s.begin();for(int i=0;i<2500;i++)index.insert(i,SlotId(i+1));s.commit();
   for(int i=0;i<2500;i++)assert(index.find(i)==SlotId(i+1));
   s.begin();auto mark=s.savepoint();for(int i=0;i<100;i++){index.erase(i);index.insert(3000+i,SlotId(i+1));}s.rollback_to(mark);s.commit();
   for(int i=0;i<100;i++){assert(index.find(i)==SlotId(i+1));assert(!index.find(3000+i));}
   s.begin();for(int i=0;i<2500;i+=17){index.erase(i);assert(!index.find(i));index.insert(i,SlotId(i+10001));}s.commit();assert(s.budget.peak<=s.budget.limit);}
 {Storage s(d,65536);PrimaryIndex index(s,"primary");for(int i=0;i<2500;i++)assert(index.find(i)==SlotId(i%17==0?i+10001:i+1));}
 // A numeric phase2 manifest is rejected before touching existing data.
 d=base/"legacy";std::filesystem::create_directories(d/"data");{std::ofstream f(d/"CURRENT");f<<"1\n";}{std::ofstream f(d/"data"/"keep");f<<"legacy";}
 bool rejected=false;try{Storage s(d,65536);}catch(const std::runtime_error& e){rejected=std::string(e.what()).find("phase2")!=std::string::npos;}assert(rejected);assert(std::filesystem::file_size(d/"data"/"keep")==6);
 std::cout<<"storage regressions passed\n";
}
