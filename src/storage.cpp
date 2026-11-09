#include "storage.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <unistd.h>
#include <fstream>
#include <zlib.h>

namespace mvdb {
namespace {
void require(bool yes,const char* message) { if(!yes) throw std::runtime_error(message); }
void write_all(int fd,const void* data,size_t n) {
  auto p=static_cast<const char*>(data);
  while(n) { auto done=::write(fd,p,n); if(done<0&&errno==EINTR) continue;
    require(done>0,"file write failed"); p+=done;n-=done; }
}
bool read_all(int fd,void* data,size_t n,uint64_t offset) {
  auto p=static_cast<char*>(data);
  while(n) { auto done=::pread(fd,p,n,static_cast<off_t>(offset));
    if(done<0&&errno==EINTR) continue;
    require(done>=0,"file read failed"); if(!done) return false;
    p+=done;n-=done;offset+=done; }
  return true;
}
void sync_drop(int fd) {
  require(::fdatasync(fd)==0,"fdatasync failed");
  ::posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);
}
template<class T> void append(std::string& out,T v) {out.append(reinterpret_cast<const char*>(&v),sizeof(v));}
template<class T> T take(const std::string& s,size_t& pos) {
  require(pos+sizeof(T)<=s.size(),"corrupt log record"); T v;
  std::memcpy(&v,s.data()+pos,sizeof(v));pos+=sizeof(v);return v;
}
uint32_t checksum(const std::string& s) {
  return static_cast<uint32_t>(::crc32(0,reinterpret_cast<const Bytef*>(s.data()),static_cast<uInt>(s.size())));
}
// Changed runs include short equal gaps to avoid headers for individual float bytes.
std::string delta(const std::string& file,uint64_t page,const Page& old,const Page& next) {
  require(!file.empty()&&file.size()<128&&file.find('/')==std::string::npos&&file.find("..") == std::string::npos,"unsafe page file");
  std::string out;append(out,static_cast<uint8_t>(file.size()));out+=file;append(out,page);
  size_t i=0;while(i<PAGE){while(i<PAGE&&old[i]==next[i])++i;if(i==PAGE)break;
    size_t start=i++,end=i;while(i<PAGE){if(old[i]!=next[i])end=i+1;if(i>=end+8)break;++i;}
    append(out,static_cast<uint16_t>(start));append(out,static_cast<uint16_t>(end-start));
    out.append(reinterpret_cast<const char*>(next.data()+start),end-start);i=end;
  }return out;
}
void apply_delta(const std::string& s,std::string& file,uint64_t& page,Page& bytes,bool header_only=false) {
  size_t pos=0;auto len=take<uint8_t>(s,pos);require(len>0&&len<128&&pos+len+8<=s.size(),"corrupt delta");
  file=s.substr(pos,len);pos+=len;require(file.find('/')==std::string::npos&&file.find("..") == std::string::npos,"unsafe page file");page=take<uint64_t>(s,pos);
  if(header_only)return;
  while(pos<s.size()){auto offset=take<uint16_t>(s,pos),n=take<uint16_t>(s,pos);require(n&&offset+n<=PAGE&&pos+n<=s.size(),"corrupt delta run");std::memcpy(bytes.data()+offset,s.data()+pos,n);pos+=n;}
}
void sync_directory(const std::filesystem::path& p){int fd=::open(p.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);require(fd>=0,"open directory failed");int rc=::fsync(fd);::close(fd);require(rc==0,"directory sync failed");}
std::filesystem::path wal_path(const std::filesystem::path& root,uint64_t generation){return root/(generation?"wal."+std::to_string(generation):"wal");}
int page_fd(const std::filesystem::path& path) {
  int fd=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_DIRECT,0600);
  require(fd>=0,"O_DIRECT unavailable for data file");return fd;
}
void raw_page(const std::filesystem::path& path,uint64_t id,const void* p) {
  int fd=page_fd(path);auto n=::pwrite(fd,p,PAGE,static_cast<off_t>(id*PAGE));
  int saved=errno;::close(fd);errno=saved;require(n==static_cast<ssize_t>(PAGE),"direct page write failed");
}
struct LogRecord {char type;std::string payload;uint64_t end;};
std::optional<LogRecord> read_record(int fd,uint64_t pos) {
  uint32_t n=0,h=0;
  if(!read_all(fd,&n,4,pos)) return {};
  if(n<1||n>2*PAGE+256) return {};
  std::string bytes(n,'\0');
  if(!read_all(fd,&h,4,pos+4)||!read_all(fd,bytes.data(),n,pos+8)) return {};
  require(checksum(bytes)==h,"log CRC mismatch");
  // Advise complete consumed 64KiB blocks on both verification and replay.
  constexpr uint64_t block=64*1024;
  uint64_t first=pos/block*block,last=(pos+8+n)/block*block;
  if(last>first)::posix_fadvise(fd,static_cast<off_t>(first),static_cast<off_t>(last-first),POSIX_FADV_DONTNEED);
  if(bytes[0]=='Z'){
    size_t cursor=1;auto size=take<uint32_t>(bytes,cursor);if(size>16384)return {};
    std::string expanded(size,'\0');z_stream z{};if(inflateInit2(&z,15)!=Z_OK)return {};
    z.next_in=reinterpret_cast<Bytef*>(bytes.data()+cursor);z.avail_in=bytes.size()-cursor;
    z.next_out=reinterpret_cast<Bytef*>(expanded.data());z.avail_out=size;
    auto rc=inflate(&z,Z_FINISH);bool okay=rc==Z_STREAM_END&&z.total_out==size&&z.avail_in==0;inflateEnd(&z);if(!okay)return {};
    return LogRecord{'X',std::move(expanded),pos+8+n};
  }
  return LogRecord{bytes[0],bytes.substr(1),pos+8+n};
}
}

