#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/storage.hpp"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace mvdb;
namespace {
unsigned drops=0;
bool audit_drop=true;
bool fail_drop=false,fail_sync=false;
int fail_drop_after=-1;
unsigned sync_calls=0;
size_t file_cached_pages(const std::filesystem::path& path,size_t bytes){
 int fd=::open(path.c_str(),O_RDONLY);assert(fd>=0);
 void* mapping=::mmap(nullptr,bytes,PROT_READ,MAP_SHARED,fd,0);assert(mapping!=MAP_FAILED);
 std::vector<unsigned char> resident((bytes+4095)/4096);assert(::mincore(mapping,bytes,resident.data())==0);
 size_t count=0;for(auto n:resident)count+=(n&1);::munmap(mapping,bytes);::close(fd);return count;
}
Page filled(unsigned n){Page p{};p.fill(std::byte(n));return p;}
void release_and_coherence(const std::filesystem::path& root){
 MemoryBudget budget(1<<20);BufferPool pool(root,budget);auto reserved=budget.used;
 pool.append_extent("table.v0",0,filled(7),0,PAGE);
 pool.append_extent("table.blob",0,filled(8),0,PAGE);
 auto stale=pool.handle("table.v0",0);auto neighbor=pool.handle("table.blob",0);
 assert(pool.allocated_page_bytes()==2*PAGE);
 {auto guard=pool.lease("table.v0",0);assert(!pool.release_clean_file("table.v0"));assert(pool.resident(stale)==guard.data);}
 assert(pool.release_clean_file("table.v0"));assert(!pool.resident(stale));assert(pool.resident(neighbor));
 assert(pool.allocated_page_bytes()==PAGE&&budget.used==reserved);
 assert(pool.get("table.v0",0)==filled(7));
 pool.put("table.v0",0,filled(9));assert(!pool.release_clean_file("table.v0"));
 pool.flush_all();assert(pool.release_clean_file("table.v0"));assert(pool.get("table.v0",0)==filled(9));
 pool.transaction_active=true;pool.put("table.v0",0,filled(10));pool.flush_all();
 assert(!pool.release_clean_file("table.v0"));pool.committed();assert(pool.release_clean_file("table.v0"));
 assert(pool.get("table.v0",0)==filled(10));assert(drops==0);
 pool.append_extent("table.v0",1,filled(11),0,PAGE);
 auto first=pool.handle("table.v0",0),second=pool.handle("table.v0",1);
 {auto guard=pool.lease("table.v0",1);assert(!pool.release_clean_file("table.v0"));
  assert(pool.resident(first)&&pool.resident(second));}
 pool.put("table.v0",1,filled(12));assert(!pool.release_clean_file("table.v0"));
 assert(pool.resident(first)&&pool.resident(second));pool.flush_all();
 assert(pool.release_clean_file("table.v0"));assert(!pool.resident(first)&&!pool.resident(second));
}
size_t anonymous_kib(){
 std::ifstream in("/proc/self/status");std::string key,line;size_t value=0;
 while(in>>key){if(key=="RssAnon:"){in>>value;return value;}std::getline(in,line);}return 0;
}
void completed_appends(const std::filesystem::path& root){
 ::unsetenv("MVDB_APPEND_RELEASE");
 MemoryBudget budget(32<<20);BufferPool pool(root,budget);auto reserved=budget.used;auto start_drops=drops;
 for(unsigned i=0;i<512;++i)pool.append_extent(i%2?"a.v0":"b.blob",i/2,filled(i%251+1),0,PAGE);
 assert(pool.allocated_page_bytes()==0&&pool.resident_page_count()==0&&pool.clean_page_releases==512);
 assert(budget.used==reserved&&drops==start_drops);
 pool.append_extent("a.v0",256,filled(17),0,PAGE/2);
 assert(pool.allocated_page_bytes()==PAGE);
 {auto guard=pool.lease("a.v0",256);pool.append_extent("a.v0",256,filled(17),PAGE/2,PAGE/2);
  assert(guard.data[0]==std::byte(17)&&guard.data[PAGE-1]==std::byte(17));assert(pool.allocated_page_bytes()==PAGE);}
 assert(pool.release_clean_page("a.v0",256));assert(pool.allocated_page_bytes()==0);
 for(unsigned i=0;i<512;++i)assert(pool.get(i%2?"a.v0":"b.blob",i/2)==filled(i%251+1));
 auto before=anonymous_kib(),backing=pool.allocated_page_bytes();assert(backing==512*PAGE);
 assert(pool.release_clean_file("a.v0")&&pool.release_clean_file("b.blob"));
 auto after=anonymous_kib();assert(pool.allocated_page_bytes()==0&&budget.used==reserved);
 std::cout<<"completed append release: backing "<<backing<<" -> 0 bytes; RssAnon "<<before<<" -> "<<after<<" KiB; no kernel drop\n";
 // Explicit O_DIRECT requests use the existing buffered-extent fallback.
 ::setenv("MVDB_USE_ODIRECT","1",1);pool.append_extent("direct.v1",0,filled(33),0,PAGE);
 assert(pool.get("direct.v1",0)==filled(33));::unsetenv("MVDB_USE_ODIRECT");
}
void prepared_residency(const std::filesystem::path& root){
 MemoryBudget budget(4<<20);BufferPool pool(root,budget);
 constexpr size_t count=64;
 for(unsigned p=0;p<count;++p)pool.append_extent("prepare.v0",p,filled(p+1),0,PAGE);
 auto base_drops=drops;auto reserved=budget.used;
 {MemoryBudget tiny_budget(65536);BufferPool tiny(root/"tiny",tiny_budget);
  assert(!tiny.load_residency("tiny.v0",1,0)&&drops==base_drops);}
 auto remaining=budget.limit-budget.used;budget.acquire(remaining);
 assert(!pool.load_residency("prepare.v0",count,32)&&drops==base_drops&&pool.pinned_page_count()==0);
 budget.release(remaining);
 assert(!pool.load_residency("prepare.v0",pool.frame_capacity(),32)&&drops==base_drops);
 {auto unrelated=pool.lease("other.v0",0);
  assert(!pool.load_residency("prepare.v0",count,32)&&drops==base_drops);}
 pool.put("prepare.v0",0,filled(9)); // committed dirty frame: preparation may flush it.
 pool.transaction_active=true;
 auto sync_before=sync_calls;auto lease=pool.load_residency("prepare.v0",count,32);assert(lease);
 assert(sync_calls==sync_before+1&&drops==base_drops+count&&pool.pinned_page_count()==count);
 assert(file_cached_pages(root/"prepare.v0",count*PAGE)==0);
 auto transferred=std::move(*lease);lease.reset();assert(pool.pinned_page_count()==count);
 bool refused=false;try{pool.invalidate_prefix("prepare");}catch(const std::runtime_error&){refused=true;}assert(refused&&!pool.release_clean_file("prepare.v0"));
 auto before=pool.physical_reads;
 for(unsigned pass=0;pass<3;++pass)for(unsigned p=0;p<count;++p)
  assert(pool.get("prepare.v0",p)==filled(p? p+1:9));
 assert(pool.physical_reads==before);transferred.reset();assert(budget.used==reserved);
 pool.put("prepare.v0",0,filled(10));base_drops=drops;
 assert(!pool.load_residency("prepare.v0",count,32)&&drops==base_drops);
 pool.flush_all();assert(!pool.load_residency("prepare.v0",count,32)&&drops==base_drops);
 pool.committed();pool.release_clean_file("prepare.v0");
 auto sync_start=sync_calls;fail_sync=true;bool sync_failed=false;
 try{pool.load_residency("prepare.v0",count,32);}catch(const std::runtime_error&){sync_failed=true;}
 fail_sync=false;assert(sync_failed&&sync_calls==sync_start+1&&drops==base_drops&&pool.pinned_page_count()==0);
 fail_drop_after=3;bool failed=false;
 try{pool.load_residency("prepare.v0",count,32);}catch(const std::runtime_error&){failed=true;}
 fail_drop_after=-1;assert(failed&&pool.pinned_page_count()==0&&budget.used==reserved);
 assert(file_cached_pages(root/"prepare.v0",count*PAGE)>=3*PAGE/4096);
 for(unsigned p=0;p<count;++p)assert(pool.get("prepare.v0",p)==filled(p? p+1:10));
 std::cout<<"explicit prepare: 64 incremental page drops, one sync, zero repeated reads; failure restoration and refusal passed\n";
}
void multi_file_refusal(const std::filesystem::path& root){
 std::filesystem::create_directories(root);Page bytes=filled(23);
 for(unsigned f=0;f<4;++f){std::ofstream out(root/("multi"+std::to_string(f)+".v0"),std::ios::binary);for(unsigned p=0;p<256;++p)out.write(reinterpret_cast<const char*>(bytes.data()),PAGE);}
 MemoryBudget budget(10*1024*1024);BufferPool pool(root,budget);auto before=drops;
 for(unsigned p=0;p<256;++p)for(unsigned f=0;f<4;++f)assert(pool.get("multi"+std::to_string(f)+".v0",p)==bytes);
 assert(!pool.load_residency("multi0.v0",256,32)&&drops==before&&pool.pinned_page_count()==0);
 std::cout<<"4x4MiB overflow: zero kernel drops despite individual 4MiB files fitting the pool\n";
}
void transactions(const std::filesystem::path& root){
 {
  Storage storage(root,1<<20);storage.begin();auto mark=storage.savepoint();
  storage.append("t.v0",0,filled(5),0,PAGE);assert(storage.savepoint()==mark);
  assert(storage.pool.allocated_page_bytes()==0);
  storage.write("t.header",0,filled(1));storage.commit();storage.checkpoint();
  storage.begin();storage.write("t.v0",0,filled(6));storage.pool.flush_all();
  assert(!storage.pool.release_clean_file("t.v0"));storage.rollback();
  assert(storage.read("t.v0",0)==filled(5));
  storage.begin();auto page=storage.read("t.v0",0);page[PAGE-1]=std::byte(42);mark=storage.savepoint();
  storage.append("t.v0",0,page,PAGE-1,1);assert(storage.savepoint()>mark);
  assert(!storage.pool.release_clean_page("t.v0",0));storage.rollback();
  assert(storage.read("t.v0",0)==filled(5));storage.close();
 }
 auto child=::fork();assert(child>=0);
 if(child==0){
  Storage storage(root,1<<20);storage.begin();storage.append("t.v0",1,filled(7),0,PAGE);
  storage.write("t.header",0,filled(2));storage.commit();
  storage.begin();storage.write("t.v0",0,filled(99));storage.pool.flush_all();
  ::kill(::getpid(),SIGKILL);_exit(2);
 }
 int status=0;assert(::waitpid(child,&status,0)==child);assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
 for(unsigned i=0;i<2;++i){Storage storage(root,65536);
  assert(storage.read("t.header",0)==filled(2));assert(storage.read("t.v0",0)==filled(5));assert(storage.read("t.v0",1)==filled(7));}
 std::cout<<"append release + ordinary WAL update/rollback + committed append/uncommitted overwrite SIGKILL recovery passed\n";
}
}
extern "C" int posix_fadvise(int fd,off_t offset,off_t length,int advice){
 if(advice==POSIX_FADV_DONTNEED&&fail_drop_after>=0){if(!fail_drop_after)return EIO;--fail_drop_after;}
 if(fail_drop&&advice==POSIX_FADV_DONTNEED)return EIO;
 if(audit_drop&&advice==POSIX_FADV_DONTNEED){assert(offset%PAGE==0&&length>0);++drops;}
 auto result=::syscall(SYS_fadvise64,fd,offset,length,advice);return result<0?errno:0;
}
extern "C" int fdatasync(int fd){
 ++sync_calls;
 if(fail_sync){errno=EIO;return -1;}return static_cast<int>(::syscall(SYS_fdatasync,fd));
}
int main(){
 ::setenv("MVDB_APPEND_RELEASE","off",1);
 auto pattern=(std::filesystem::temp_directory_path()/"mvdb-structural-cache-XXXXXX").string();
 std::vector<char> temp(pattern.begin(),pattern.end());temp.push_back('\0');
 assert(::mkdtemp(temp.data()));auto root=std::filesystem::path(temp.data());
 release_and_coherence(root/"release");
 completed_appends(root/"completed");prepared_residency(root/"prepared");multi_file_refusal(root/"multi");audit_drop=false;transactions(root/"transactions");
 std::filesystem::remove_all(root);std::cout<<"structural cache release/coherence and atomic residency passed\n";
}
