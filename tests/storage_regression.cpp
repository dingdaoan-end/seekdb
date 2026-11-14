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
#include <sys/mman.h>
using namespace mvdb;
// Deterministically emulate filesystems rejecting O_DIRECT at open or first
// I/O. The kernel handles all operations outside each narrowly scoped test.
namespace {
int reject_direct=0;unsigned direct_rejections=0;
int kill_append_write_stage=0;
bool audit_io=false;
unsigned direct_opens=0,data_syncs=0,full_syncs=0,cache_drops=0,data_file_syncs=0,wal_file_syncs=0;
unsigned extent_write_calls=0;
unsigned sync_interruptions=0,index_read_interruptions=0,index_write_interruptions=0;
std::string descriptor_path(int fd){
 char path[512];auto link="/proc/self/fd/"+std::to_string(fd);auto n=::readlink(link.c_str(),path,sizeof(path));
 return n>0?std::string(path,static_cast<size_t>(n)):std::string{};
}
bool shadow_index(int fd){return descriptor_path(fd).find("/index/")!=std::string::npos;}
void reset_audit(){extent_write_calls=0;direct_opens=data_syncs=full_syncs=cache_drops=data_file_syncs=wal_file_syncs=0;}
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
 if(audit_io&&descriptor_path(fd).ends_with("/data/t.v1"))++extent_write_calls;
 if(kill_append_write_stage&&descriptor_path(fd).ends_with("/data/t.v1")){
  if(kill_append_write_stage==1)::kill(::getpid(),SIGKILL);
  if(kill_append_write_stage==3)n=std::min<size_t>(n,113);
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
// Page batches may stop mid-statement or mid-pwrite. Only normally logged
// metadata publishes rows, and guarded files retain savepoint byte undo.
void batched_append(){
 std::array<std::byte,8192> batch;batch.fill(std::byte(42));
 for(int stage=1;stage<=3;stage++){
  auto d=base/("batch_mid_write"+std::to_string(stage));
  child([&]{Storage s(d,65536);s.begin();kill_append_write_stage=stage;
   s.append_bytes("t.v1",0,512,batch.data(),batch.size());},true);
  Storage s(d,65536);assert(s.read("t.header",0)[0]==std::byte(0));
  auto bytes=s.read("t.v1",0);assert(bytes[0]==std::byte(0));
  assert(std::filesystem::file_size(d/"data"/"t.v1")==PAGE);
  if(stage==2)assert(bytes[512+batch.size()-1]==std::byte(42));
  if(stage==3)assert(bytes[512]==std::byte(42)&&bytes[512+113]==std::byte(0));
 }
 for(bool committed:{false,true}){
  auto d=base/(committed?"batch_commit":"batch_partial_rollback");
  child([&]{Storage s(d,65536);s.begin();Page h{};auto mark=s.savepoint();
   auto wal=s.wal_bytes_written;auto bytes=s.pool.data_page_write_bytes;reset_audit();audit_io=true;
   s.append_bytes("t.v1",0,512,batch.data(),batch.size());
   s.append_bytes("t.v1",1,0,batch.data(),batch.size());
   audit_io=false;assert(extent_write_calls==2);assert(s.pool.data_page_write_bytes-bytes==2*batch.size());
   assert(s.savepoint()==mark&&s.wal_bytes_written==wal);
   h[0]=std::byte(32);s.write("t.header",0,h);s.rollback_to(mark);
   assert(s.read("t.header",0)[0]==std::byte(0));
   std::array<std::byte,512> reused;reused.fill(std::byte(77));
   s.append_bytes("t.v1",0,512,reused.data(),reused.size());
   h[0]=std::byte(1);s.write("t.header",0,h);if(committed)s.commit();kill_now();},true);
  Storage s(d,65536);assert(s.read("t.header",0)[0]==std::byte(committed?1:0));
  assert(s.read("t.v1",0)[512]==std::byte(77));
 }
 auto d=base/"batch_guarded_savepoint";
 child([&]{Storage s(d,32<<20);s.begin();Page p{};p[0]=std::byte(9);s.write("t.v1",0,p);
  auto mark=s.savepoint();s.append_bytes("t.v1",1,0,batch.data(),batch.size());
  assert(s.savepoint()>mark);s.rollback_to(mark);assert(s.read("t.v1",1)[0]==std::byte(0));
  s.append_bytes("t.v1",1,0,batch.data(),batch.size());s.commit();kill_now();},true);
 {Storage s(d,65536);assert(s.read("t.v1",0)[0]==std::byte(9));assert(s.read("t.v1",1)[0]==std::byte(42));}
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
// Exhaustive small intervals plus randomized, deliberately unaligned buffers.
// The inaccessible next page catches even a one-byte read beyond ceil(end/8).
void bitmap_words(){
 size_t cases=0;std::mt19937_64 random(0xb17b17);
 auto check=[&](const std::byte* bytes,size_t begin,size_t end,bool set){
  size_t expected=begin;while(expected<end&&bool((std::to_integer<unsigned>(bytes[expected/8])>>(expected%8))&1)!=set)++expected;
  assert(detail::bitmap_next(bytes,begin,end,set)==expected);++cases;
 };
 assert(detail::bitmap_next(nullptr,0,0,true)==0);
 for(unsigned pattern=0;pattern<256;++pattern){std::array<std::byte,17> bytes;bytes.fill(std::byte(pattern));
  for(size_t begin=0;begin<=128;++begin)for(size_t end=begin;end<=128;++end)for(bool set:{false,true})check(bytes.data()+1,begin,end,set);
 }
 for(size_t alignment=0;alignment<8;++alignment){std::array<std::byte,265> bytes;
  for(int trial=0;trial<4096;++trial){for(auto& b:bytes)b=std::byte(random());size_t end=random()%2049,begin=random()%(end+1);
   for(bool set:{false,true})check(bytes.data()+alignment,begin,end,set);
  }
 }
 auto page_size=static_cast<size_t>(::sysconf(_SC_PAGESIZE));
 auto mapping=static_cast<std::byte*>(::mmap(nullptr,2*page_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(mapping!=MAP_FAILED);
 assert(::mprotect(mapping+page_size,page_size,PROT_NONE)==0);
 for(size_t bits=1;bits<=1024;++bits){auto bytes=mapping+page_size-(bits+7)/8;
  for(bool fill:{false,true}){std::memset(bytes,fill?255:0,(bits+7)/8);
   for(bool set:{false,true})for(size_t begin:{size_t(0),bits/2,bits-1,bits})check(bytes,begin,bits,set);
  }
 }
 assert(::munmap(mapping,2*page_size)==0);std::cout<<"bitmap oracle cases="<<cases<<"\n";
}


// Independent bytewise R9 encoder: retain short equal gaps exactly, including
// eight-byte gaps and the final byte. Test the actual production encoder seam.
std::string bytewise_delta(const std::string& file,uint64_t page,const Page& old,const Page& next){
 std::string out;
 auto append=[&](auto value){out.append(reinterpret_cast<const char*>(&value),sizeof(value));};
 append(static_cast<uint8_t>(file.size()));out+=file;append(page);
 size_t i=0;while(i<PAGE){while(i<PAGE&&old[i]==next[i])++i;if(i==PAGE)break;
  size_t start=i++,end=i;while(i<PAGE){if(old[i]!=next[i])end=i+1;if(i>=end+8)break;++i;}
  append(static_cast<uint16_t>(start));append(static_cast<uint16_t>(end-start));
  out.append(reinterpret_cast<const char*>(next.data()+start),end-start);i=end;
 }return out;
}
void decode_test_delta(const std::string& encoded,Page& page){
 size_t pos=1+std::to_integer<unsigned>(std::byte(encoded[0]))+sizeof(uint64_t);
 while(pos<encoded.size()){
  uint16_t offset,n;assert(pos+4<=encoded.size());std::memcpy(&offset,encoded.data()+pos,2);std::memcpy(&n,encoded.data()+pos+2,2);pos+=4;
  assert(n&&offset+n<=PAGE&&pos+n<=encoded.size());std::memcpy(page.data()+offset,encoded.data()+pos,n);pos+=n;
 }
 assert(pos==encoded.size());
}
void delta_oracle(){
 uint64_t cases=0,encoded_bytes=0;
 auto check=[&](const Page& a,const Page& b){
  const std::string name="delta.file.v1";constexpr uint64_t page=0x1020304050607080ULL;
  auto next=detail::page_delta(name,page,a,b),forward=bytewise_delta(name,page,a,b);
  assert(next==forward);encoded_bytes+=next.size();
  auto reverse=detail::page_delta(name,page,b,a);assert(reverse==bytewise_delta(name,page,b,a));
  Page actual=a;decode_test_delta(next,actual);assert(actual==b);
  decode_test_delta(reverse,actual);assert(actual==a);++cases;
 };
 struct OffsetPage{std::byte prefix;Page page;};
  std::mt19937_64 random(0x5afed311);
  alignas(64) std::array<OffsetPage,8> old_slots{},new_slots{};unsigned alignments=0;
  for(size_t alignment=0;alignment<8;++alignment){
    auto& old=old_slots[alignment].page;auto& next=new_slots[alignment].page;
    alignments|=1U<<(reinterpret_cast<uintptr_t>(old.data())%8);
    for(auto& byte:old)byte=std::byte(random()&255);
    next=old;check(old,next);
    for(size_t offset=alignment;offset<PAGE;offset+=8){next=old;next[offset]^=std::byte{0xff};check(old,next);}
    for(size_t gap=1;gap<=24;++gap)for(size_t offset=alignment;offset<PAGE;offset+=128){
      next=old;next[offset]^=std::byte{1};if(offset+gap<PAGE)next[offset+gap]^=std::byte{2};check(old,next);
    }
    for(size_t width:{size_t(1),size_t(7),size_t(8),size_t(9),size_t(15),size_t(16),size_t(17),size_t(63),size_t(64),size_t(65),size_t(4095),size_t(4096),size_t(4097),PAGE}){
      next=old;for(size_t i=PAGE-width;i<PAGE;++i)next[i]^=std::byte{0xaa};check(old,next);
    }
  }
  assert(alignments==255);Page old{},next{};
  for(int iteration=0;iteration<4096;++iteration){
    for(auto& byte:old)byte=std::byte(random()&255);
    next=old;
    switch(iteration%6){
      case 0:break;
      case 1:next[random()%PAGE]^=std::byte{1};break;
      case 2:for(int i=0;i<23;++i)next[random()%PAGE]=std::byte(random()&255);break;
      case 3:{size_t first=random()%PAGE,n=std::min<size_t>(1+random()%2048,PAGE-first);for(size_t i=first;i<first+n;++i)next[i]^=std::byte{0x5a};break;}
      case 4:for(size_t i=0;i<PAGE;i+=1+random()%15)next[i]=std::byte(random()&255);break;
      case 5:for(auto& byte:next)byte=std::byte(random()&255);break;
    }check(old,next);
  }

 assert(cases==45176);
 std::cout<<"delta oracle cases="<<cases<<" forward_bytes="<<encoded_bytes<<" pointer_alignments=8 redo_undo=exact\n";
}

void primary_split_rollback(){
 auto d=base/"primary_split_rollback";
 {Storage s(d,65536);PrimaryIndex index(s,"primary");s.begin();
  // Fill a single leaf, then roll back the insertion that creates a new root.
  for(int i=999;i>=0;--i)index.insert(i,SlotId(i+1));
  s.commit();s.begin();auto mark=s.savepoint();
  index.insert(1000,1001);assert(index.find(1000)==1001);s.rollback_to(mark);
  assert(!index.find(1000));for(int i=0;i<1000;++i)assert(index.find(i)==SlotId(i+1));
  // Keep physical slot identifiers in input order despite sorted insertion.
  for(int i=1000;i<2500;++i)index.insert(i,SlotId(9000-i));
  s.commit();assert(s.budget.peak<=s.budget.limit);
 }
 {Storage s(d,65536);PrimaryIndex index(s,"primary");for(int i=0;i<2500;++i)assert(index.find(i)==SlotId(i<1000?i+1:9000-i));}
 std::cout<<"primary split rollback and ordered slot mapping passed\n";
}

int main(int argc,char** argv){
 ::unsetenv("MVDB_USE_ODIRECT");
 if(argc==2){std::filesystem::remove_all(base);std::string test=argv[1];
  if(test=="delta")delta_oracle();else if(test=="primary")primary_split_rollback();else if(test=="bitmap")bitmap_words();else if(test=="buffered")buffered_default();else if(test=="metadata")metadata_scan();else if(test=="sync")transaction_sync_boundary();else if(test=="interrupt")interrupted_io();else if(test=="append")invisible_append();else if(test=="resident")resident_handles();else if(test=="batch")batched_append();else return 2;return 0;}
 std::filesystem::remove_all(base);delta_oracle();primary_split_rollback();bitmap_words();buffered_default();metadata_scan();transaction_sync_boundary();interrupted_io();invisible_append();batched_append();resident_handles();
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