void MemoryBudget::acquire(size_t n) {require(n<=limit-used,"memory budget exceeded");used+=n;peak=std::max(peak,used);}
BufferPool::BufferPool(std::filesystem::path path,MemoryBudget& b):directory(std::move(path)),budget(b) {
  std::filesystem::create_directories(directory);
  size_t count=std::max<size_t>(3,b.limit/(2*(PAGE+sizeof(Frame)+384)));
  require(count>=2,"buffer budget needs at least two frames");
  reservation=count*(PAGE+sizeof(Frame)+384);budget.acquire(reservation);
  require(::posix_memalign(reinterpret_cast<void**>(&arena),4096,count*PAGE)==0,"aligned allocation failed");
  frames.resize(count);lookup.reserve(count);descriptors.reserve(std::min<size_t>(16,count));
}
BufferPool::~BufferPool() {for(auto& entry:descriptors)::close(entry.second);std::free(arena);budget.release(reservation);}
int BufferPool::descriptor(const std::string& file){auto it=descriptors.find(file);if(it!=descriptors.end())return it->second;
  if(descriptors.size()>=std::min<size_t>(16,frames.size())){auto victim=descriptors.begin();::close(victim->second);descriptors.erase(victim);}
  int fd=page_fd(directory/file);descriptors.emplace(file,fd);return fd;
}
void BufferPool::flush(size_t i) {
  auto& f=frames[i];if(f.valid&&f.dirty) {
    if(f.uncommitted&&before_flush)before_flush(f.file,f.page);
    auto n=::pwrite(descriptor(f.file),arena+i*PAGE,PAGE,static_cast<off_t>(f.page*PAGE));require(n==static_cast<ssize_t>(PAGE),"direct page write failed");data_page_write_bytes+=PAGE;f.dirty=false;
  }
}
size_t BufferPool::pin(const std::string& file,uint64_t page) {
  auto key=file+":"+std::to_string(page);auto found=lookup.find(key);
  if(found!=lookup.end()){frames[found->second].age=++clock;return found->second;}
  size_t victim=frames.size();
  for(size_t i=0;i<frames.size();++i) {
    if(frames[i].pins)continue;
    if(!frames[i].valid) {victim=i;break;}
    if(victim==frames.size()||frames[i].age<frames[victim].age) victim=i;
  }
  require(victim<frames.size(),"all buffer frames pinned");
  // Prefer avoiding a steal only within a bounded LRU age window. Cold dirty
  // pages must eventually yield to a hot clean working set in tiny pools.
  if(frames[victim].valid&&frames[victim].dirty&&frames[victim].uncommitted){
    auto oldest=frames[victim].age;size_t alternative=frames.size();
    for(size_t i=0;i<frames.size();++i){auto& candidate=frames[i];
      if(candidate.pins||(candidate.dirty&&candidate.uncommitted)||candidate.age-oldest>2*frames.size())continue;
      if(alternative==frames.size()||candidate.age<frames[alternative].age)alternative=i;
    }
    if(alternative<frames.size())victim=alternative;
  }
  flush(victim);if(frames[victim].valid)lookup.erase(frames[victim].file+":"+std::to_string(frames[victim].page));auto p=arena+victim*PAGE;std::memset(p,0,PAGE);
  auto n=::pread(descriptor(file),p,PAGE,static_cast<off_t>(page*PAGE));
  require(n==0||n==static_cast<ssize_t>(PAGE),"short/corrupt data page");
  ++physical_reads;
  auto marker=file.rfind(".v");
  if(marker!=std::string::npos&&marker+2<file.size()&&file[marker+2]>='0'&&file[marker+2]<='9')++vector_reads;
  frames[victim]={file,page,++clock,true,false};lookup.emplace(std::move(key),victim);return victim;
}
Page BufferPool::get(const std::string& f,uint64_t p) {Page r;std::memcpy(r.data(),view(f,p),PAGE);return r;}
const std::byte* BufferPool::view(const std::string& f,uint64_t p) {return arena+pin(f,p)*PAGE;}
BufferPool::PageGuard BufferPool::lease(const std::string& f,uint64_t p){auto i=pin(f,p);++frames[i].pins;return {this,i,arena+i*PAGE};}
void BufferPool::put(const std::string& f,uint64_t p,const Page& bytes) {
  auto i=pin(f,p);std::memcpy(arena+i*PAGE,bytes.data(),PAGE);frames[i].dirty=true;frames[i].uncommitted=transaction_active;
}
void BufferPool::committed(){transaction_active=false;for(auto& f:frames)f.uncommitted=false;}
void BufferPool::invalidate_prefix(const std::string& prefix){
  for(auto& f:frames)if(f.valid&&(f.file==prefix||f.file.rfind(prefix+".",0)==0)){require(!f.pins,"discard pinned region");lookup.erase(f.file+":"+std::to_string(f.page));f.valid=f.dirty=false;}
  for(auto it=descriptors.begin();it!=descriptors.end();)if(it->first==prefix||it->first.rfind(prefix+".",0)==0){::close(it->second);it=descriptors.erase(it);}else ++it;
}
void BufferPool::sync_all(){
  for(auto& e:std::filesystem::directory_iterator(directory)){int fd=page_fd(e.path());try{require(::fsync(fd)==0,"data file sync failed");::posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);}catch(...){::close(fd);throw;}::close(fd);}sync_directory(directory);
}
void BufferPool::flush_all() {for(size_t i=0;i<frames.size();++i) flush(i);}

