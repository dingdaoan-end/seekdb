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
using namespace mvdb;
const std::filesystem::path base="/tmp/mvdb_storage_phase3";
template<class F> void child(F action,bool killed=false){auto c=fork();assert(c>=0);if(!c){action();_exit(0);}int status;assert(waitpid(c,&status,0)==c);assert(killed?WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL:WIFEXITED(status)&&WEXITSTATUS(status)==0);}
void kill_now(){::kill(getpid(),SIGKILL);}
int main(){std::filesystem::remove_all(base);
 auto d=base/"compact";
 {Storage s(d,32<<20);s.begin();std::mt19937 random(7);Page p{},meta{};
  for(int i=0;i<100;i++){for(int j=0;j<128;j++){float v=float(random()%100000)/100000;std::memcpy(p.data()+j*4,&v,4);}s.write("t.v1",i,p);meta[0]=std::byte(i);s.write("t.meta",0,meta);}
  s.commit();for(auto& e:std::filesystem::directory_iterator(d))assert(e.path().filename().string().rfind("committed.",0)!=0);auto n=std::filesystem::file_size(d/"wal");std::cout<<"128D row WAL bytes="<<n/100.0<<" undo spill="<<s.undo_spill_bytes<<"\n";assert(n<=60000);assert(s.shadow_bytes_written==0);assert(s.undo_spill_bytes==0);assert(s.budget.peak<=s.budget.limit);s.close();assert(s.shadow_bytes_written==0);}
 // Consecutive ordinary batches cross absolute 64KiB file offsets, but each
 // remains below the dirty-window limit and has exactly one WAL fdatasync.
 d=base/"single_sync";
 {Storage s(d,32<<20);std::mt19937 random(17);
  for(int batch=0;batch<4;batch++){auto syncs=s.wal_syncs,bytes=s.wal_bytes_written;s.begin();Page p{},meta{};
   for(int i=0;i<100;i++){for(int j=0;j<128;j++){float v=float(random()%100000)/100000;std::memcpy(p.data()+j*4,&v,4);}s.write("t.v1",batch*100+i,p);meta[0]=std::byte(i);for(int j=8;j<104;j++)meta[j]=std::byte(random()&255);s.write("t.meta",0,meta);}
   s.commit();auto written=s.wal_bytes_written-bytes;std::cout<<"100-row WAL bytes="<<written<<" syncs="<<s.wal_syncs-syncs<<"\n";assert(written>54*1024&&written<64*1024);assert(s.wal_syncs-syncs==1);
  }
  // Large transactions still drain bounded WAL windows rather than retaining
  // their whole dirty log in the kernel until COMMIT.
  auto syncs=s.wal_syncs,bytes=s.wal_bytes_written;s.begin();Page p{};
  for(int i=0;i<1600;i++){for(int j=0;j<128;j++){auto v=random();std::memcpy(p.data()+j*4,&v,4);}s.write("large",i,p);}
  s.commit();auto written=s.wal_bytes_written-bytes;assert(written>512*1024);assert(s.wal_syncs-syncs>1);
  assert(s.wal_syncs-syncs<=written/(64*1024)+1);
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
