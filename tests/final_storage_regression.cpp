#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/storage.hpp"
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <fcntl.h>

using namespace mvdb;
namespace {
bool audit_writes=false;
uint64_t write_calls=0,write_bytes=0;
bool audit_cache_drop=false;
unsigned cache_drops=0;
void audit_begin(){write_calls=write_bytes=0;audit_writes=true;}
void audit_end(){audit_writes=false;assert(write_calls==0&&write_bytes==0);}
Page filled(unsigned value){Page p{};p.fill(std::byte(value));return p;}
void save(const std::filesystem::path& file,const Page& page,size_t count=1){
 std::ofstream out(file,std::ios::binary|std::ios::trunc);
 for(size_t i=0;i<count;++i)out.write(reinterpret_cast<const char*>(page.data()),PAGE);
 assert(out.good());
}
Page disk_page(const std::filesystem::path& file){Page p{};std::ifstream in(file,std::ios::binary);in.read(reinterpret_cast<char*>(p.data()),PAGE);assert(in.gcount()==PAGE);return p;}

void lazy_pages(const std::filesystem::path& root){
 for(size_t limit:{size_t(65536),size_t(4<<20),size_t(8<<20),size_t(16<<20)}){
  MemoryBudget budget(limit);
  {
   BufferPool pool(root/std::to_string(limit),budget);
   auto reserved=budget.used;assert(reserved>3*PAGE&&reserved<=limit);
   assert(pool.allocated_page_bytes()==0);
   std::vector<BufferPool::ResidentHandle> handles;
   for(unsigned i=0;i<3;++i){
    pool.put("cold",i,filled(40+i));handles.push_back(pool.handle("cold",i));
    assert(pool.allocated_page_bytes()==(i+1)*PAGE);assert(budget.used==reserved);
   }
   for(unsigned pass=0;pass<20;++pass)for(unsigned i=0;i<3;++i){
    assert(pool.get("cold",i)==filled(40+i));assert(pool.resident(handles[i]));
    assert(reinterpret_cast<uintptr_t>(pool.resident(handles[i]))%4096==0);
   }
   pool.flush_all();audit_cache_drop=true;pool.sync_all();audit_cache_drop=false;
   assert(cache_drops==0&&pool.allocated_page_bytes()==3*PAGE);
   pool.invalidate_prefix("cold");assert(pool.allocated_page_bytes()==0);
   for(auto h:handles)assert(!pool.resident(h));
   for(unsigned i=0;i<3;++i)assert(pool.get("cold",i)==filled(40+i));
   assert(pool.allocated_page_bytes()==3*PAGE&&budget.used==reserved);
  }
  assert(budget.used==0);
 }
 // Saturate fewer than 500 frames using leases, then repeatedly reuse every
 // backing allocation and verify both live and evicted handle identities.
 MemoryBudget budget(8<<20);BufferPool pool(root/"saturated",budget);
 std::vector<BufferPool::PageGuard> guards;
 std::vector<BufferPool::ResidentHandle> old;
 for(unsigned i=0;i<500;++i){
  try{auto guard=pool.lease("saturated",i);guards.push_back(std::move(guard));}
  catch(const std::runtime_error&){break;}
  pool.put("saturated",i,filled(i%251+1));old.push_back(pool.handle("saturated",i));
 }
 assert(guards.size()>=3&&guards.size()<500);const size_t count=guards.size();
 assert(pool.allocated_page_bytes()==count*PAGE);
 for(size_t i=0;i<count;++i)assert(pool.resident(old[i])==guards[i].data);
 guards.clear();pool.flush_all();
 for(unsigned pass=0;pass<3;++pass){
  for(size_t i=count;i<2*count;++i)assert(pool.get("pressure",i)==Page{});
  for(auto h:old)assert(!pool.resident(h));
  for(size_t i=0;i<count;++i)assert(pool.get("saturated",i)==filled(i%251+1));
  assert(pool.allocated_page_bytes()==count*PAGE);
 }
 std::cout<<"lazy pages: cold=3, saturated="<<count<<", aligned backing reuse, budget and handle lifecycle passed\n";
}

void failed_replacement(const std::filesystem::path& root){
 MemoryBudget budget(65536);BufferPool pool(root,budget);
 pool.put("old",0,filled(1));auto old=pool.handle("old",0);
 pool.put("old",1,filled(2));pool.put("old",2,filled(3));
 std::ofstream broken(root/"short",std::ios::binary);broken.put('x');broken.close();
 bool rejected=false;try{pool.get("short",0);}catch(const std::runtime_error&){rejected=true;}
 assert(rejected&&!pool.resident(old));assert(pool.get("old",0)==filled(1));
}

// The pinned suffix must be found after the first matching frame. A rejected
// discard must preserve all dirty pages, handles and open-file associations.
void pinned_preflight(const std::filesystem::path& directory){
 MemoryBudget budget(65536);BufferPool pool(directory,budget);
 pool.put("index",0,filled(11));auto first=pool.handle("index",0);
 pool.put("index.nodes",0,filled(12));auto second=pool.handle("index.nodes",0);
 auto pinned=pool.lease("index.nodes",0);
 bool rejected=false;audit_begin();
 try{pool.invalidate_prefix("index");}catch(const std::runtime_error&){rejected=true;}
 audit_end();assert(rejected);
 assert(pool.resident(first)&&pool.resident(second));
 assert(pool.get("index",0)==filled(11));assert(pool.get("index.nodes",0)==filled(12));
 pinned.reset();audit_begin();pool.invalidate_prefix("index");pool.flush_all();audit_end();
 assert(!pool.resident(first)&&!pool.resident(second));
 std::cout<<"pinned discard preflight: all matching frames preserved; pwrite calls=0 bytes=0\n";
}

void discard_rebuild(const std::filesystem::path& root){
 const auto directory=root/"data",retired=root/"retired";
 std::filesystem::create_directories(directory);std::filesystem::create_directory(retired);
 save(directory/"index",filled(21),2);save(directory/"index.nodes",filled(22),2);
 save(directory/"index_extra",filled(23));save(directory/"index2.nodes",filled(24));
 {
  MemoryBudget budget(1<<20);BufferPool pool(directory,budget);
  pool.put("index",0,filled(91));pool.put("index.nodes",1,filled(92));
  auto first=pool.handle("index",0),second=pool.handle("index.nodes",1);
  auto neighbor=pool.handle("index_extra",0),neighbor2=pool.handle("index2.nodes",0);
  unsigned protects=0;pool.before_flush=[&](const std::string&,uint64_t){++protects;};
  const auto initial_bytes=pool.data_page_write_bytes;
  audit_begin();pool.invalidate_prefix("index");pool.flush_all();audit_end();
  assert(!pool.resident(first)&&!pool.resident(second));
  assert(pool.resident(neighbor)&&pool.resident(neighbor2));
  // Keep old inodes inspectable, and recreate the exact filename with a shorter
  // layout. A stale descriptor would read or write the retired generation.
  std::filesystem::rename(directory/"index",retired/"index");
  std::filesystem::rename(directory/"index.nodes",retired/"index.nodes");
  save(directory/"index",filled(31));save(directory/"index.leaves",filled(32),3);
  audit_begin();
  assert(pool.get("index",0)==filled(31));assert(pool.get("index",1)==Page{});
  assert(pool.get("index.leaves",2)==filled(32));assert(pool.get("index.nodes",1)==Page{});
  for(unsigned i=0;i<100;++i)assert(pool.get("pressure",i)==Page{});
  pool.flush_all();audit_end();
  assert(!pool.resident(first)&&!pool.resident(second));assert(protects==0);
  assert(pool.data_page_write_bytes==initial_bytes);
  assert(disk_page(retired/"index")==filled(21));assert(disk_page(retired/"index.nodes")==filled(22));
  assert(std::filesystem::file_size(directory/"index")==PAGE);
  assert(disk_page(directory/"index_extra")==filled(23));assert(disk_page(directory/"index2.nodes")==filled(24));
 }
 {
  MemoryBudget budget(65536);BufferPool reopened(directory,budget);
  assert(reopened.get("index",0)==filled(31));assert(reopened.get("index",1)==Page{});
  assert(reopened.get("index.leaves",2)==filled(32));
 }
 std::cout<<"dirty discard + same-name different-layout rebuild + reopen: pwrite calls=0 bytes=0\n";
}

void fresh_region_rollback(const std::filesystem::path& root,bool full_rollback){
 std::string old_region,new_region;
 {
  Storage storage(root,1<<20);storage.begin();auto mark=storage.savepoint();
  old_region=storage.begin_region("same_index");
  storage.write_new(old_region+".nodes",0,filled(51));storage.write_new(old_region+".nodes",1,filled(52));
  auto old=storage.pool.handle(old_region+".nodes",1);
  audit_begin();if(full_rollback)storage.rollback();else storage.rollback_to(mark);audit_end();
  assert(!storage.pool.resident(old));assert(!std::filesystem::exists(root/"data"/(old_region+".nodes")));
  if(full_rollback)storage.begin();
  new_region=storage.begin_region("same_index");assert(new_region!=old_region);
  storage.write_new(new_region+".leaves",2,filled(61));storage.seal_region(new_region);storage.commit();
  storage.close();
 }
 {
  Storage reopened(root,65536);assert(!std::filesystem::exists(root/"data"/(old_region+".nodes")));
  assert(reopened.read(new_region+".leaves",2)==filled(61));
 }
 std::cout<<(full_rollback?"full":"savepoint")<<" fresh-region rollback + rebuild + reopen: discard pwrite calls=0 bytes=0\n";
}

void crash_recovery(const std::filesystem::path& root){
 auto child=::fork();assert(child>=0);
 if(child==0){
  Storage storage(root,1<<20);storage.begin();auto mark=storage.savepoint();auto old=storage.begin_region("same_index");
  storage.write_new(old+".nodes",0,filled(71));storage.write_new(old+".nodes",1,filled(72));
  // Force original frames to disk so committed recovery must replay the region
  // discard record, rather than merely observing absent unflushed pages.
  storage.pool.flush_all();storage.rollback_to(mark);
  auto fresh=storage.begin_region("same_index");assert(fresh!=old);
  storage.write_new(fresh+".leaves",2,filled(81));storage.seal_region(fresh);storage.commit();
  ::kill(::getpid(),SIGKILL);_exit(2);
 }
 int status=0;assert(::waitpid(child,&status,0)==child);assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
 for(unsigned pass=0;pass<2;++pass){
  Storage recovered(root,65536);assert(!std::filesystem::exists(root/"data"/"same_index.r1.nodes"));
  assert(recovered.read("same_index.r2.leaves",2)==filled(81));
 }
 std::cout<<"fresh-region savepoint discard + rebuild committed WAL + SIGKILL recovery/reopen passed\n";
}
}
extern "C" ssize_t pwrite(int fd,const void* bytes,size_t size,off_t offset){
 if(audit_writes)++write_calls;
 auto result=::syscall(SYS_pwrite64,fd,bytes,size,offset);
 if(audit_writes&&result>0)write_bytes+=static_cast<uint64_t>(result);
 return result;
}
extern "C" int posix_fadvise(int fd,off_t offset,off_t length,int advice){
 if(audit_cache_drop&&advice==POSIX_FADV_DONTNEED)++cache_drops;
 auto result=::syscall(SYS_fadvise64,fd,offset,length,advice);
 return result<0?errno:0;
}
int main(){
 const auto root=std::filesystem::temp_directory_path()/("mvdb-discard-"+std::to_string(::getpid()));
 assert(!std::filesystem::exists(root));std::filesystem::create_directory(root);
 pinned_preflight(root/"pinned");discard_rebuild(root/"rebuild");
 lazy_pages(root/"lazy");failed_replacement(root/"failed");
 fresh_region_rollback(root/"savepoint",false);fresh_region_rollback(root/"rollback",true);
 crash_recovery(root/"crash");std::filesystem::remove_all(root);
}