Storage::Storage(std::filesystem::path path,size_t bytes,std::function<void(size_t)> hook)
 :root(std::move(path)),budget(bytes),pool(root/"data",budget) {
  try{
  auto manifest=root/"CURRENT";
  if(std::filesystem::exists(manifest)){std::ifstream in(manifest);std::string magic;require(bool(in>>magic>>generation)&&magic=="MV3"&&generation>0,"legacy checkpoint: phase2 migration unsupported");}
  else if(!std::filesystem::exists(root/"FORMAT")&&std::filesystem::exists(root/"wal")&&std::filesystem::file_size(root/"wal"))throw std::runtime_error("legacy WAL: phase2 migration unsupported");
  if(!generation&&!std::filesystem::exists(root/"FORMAT")){
    require(std::filesystem::is_empty(root/"data"),"legacy data: phase2 migration unsupported");
    for(auto& entry:std::filesystem::directory_iterator(root))require(entry.path().filename().string().rfind("snapshot.",0)!=0,"legacy snapshot: phase2 migration unsupported");
  }
  if(!std::filesystem::exists(root/"FORMAT")){int fd=::open((root/"FORMAT").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);require(fd>=0,"create format marker failed");write_all(fd,"MV3\n",4);sync_drop(fd);::close(fd);sync_directory(root);}
  else{std::ifstream format(root/"FORMAT");std::string magic;require(bool(format>>magic)&&magic=="MV3","invalid storage format");}
  wal=::open(wal_path(root,generation).c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);require(wal>=0,"open WAL failed");
  shadow_directory=root/("shadow."+std::to_string(generation));
  shadow_index=shadow_directory/"index";
  std::filesystem::create_directories(shadow_directory);
  shadow=::open((shadow_directory/"journal").c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);require(shadow>=0,"open shadow journal failed");
  sync_directory(shadow_directory);sync_directory(root);
  restore_shadow();recover(hook);
  pool.before_flush=[this](const std::string& f,uint64_t p){protect(f,p);};
  if(generation){auto link=root/"wal.linktmp";std::filesystem::remove(link);std::filesystem::create_symlink(wal_path(root,generation).filename(),link);std::filesystem::rename(link,root/"wal");sync_directory(root);}
  undo=::open((root/"undo").c_str(),O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0600);require(undo>=0,"open undo failed");
  undo_capacity=std::min<size_t>(256*1024,(budget.limit-budget.used)/4);
  budget.acquire(undo_capacity);undo_memory.resize(undo_capacity);
  if(budget.limit-budget.used>1024*1024){redo_capacity=16384;budget.acquire(redo_capacity);redo_buffer.reserve(redo_capacity);}
  }catch(...){if(wal>=0)::close(wal);if(shadow>=0)::close(shadow);if(undo>=0)::close(undo);throw;}
}
Storage::~Storage() {
  try{close();}catch(...){/* The published epoch and its shadow/WAL remain recoverable. */}
  if(undo>=0)::close(undo);
  if(wal>=0)::close(wal);
  if(shadow>=0)::close(shadow);
  budget.release(undo_capacity+redo_capacity);
}
void Storage::close(){
  if(closed)return;
  if(active)rollback();
  checkpoint();
  closed=true;
}
void Storage::sync_wal(){sync_drop(wal);++wal_syncs;wal_dirty_bytes=0;}
void Storage::record(char type,const std::string& payload) {
  if(type=='P'&&redo_capacity){
    std::string entry;append(entry,static_cast<uint32_t>(payload.size()));entry+=payload;
    if(redo_buffer.size()+entry.size()>redo_capacity)flush_redo();
    if(entry.size()<=redo_capacity){redo_buffer+=entry;std::string bytes(1,'P');bytes+=payload;transaction_crc=::crc32(transaction_crc,reinterpret_cast<const Bytef*>(bytes.data()),bytes.size());return;}
  }
  std::string bytes(1,type);bytes+=payload;uint32_t n=static_cast<uint32_t>(bytes.size()),h=checksum(bytes);
  auto offset=::lseek(wal,0,SEEK_END);require(offset>=0,"WAL seek failed");
  try{write_all(wal,&n,4);write_all(wal,&h,4);write_all(wal,bytes.data(),bytes.size());}catch(...){closed=true;throw;}
  wal_bytes_written+=8+n;
  // Bound dirty WAL cache by bytes since the last sync, independent of the
  // absolute file offset. An ordinary <64KiB transaction gets only its COMMIT
  // sync even when it crosses a file boundary. Large transactions drain a
  // bounded window (at most 64KiB plus one record) before continuing.
  wal_dirty_bytes+=8+n;
  if(type!='C'&&wal_dirty_bytes>=64*1024)sync_wal();
  if(type=='P')transaction_crc=::crc32(transaction_crc,reinterpret_cast<const Bytef*>(bytes.data()),bytes.size());
}
void Storage::flush_redo(){
  if(redo_buffer.empty())return;
  // Small DEFLATE window and low memory level bound compressor work space.
  constexpr size_t compression_workspace=320*1024;
  budget.acquire(compression_workspace);
  z_stream z{};int rc=deflateInit2(&z,6,Z_DEFLATED,15,8,Z_DEFAULT_STRATEGY);
  if(rc!=Z_OK){budget.release(compression_workspace);throw std::runtime_error("WAL compression initialization failed");}
  std::array<char,32768> output{};z.next_in=reinterpret_cast<Bytef*>(redo_buffer.data());z.avail_in=redo_buffer.size();z.next_out=reinterpret_cast<Bytef*>(output.data());z.avail_out=output.size();
  rc=deflate(&z,Z_FINISH);auto length=z.total_out;deflateEnd(&z);budget.release(compression_workspace);require(rc==Z_STREAM_END,"WAL compression failed");
  if(length+4<redo_buffer.size()){std::string payload;append(payload,static_cast<uint32_t>(redo_buffer.size()));payload.append(output.data(),length);record('Z',payload);}
  else record('X',redo_buffer);
  redo_buffer.clear();
}
void Storage::recover(const std::function<void(size_t)>& hook) {
  auto each_delta=[](const LogRecord& entry,const auto& callback){
    if(entry.type=='P'){callback(entry.payload);return;}
    require(entry.type=='X',"invalid REDO record");size_t cursor=0;while(cursor<entry.payload.size()){auto n=take<uint32_t>(entry.payload,cursor);require(n&&cursor+n<=entry.payload.size(),"corrupt REDO batch");callback(entry.payload.substr(cursor,n));cursor+=n;}
  };
  uint64_t pos=0,start=0,valid=0;bool begun=false;size_t applied=0;uint32_t crc=0;
  void* aligned=nullptr;require(::posix_memalign(&aligned,4096,PAGE)==0,"recovery allocation failed");
  try{
    while(auto r=read_record(wal,pos)){
      if(r->type=='B'){if(begun||r->payload!="MV2")break;begun=true;start=r->end;crc=0;}
      else if((r->type=='P'||r->type=='X')&&begun){each_delta(*r,[&](const std::string& payload){std::string bytes(1,'P');bytes+=payload;crc=::crc32(crc,reinterpret_cast<const Bytef*>(bytes.data()),bytes.size());});}
      else if(r->type=='C'&&begun){size_t offset=0;if(r->payload.size()!=4||take<uint32_t>(r->payload,offset)!=crc)break;
        uint64_t replay=start;
        while(replay<pos){auto entry=read_record(wal,replay);require(entry.has_value(),"invalid committed WAL");each_delta(*entry,[&](const std::string& payload){
          if(!payload.empty()&&payload[0]=='\0'){discard_region(payload.substr(1));if(hook)hook(++applied);return;}
          std::string file;uint64_t page;Page bytes{};apply_delta(payload,file,page,bytes,true);
          int fd=page_fd(root/"data"/file);auto n=::pread(fd,aligned,PAGE,static_cast<off_t>(page*PAGE));::close(fd);require(n>=0&&n<=ssize_t(PAGE),"data recovery read failed");
          if(n)std::memcpy(bytes.data(),aligned,static_cast<size_t>(n));
          apply_delta(payload,file,page,bytes);
          std::memcpy(aligned,bytes.data(),PAGE);raw_page(root/"data"/file,page,aligned);if(hook)hook(++applied);});replay=entry->end;
        }begun=false;valid=r->end;
      }else break;
      pos=r->end;
    }
    // For SIGKILL recovery, a complete CRC-checked COMMIT is the authority.
    // A partial tail has no commit and is discarded after shadow restoration.
    require(::ftruncate(wal,static_cast<off_t>(valid))==0,"WAL tail truncation failed");sync_wal();
  }catch(...){std::free(aligned);throw;}std::free(aligned);
}
// The journal is authority; the sparse index is a disposable bounded-memory
// accelerator. A page's original on-disk image is synced before its first
// uncommitted overwrite in this epoch. Committed pages need only durable WAL:
// replaying absolute changed runs in commit order is idempotent on their images.
void Storage::protect(const std::string& file,uint64_t page){
  if(!active)return;
  require(!file.empty()&&file.size()<128&&file.find('/')==std::string::npos&&file.find("..") == std::string::npos,"unsafe shadow file");
  int index=::open((shadow_index/file).c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);require(index>=0,"open shadow index failed");
  try{
    char marked=0;auto got=::pread(index,&marked,1,static_cast<off_t>(8+page));require(got>=0,"shadow index read failed");
    if(got&&marked){::close(index);return;}
    uint64_t length=0;int fd=page_fd(root/"data"/file);
    auto size=::lseek(index,0,SEEK_END);
    if(size<8){auto end=::lseek(fd,0,SEEK_END);require(end>=0,"data size failed");length=end;require(::pwrite(index,&length,8,0)==8,"shadow index header failed");}
    else require(read_all(index,&length,8,0),"shadow index header missing");
    std::string payload;append(payload,static_cast<uint8_t>(file.size()));payload+=file;append(payload,page);append(payload,length);
    bool exists=page<length/PAGE;append(payload,static_cast<uint8_t>(exists));
    if(exists){alignas(4096) Page bytes{};bool ok=read_all(fd,bytes.data(),PAGE,page*PAGE);::close(fd);fd=-1;require(ok,"shadow original missing");payload.append(reinterpret_cast<const char*>(bytes.data()),PAGE);}else ::close(fd);
    std::string record(1,'S');record+=payload;uint32_t n=record.size(),crc=checksum(record);
    auto end=::lseek(shadow,0,SEEK_END);require(end>=0,"shadow journal seek failed");
    try{write_all(shadow,&n,4);write_all(shadow,&crc,4);write_all(shadow,record.data(),record.size());sync_drop(shadow);shadow_bytes_written+=8+record.size();}catch(...){closed=true;throw;}
    marked=1;require(::pwrite(index,&marked,1,static_cast<off_t>(8+page))==1,"shadow marker failed");
    ::posix_fadvise(index,0,0,POSIX_FADV_DONTNEED);::close(index);
  }catch(...){::close(index);throw;}
}
void Storage::restore_shadow(){
  std::filesystem::remove_all(shadow_index);std::filesystem::create_directory(shadow_index);
  uint64_t pos=0;
  while(auto entry=read_record(shadow,pos)){
    require(entry->type=='S',"invalid shadow journal");size_t cursor=0;auto n=take<uint8_t>(entry->payload,cursor);
    require(n&&n<128&&cursor+n<=entry->payload.size(),"invalid shadow filename");std::string file=entry->payload.substr(cursor,n);cursor+=n;
    require(file.find('/')==std::string::npos&&file.find("..") == std::string::npos,"unsafe shadow filename");
    auto page=take<uint64_t>(entry->payload,cursor),length=take<uint64_t>(entry->payload,cursor);auto exists=take<uint8_t>(entry->payload,cursor);
    require(length%PAGE==0&&exists<=1&&cursor+(exists?PAGE:0)==entry->payload.size(),"invalid shadow page");
    if(exists){alignas(4096) Page bytes;std::memcpy(bytes.data(),entry->payload.data()+cursor,PAGE);raw_page(root/"data"/file,page,bytes.data());}
    int fd=page_fd(root/"data"/file);require(::ftruncate(fd,static_cast<off_t>(length))==0,"restore shadow length failed");::close(fd);
    int index=::open((shadow_index/file).c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);require(index>=0,"rebuild shadow index failed");
    char marked=1;bool ok=::pwrite(index,&length,8,0)==8&&::pwrite(index,&marked,1,static_cast<off_t>(8+page))==1;::close(index);require(ok,"rebuild shadow marker failed");pos=entry->end;
  }
  require(::ftruncate(shadow,static_cast<off_t>(pos))==0,"truncate torn shadow failed");sync_drop(shadow);
}
bool Storage::fresh_region(const std::string& file)const{
  for(auto& r:regions)if(file==r||file.rfind(r+".",0)==0)return true;
  return false;
}
std::string Storage::begin_region(const std::string& prefix){
  require(active,"region outside transaction");require(!prefix.empty()&&prefix.size()<90&&prefix.find('/')==std::string::npos&&prefix.find("..") == std::string::npos,"unsafe region prefix");
  std::filesystem::create_directories(root/"regions");
  uint64_t id=0;std::string name;
  for(;;){name=prefix+".r"+std::to_string(++id);int fd=::open((root/"regions"/name).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd>=0){sync_drop(fd);::close(fd);sync_directory(root/"regions");sync_directory(root);break;}require(errno==EEXIST,"allocate region failed");}
  std::string data(1,'\0');data+=name;uint32_t n=data.size();undo_write(data.data(),n,undo_top);undo_write(reinterpret_cast<const char*>(&n),4,undo_top+n);undo_top+=n+4;regions.push_back(name);return name;
}
void Storage::seal_region(const std::string& prefix){require(active,"seal outside transaction");regions.erase(std::remove(regions.begin(),regions.end(),prefix),regions.end());}
void Storage::write_new(const std::string& file,uint64_t page,const Page& bytes){require(!closed&&active&&fresh_region(file),"write_new outside fresh region");write_redo(file,page,bytes);}
void Storage::discard_region(const std::string& prefix){
  pool.invalidate_prefix(prefix);
  for(auto& e:std::filesystem::directory_iterator(root/"data")){auto name=e.path().filename().string();if(name==prefix||name.rfind(prefix+".",0)==0)std::filesystem::remove(e.path());}
}
void Storage::undo_write(const char* p,size_t n,uint64_t offset){
  if(offset<undo_capacity){size_t m=std::min<uint64_t>(n,undo_capacity-offset);std::memcpy(undo_memory.data()+offset,p,m);offset+=m;p+=m;n-=m;}
  if(n){require(::lseek(undo,static_cast<off_t>(offset-undo_capacity),SEEK_SET)>=0,"undo seek failed");write_all(undo,p,n);undo_spill_bytes+=n;
    undo_dirty_bytes+=n;
    if(undo_dirty_bytes>=64*1024){sync_drop(undo);undo_dirty_bytes=0;}}
}
void Storage::undo_read(char* p,size_t n,uint64_t offset){
  if(offset<undo_capacity){size_t m=std::min<uint64_t>(n,undo_capacity-offset);std::memcpy(p,undo_memory.data()+offset,m);offset+=m;p+=m;n-=m;}
  if(n){auto disk_offset=offset-undo_capacity;require(read_all(undo,p,n,disk_offset),"undo record missing");
    constexpr uint64_t alignment=4096;
    auto first=disk_offset/alignment*alignment,last=(disk_offset+n+alignment-1)/alignment*alignment;
    ::posix_fadvise(undo,static_cast<off_t>(first),static_cast<off_t>(last-first),POSIX_FADV_DONTNEED);
  }
}
void Storage::begin(){require(!closed,"storage is closed");require(!active,"transaction already active");require(::ftruncate(undo,0)==0,"undo reset failed");undo_top=0;undo_dirty_bytes=0;wal_dirty_bytes=0;redo_buffer.clear();transaction_crc=0;transaction_start=::lseek(wal,0,SEEK_END);record('B',"MV2");active=true;pool.transaction_active=true;}
void Storage::commit(){require(!closed&&active,"no active transaction");try{flush_redo();std::string payload;append(payload,transaction_crc);record('C',payload);sync_wal();
  active=false;pool.committed();regions.clear();undo_top=0;
  if(::lseek(wal,0,SEEK_END)>=64*1024*1024)checkpoint();
  }catch(...){closed=true;throw;}
}
void Storage::write_redo(const std::string& f,uint64_t p,const Page& bytes){auto old=pool.get(f,p);if(old==bytes)return;record('P',delta(f,p,old,bytes));pool.put(f,p,bytes);}
void Storage::write(const std::string& f,uint64_t p,const Page& bytes){require(!closed&&active,"write outside transaction");auto before=pool.get(f,p);if(before==bytes)return;
  if(fresh_region(f)){write_new(f,p,bytes);return;}
  auto data=delta(f,p,bytes,before);uint32_t n=data.size();undo_write(data.data(),n,undo_top);undo_write(reinterpret_cast<const char*>(&n),4,undo_top+n);undo_top+=n+4;
  record('P',delta(f,p,before,bytes));pool.put(f,p,bytes);
}
void Storage::rollback_to(uint64_t target){require(active&&target<=undo_top,"invalid savepoint");while(undo_top>target){uint32_t n=0;undo_read(reinterpret_cast<char*>(&n),4,undo_top-4);require(n<=2*PAGE+256&&n+4<=undo_top-target,"invalid undo length");std::string data(n,'\0');undo_read(data.data(),n,undo_top-n-4);
  if(!data.empty()&&data[0]=='\0'){auto prefix=data.substr(1);record('P',data);discard_region(prefix);regions.erase(std::remove(regions.begin(),regions.end(),prefix),regions.end());undo_top-=n+4;continue;}
  std::string file;uint64_t page;Page bytes{};apply_delta(data,file,page,bytes,true);bytes=pool.get(file,page);apply_delta(data,file,page,bytes);write_redo(file,page,bytes);undo_top-=n+4;
}}
void Storage::rollback(){rollback_to(0);redo_buffer.clear();require(::ftruncate(wal,static_cast<off_t>(transaction_start))==0,"rollback WAL truncation failed");active=false;pool.committed();regions.clear();}
void Storage::checkpoint(const std::function<void(size_t)>& stage){
  if(closed)return;
  require(!active,"checkpoint inside transaction");
  int fresh=-1,nextfd=-1;
  try{pool.flush_all();pool.sync_all();if(stage)stage(1);
  uint64_t next=generation+1;auto nextwal=wal_path(root,next);
  fresh=::open(nextwal.c_str(),O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0600);require(fresh>=0,"create checkpoint WAL failed");sync_drop(fresh);++wal_syncs;
  auto nextshadow=root/("shadow."+std::to_string(next));std::filesystem::remove_all(nextshadow);std::filesystem::create_directories(nextshadow);
  nextfd=::open((nextshadow/"journal").c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);require(nextfd>=0,"create shadow journal failed");sync_drop(nextfd);sync_directory(nextshadow);
  auto pending=root/"CURRENT.tmp";int fd=::open(pending.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);require(fd>=0,"create checkpoint manifest failed");auto value="MV3 "+std::to_string(next)+"\n";write_all(fd,value.data(),value.size());sync_drop(fd);::close(fd);sync_directory(root);if(stage)stage(2);
  std::filesystem::rename(pending,root/"CURRENT");sync_directory(root);
  auto oldwal=wal_path(root,generation),oldshadow=shadow_directory;
  ::close(wal);::close(shadow);wal=fresh;shadow=nextfd;fresh=nextfd=-1;wal_dirty_bytes=0;generation=next;shadow_directory=nextshadow;shadow_index=nextshadow/"index";std::filesystem::create_directory(shadow_index);
  if(stage)stage(3);
  auto link=root/"wal.linktmp";std::filesystem::remove(link);std::filesystem::create_symlink(nextwal.filename(),link);std::filesystem::rename(link,root/"wal");sync_directory(root);if(stage)stage(4);
  if(oldwal!=root/"wal")std::filesystem::remove(oldwal);
  std::filesystem::remove_all(oldshadow);
  }catch(...){if(fresh>=0)::close(fresh);if(nextfd>=0)::close(nextfd);closed=true;throw;}
}

std::optional<SlotId> PrimaryIndex::find(int64_t key) {
  auto h=storage.load<Header>(file,0);uint32_t p=h.root;
  while(p) {auto n=storage.load<Node>(file,p);
    if(n.leaf) {auto it=std::lower_bound(n.keys,n.keys+n.n,key);auto i=it-n.keys;
      if(i<n.n&&*it==key)return n.slots[i];
      return {};}
    auto i=std::upper_bound(n.keys,n.keys+n.n,key)-n.keys;p=n.children[i];
  }return {};
}
uint32_t PrimaryIndex::allocate(const Node& n) {
  auto h=storage.load<Header>(file,0);if(h.next==0)h.next=1;
  uint32_t p=h.next++;storage.store(file,0,h);storage.store(file,p,n);return p;
}
std::optional<PrimaryIndex::Split> PrimaryIndex::insert_node(uint32_t page,int64_t key,SlotId slot) {
  auto n=storage.load<Node>(file,page);
  if(n.leaf) {
    auto i=static_cast<size_t>(std::lower_bound(n.keys,n.keys+n.n,key)-n.keys);
    require(i==n.n||n.keys[i]!=key,"duplicate primary key");
    for(size_t j=n.n;j>i;--j){n.keys[j]=n.keys[j-1];n.slots[j]=n.slots[j-1];}
    n.keys[i]=key;n.slots[i]=slot;++n.n;
  } else {
    auto i=static_cast<size_t>(std::upper_bound(n.keys,n.keys+n.n,key)-n.keys);
    auto split=insert_node(n.children[i],key,slot);if(!split)return {};
    for(size_t j=n.n;j>i;--j){n.keys[j]=n.keys[j-1];n.children[j+1]=n.children[j];}
    n.keys[i]=split->key;n.children[i+1]=split->right;++n.n;
  }
  if(n.n<=ORDER) {storage.store(file,page,n);return {};}
  Node right{};right.leaf=n.leaf;uint32_t mid=n.n/2;int64_t separator=n.keys[mid];
  uint32_t first=n.leaf?mid:mid+1;right.n=n.n-first;
  for(uint32_t j=0;j<right.n;++j){right.keys[j]=n.keys[first+j];right.slots[j]=n.slots[first+j];}
  if(!n.leaf)for(uint32_t j=0;j<=right.n;++j)right.children[j]=n.children[first+j];
  n.n=mid;auto r=allocate(right);storage.store(file,page,n);return Split{separator,r};
}
void PrimaryIndex::insert(int64_t key,SlotId slot) {
  auto h=storage.load<Header>(file,0);
  if(!h.root) {Node n{};n.n=1;n.keys[0]=key;n.slots[0]=slot;auto root=allocate(n);
    h=storage.load<Header>(file,0);h.root=root;storage.store(file,0,h);return;}
  auto split=insert_node(h.root,key,slot);
  if(split) {Node n{};n.leaf=0;n.n=1;n.keys[0]=split->key;n.children[0]=h.root;n.children[1]=split->right;
    auto root=allocate(n);h=storage.load<Header>(file,0);h.root=root;storage.store(file,0,h);}
}
void PrimaryIndex::erase(int64_t key) {
  auto p=storage.load<Header>(file,0).root;
  while(p) {auto n=storage.load<Node>(file,p);
    if(n.leaf) {auto i=static_cast<uint32_t>(std::lower_bound(n.keys,n.keys+n.n,key)-n.keys);
      if(i<n.n&&n.keys[i]==key) {for(auto j=i+1;j<n.n;++j){n.keys[j-1]=n.keys[j];n.slots[j-1]=n.slots[j];}
        --n.n;storage.store(file,p,n);}return;}
    p=n.children[std::upper_bound(n.keys,n.keys+n.n,key)-n.keys];
  }
}
}
