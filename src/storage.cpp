#include "storage.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <unistd.h>
#include <fstream>
#include <zlib.h>

namespace mvdb {
size_t detail::bitmap_next(const std::byte* bytes,size_t begin,size_t end,bool want_set){
  if(begin>=end)return end;
  size_t base=begin-begin%64;
  for(;;){
    const size_t valid=std::min<size_t>(64,end-base);
    const size_t count=(valid+7)/8;
    uint64_t word=0;
    if constexpr(std::endian::native==std::endian::little){
      if(count==sizeof(word))std::memcpy(&word,bytes+base/8,sizeof(word));
      else std::memcpy(&word,bytes+base/8,count);
    }
    else
      for(size_t i=0;i<count;++i)word|=uint64_t(std::to_integer<unsigned>(bytes[base/8+i]))<<(8*i);
    if(!want_set)word=~word;
    word&=~uint64_t(0)<<(begin-base);
    if(valid<64)word&=(uint64_t(1)<<valid)-1;
    // countr_zero is reached only for a nonzero word on every platform.
    if(word)return base+std::countr_zero(word);
    if(end-base<=64)return end;
    base+=64;begin=base;
  }
}
namespace {
void require(bool yes,const char* message) { if(!yes) throw std::runtime_error(message); }
template<class F> auto retry_eintr(F operation){
  decltype(operation()) result;do{result=operation();}while(result<0&&errno==EINTR);return result;
}
bool disable_direct(int fd){
  auto flags=::fcntl(fd,F_GETFL);
  if(flags<0||!(flags&O_DIRECT))return false;
  require(::fcntl(fd,F_SETFL,flags&~O_DIRECT)==0,"buffered I/O fallback failed");
  ::posix_fadvise(fd,0,0,POSIX_FADV_RANDOM);return true;
}
ssize_t page_read(int fd,void* data,size_t n,off_t offset){
  ssize_t got;
  for(;;){got=::pread(fd,data,n,offset);if(got<0&&errno==EINTR)continue;
    if(got<0&&errno==EINVAL&&disable_direct(fd))continue;
    break;}
  return got;
}
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
void sync_file(int fd) {require(retry_eintr([&]{return ::fdatasync(fd);})==0,"fdatasync failed");}
void page_write(int fd,const void* data,off_t offset){
  ssize_t n;
  for(;;){n=::pwrite(fd,data,PAGE,offset);if(n<0&&errno==EINTR)continue;
    if(n<0&&errno==EINVAL&&disable_direct(fd))continue;
    break;}
  require(n==static_cast<ssize_t>(PAGE),"page write failed");
}
template<class T> void append_value(std::string& out,T v) {out.append(reinterpret_cast<const char*>(&v),sizeof(v));}
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
  std::string out;append_value(out,static_cast<uint8_t>(file.size()));out+=file;append_value(out,page);
  size_t i=0;while(i<PAGE){
    // Skip equal words without alignment/aliasing assumptions. The byte scan
    // still finds the identical first difference and preserves every WAL run.
    while(i+sizeof(uint64_t)<=PAGE){uint64_t a,b;std::memcpy(&a,old.data()+i,sizeof(a));std::memcpy(&b,next.data()+i,sizeof(b));if(a!=b)break;i+=sizeof(a);}
    while(i<PAGE&&old[i]==next[i])++i;
    if(i==PAGE)break;
    size_t start=i++,end=i;while(i<PAGE){if(old[i]!=next[i])end=i+1;if(i>=end+8)break;++i;}
    append_value(out,static_cast<uint16_t>(start));append_value(out,static_cast<uint16_t>(end-start));
    out.append(reinterpret_cast<const char*>(next.data()+start),end-start);i=end;
  }return out;
}
void apply_delta(const std::string& s,std::string& file,uint64_t& page,Page& bytes,bool header_only=false) {
  size_t pos=0;auto len=take<uint8_t>(s,pos);require(len>0&&len<128&&pos+len+8<=s.size(),"corrupt delta");
  file=s.substr(pos,len);pos+=len;require(file.find('/')==std::string::npos&&file.find("..") == std::string::npos,"unsafe page file");page=take<uint64_t>(s,pos);
  if(header_only)return;
  while(pos<s.size()){auto offset=take<uint16_t>(s,pos),n=take<uint16_t>(s,pos);require(n&&offset+n<=PAGE&&pos+n<=s.size(),"corrupt delta run");std::memcpy(bytes.data()+offset,s.data()+pos,n);pos+=n;}
}
void sync_directory(const std::filesystem::path& p){int fd=::open(p.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);require(fd>=0,"open directory failed");int rc=retry_eintr([&]{return ::fsync(fd);});::close(fd);require(rc==0,"directory sync failed");}
std::filesystem::path wal_path(const std::filesystem::path& root,uint64_t generation){return root/(generation?"wal."+std::to_string(generation):"wal");}
int page_fd(const std::filesystem::path& path) {
  const char* option=std::getenv("MVDB_USE_ODIRECT");
  bool direct=option&&std::strcmp(option,"1")==0;
  int fd=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|(direct?O_DIRECT:0),0600);
  if(fd<0&&direct&&(errno==EINVAL||errno==EOPNOTSUPP)){
    fd=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
    if(fd>=0)::posix_fadvise(fd,0,0,POSIX_FADV_RANDOM);
  }
  require(fd>=0,"open data file failed");return fd;
}
void raw_page(const std::filesystem::path& path,uint64_t id,const void* p) {
  int fd=page_fd(path);try{page_write(fd,p,static_cast<off_t>(id*PAGE));}catch(...){::close(fd);throw;}::close(fd);
}
struct LogRecord {char type;std::string payload;uint64_t end;};
std::optional<LogRecord> read_record(int fd,uint64_t pos) {
  uint32_t n=0,h=0;
  if(!read_all(fd,&n,4,pos)) return {};
  if(n<1||n>2*PAGE+256) return {};
  std::string bytes(n,'\0');
  if(!read_all(fd,&h,4,pos+4)||!read_all(fd,bytes.data(),n,pos+8)) return {};
  require(checksum(bytes)==h,"log CRC mismatch");
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

std::string detail::page_delta(const std::string& file,uint64_t page,const Page& old,const Page& next){return delta(file,page,old,next);}

void MemoryBudget::acquire(size_t n) {require(n<=limit-used,"memory budget exceeded");used+=n;peak=std::max(peak,used);}
BufferPool::BufferPool(std::filesystem::path path,MemoryBudget& b):directory(std::move(path)),budget(b) {
  std::filesystem::create_directories(directory);
  size_t allocation=b.limit>8*1024*1024 ? b.limit-b.limit/4 : b.limit/2;
  size_t count=std::max<size_t>(3,allocation/(PAGE+sizeof(Frame)+384));
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
    page_write(descriptor(f.file),arena+i*PAGE,static_cast<off_t>(f.page*PAGE));data_page_write_bytes+=PAGE;f.dirty=false;
  }
}
size_t BufferPool::pin(const std::string& file,uint64_t page) {
  ++page_accesses;
  auto key=file+":"+std::to_string(page);auto found=lookup.find(key);
  if(found!=lookup.end()){frames[found->second].age=++clock;return found->second;}
  // Give frequently reused primary/header pages a bounded protected share.
  // A streaming vector scan can reuse the remaining frames without flushing
  // those pages from the cache; even a three-frame pool keeps a payload slot.
  bool metadata=file.ends_with(".pk")||file.ends_with(".header");
  size_t protected_count=0,oldest_metadata=frames.size(),oldest_payload=frames.size();
  for(size_t i=0;i<frames.size();++i){auto& f=frames[i];
    if(f.valid&&f.metadata)++protected_count;
    if(f.pins)continue;
    auto& oldest=f.valid&&f.metadata?oldest_metadata:oldest_payload;
    if(oldest==frames.size()||(!f.valid&&frames[oldest].valid)||
       (f.valid==frames[oldest].valid&&f.age<frames[oldest].age))oldest=i;
  }
  size_t quota=std::min(frames.size()-1,std::max<size_t>(2,frames.size()/2));
  bool replace_metadata=metadata&&protected_count>=quota&&oldest_metadata<frames.size();
  size_t victim=replace_metadata?oldest_metadata:oldest_payload;
  if(victim==frames.size())victim=oldest_metadata;
  require(victim<frames.size(),"all buffer frames pinned");
  // No-Steal remains only a bounded age preference within the chosen class.
  // Metadata protection cannot make all available payload frames unevictable.
  if(frames[victim].valid&&frames[victim].dirty&&frames[victim].uncommitted){
    auto oldest=frames[victim].age;size_t alternative=frames.size();
    for(size_t i=0;i<frames.size();++i){auto& candidate=frames[i];
      if(candidate.pins||candidate.metadata!=frames[victim].metadata||
         (candidate.dirty&&candidate.uncommitted)||candidate.age-oldest>2*frames.size())continue;
      if(alternative==frames.size()||candidate.age<frames[alternative].age)alternative=i;
    }
    if(alternative<frames.size())victim=alternative;
  }
  flush(victim);if(frames[victim].valid)lookup.erase(frames[victim].file+":"+std::to_string(frames[victim].page));auto p=arena+victim*PAGE;std::memset(p,0,PAGE);
  auto n=page_read(descriptor(file),p,PAGE,static_cast<off_t>(page*PAGE));
  require(n==0||n==static_cast<ssize_t>(PAGE),"short/corrupt data page");
  ++physical_reads;
  auto marker=file.rfind(".v");
  if(marker!=std::string::npos&&marker+2<file.size()&&file[marker+2]>='0'&&file[marker+2]<='9')++vector_reads;
  auto version=frames[victim].version+1;
  frames[victim]={file,page,++clock,version,true,false,false,metadata};lookup.emplace(std::move(key),victim);return victim;
}
Page BufferPool::get(const std::string& f,uint64_t p) {Page r;std::memcpy(r.data(),view(f,p),PAGE);return r;}
const std::byte* BufferPool::view(const std::string& f,uint64_t p) {return arena+pin(f,p)*PAGE;}
BufferPool::PageGuard BufferPool::lease(const std::string& f,uint64_t p){auto i=pin(f,p);++frames[i].pins;return {this,i,arena+i*PAGE};}
void BufferPool::put(const std::string& f,uint64_t p,const Page& bytes) {
  auto i=pin(f,p);std::memcpy(arena+i*PAGE,bytes.data(),PAGE);frames[i].dirty=true;frames[i].uncommitted=transaction_active;
}
BufferPool::ResidentHandle BufferPool::handle(const std::string& f,uint64_t p){auto i=pin(f,p);return {i,frames[i].version};}
const std::byte* BufferPool::resident(ResidentHandle h) const {
  if(h.frame>=frames.size()||!frames[h.frame].valid||frames[h.frame].version!=h.version)return nullptr;
  return arena+h.frame*PAGE;
}
void BufferPool::flush_page(const std::string& f,uint64_t p){flush(pin(f,p));}
void BufferPool::append_extent(const std::string& f,uint64_t p,const Page& bytes,size_t offset,size_t length){
  append_extent_bytes(f,p,offset,bytes.data()+offset,length);
}
void BufferPool::append_extent_bytes(const std::string& f,uint64_t p,size_t offset,const void* bytes,size_t length){
  auto i=pin(f,p);require(!frames[i].dirty,"append requires flushed page");
  int fd=descriptor(f);
  // An extent can be unaligned; explicit O_DIRECT remains supported by switching
  // this descriptor to buffered writes before publishing the appended payload.
  disable_direct(fd);
  // Establish a complete zero-filled page first. SIGKILL between extent write
  // and a later extension would otherwise leave an unreadable short page.
  auto size=::lseek(fd,0,SEEK_END);require(size>=0,"append size failed");
  if(static_cast<uint64_t>(size)<(p+1)*PAGE)require(::ftruncate(fd,static_cast<off_t>((p+1)*PAGE))==0,"append page extension failed");
  size_t remaining=length;const std::byte* source=static_cast<const std::byte*>(bytes);uint64_t position=p*PAGE+offset;
  while(remaining){auto done=retry_eintr([&]{return ::pwrite(fd,source,remaining,static_cast<off_t>(position));});require(done>0,"append write failed");source+=done;remaining-=done;position+=done;}
  data_page_write_bytes+=length;std::memcpy(arena+i*PAGE+offset,bytes,length);
  frames[i].dirty=frames[i].uncommitted=false;
}
void BufferPool::committed(){transaction_active=false;for(auto& f:frames)f.uncommitted=false;}
void BufferPool::invalidate_prefix(const std::string& prefix){
  for(auto& f:frames)if(f.valid&&(f.file==prefix||f.file.rfind(prefix+".",0)==0)){require(!f.pins,"discard pinned region");lookup.erase(f.file+":"+std::to_string(f.page));f.valid=f.dirty=false;++f.version;}
  for(auto it=descriptors.begin();it!=descriptors.end();)if(it->first==prefix||it->first.rfind(prefix+".",0)==0){::close(it->second);it=descriptors.erase(it);}else ++it;
}
void BufferPool::sync_all(){
  for(auto& e:std::filesystem::directory_iterator(directory)){int fd=page_fd(e.path());try{require(retry_eintr([&]{return ::fsync(fd);})==0,"data file sync failed");::posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);}catch(...){::close(fd);throw;}::close(fd);}sync_directory(directory);
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
  if(!std::filesystem::exists(root/"FORMAT")){int fd=::open((root/"FORMAT").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);require(fd>=0,"create format marker failed");write_all(fd,"MV3\n",4);sync_file(fd);::close(fd);sync_directory(root);}
  else{std::ifstream format(root/"FORMAT");std::string magic;require(bool(format>>magic)&&magic=="MV3","invalid storage format");}
  wal=::open(wal_path(root,generation).c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);require(wal>=0,"open WAL failed");
  shadow_directory=root/("shadow."+std::to_string(generation));
  shadow_index=shadow_directory/"index";written_directory=shadow_directory/"written";
  std::filesystem::create_directories(written_directory);
  written_count=budget.limit<=8*1024*1024?4:16;
  written_reservation=written_count*sizeof(WrittenEntry);budget.acquire(written_reservation);
  written_cache=std::make_unique<WrittenEntry[]>(written_count);
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
  budget.release(undo_capacity+redo_capacity+written_reservation);
}
void Storage::close(){
  if(closed)return;
  if(active)rollback();
  checkpoint();
  closed=true;
}
std::shared_ptr<FILE> Storage::temporary(){
  require(!closed,"storage is closed");
  auto name=(root/"spill.XXXXXX").string();std::vector<char> path(name.begin(),name.end());path.push_back('\0');
  int fd=::mkostemp(path.data(),O_CLOEXEC);require(fd>=0,"create spill file failed");
  if(::unlink(path.data())!=0){::close(fd);throw std::runtime_error("unlink spill file failed");}
  FILE* file=::fdopen(fd,"w+b");if(!file){::close(fd);throw std::runtime_error("open spill stream failed");}
  ::setvbuf(file,nullptr,_IONBF,0);
  return {file,[](FILE* p){::posix_fadvise(::fileno(p),0,0,POSIX_FADV_DONTNEED);std::fclose(p);}};
}
void Storage::sync_wal(){sync_file(wal);++wal_syncs;::posix_fadvise(wal,0,0,POSIX_FADV_DONTNEED);}
void Storage::record(char type,const std::string& payload) {
  if(type=='P'&&redo_capacity){
    std::string entry;append_value(entry,static_cast<uint32_t>(payload.size()));entry+=payload;
    if(redo_buffer.size()+entry.size()>redo_capacity)flush_redo();
    if(entry.size()<=redo_capacity){redo_buffer+=entry;std::string bytes(1,'P');bytes+=payload;transaction_crc=::crc32(transaction_crc,reinterpret_cast<const Bytef*>(bytes.data()),bytes.size());return;}
  }
  std::string bytes(1,type);bytes+=payload;uint32_t n=static_cast<uint32_t>(bytes.size()),h=checksum(bytes);
  auto offset=::lseek(wal,0,SEEK_END);require(offset>=0,"WAL seek failed");
  try{write_all(wal,&n,4);write_all(wal,&h,4);write_all(wal,bytes.data(),bytes.size());}catch(...){closed=true;throw;}
  wal_bytes_written+=8+n;
  // Only COMMIT/checkpoint need a sync for the SIGKILL contract. Dirty
  // kernel pages are reclaimable and charged by the external memory controller.
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
  if(length+4<redo_buffer.size()){std::string payload;append_value(payload,static_cast<uint32_t>(redo_buffer.size()));payload.append(output.data(),length);record('Z',payload);}
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
          mark_written(file);
          int fd=page_fd(root/"data"/file);auto n=page_read(fd,aligned,PAGE,static_cast<off_t>(page*PAGE));::close(fd);require(n>=0&&n<=ssize_t(PAGE),"data recovery read failed");
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
// accelerator. Complete the journal write before its first uncommitted data
// overwrite: SIGKILL preserves these kernel writes without a per-page sync.
// Checkpoint still syncs data before publishing a new epoch. Committed pages
// need only durable WAL:
// replaying absolute changed runs in commit order is idempotent on their images.
void Storage::protect(const std::string& file,uint64_t page){
  if(!active)return;
  mark_written(file);
  require(!file.empty()&&file.size()<128&&file.find('/')==std::string::npos&&file.find("..") == std::string::npos,"unsafe shadow file");
  int index=::open((shadow_index/file).c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);require(index>=0,"open shadow index failed");
  try{
    char marked=0;auto got=retry_eintr([&]{return ::pread(index,&marked,1,static_cast<off_t>(8+page));});require(got>=0,"shadow index read failed");
    if(got&&marked){::close(index);return;}
    uint64_t length=0;int fd=page_fd(root/"data"/file);
    auto size=::lseek(index,0,SEEK_END);
    if(size<8){auto end=::lseek(fd,0,SEEK_END);require(end>=0,"data size failed");length=end;require(retry_eintr([&]{return ::pwrite(index,&length,8,0);})==8,"shadow index header failed");}
    else require(read_all(index,&length,8,0),"shadow index header missing");
    std::string payload;append_value(payload,static_cast<uint8_t>(file.size()));payload+=file;append_value(payload,page);append_value(payload,length);
    bool exists=page<length/PAGE;append_value(payload,static_cast<uint8_t>(exists));
    if(exists){alignas(4096) Page bytes{};auto got=page_read(fd,bytes.data(),PAGE,static_cast<off_t>(page*PAGE));::close(fd);fd=-1;require(got==static_cast<ssize_t>(PAGE),"shadow original missing");payload.append(reinterpret_cast<const char*>(bytes.data()),PAGE);}else ::close(fd);
    std::string record(1,'S');record+=payload;uint32_t n=record.size(),crc=checksum(record);
    auto end=::lseek(shadow,0,SEEK_END);require(end>=0,"shadow journal seek failed");
    try{write_all(shadow,&n,4);write_all(shadow,&crc,4);write_all(shadow,record.data(),record.size());shadow_bytes_written+=8+record.size();}catch(...){closed=true;throw;}
    marked=1;require(retry_eintr([&]{return ::pwrite(index,&marked,1,static_cast<off_t>(8+page));})==1,"shadow marker failed");
    ::close(index);
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
    char marked=1;bool ok=retry_eintr([&]{return ::pwrite(index,&length,8,0);})==8&&retry_eintr([&]{return ::pwrite(index,&marked,1,static_cast<off_t>(8+page));})==1;::close(index);require(ok,"rebuild shadow marker failed");pos=entry->end;
  }
  require(::ftruncate(shadow,static_cast<off_t>(pos))==0,"truncate torn shadow failed");sync_file(shadow);
}
bool Storage::fresh_region(const std::string& file)const{
  for(auto& r:regions)if(file==r||file.rfind(r+".",0)==0)return true;
  return false;
}
std::string Storage::begin_region(const std::string& prefix){
  require(active,"region outside transaction");require(!prefix.empty()&&prefix.size()<90&&prefix.find('/')==std::string::npos&&prefix.find("..") == std::string::npos,"unsafe region prefix");
  std::filesystem::create_directories(root/"regions");
  uint64_t id=0;std::string name;
  for(;;){name=prefix+".r"+std::to_string(++id);int fd=::open((root/"regions"/name).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd>=0){sync_file(fd);::close(fd);sync_directory(root/"regions");sync_directory(root);break;}require(errno==EEXIST,"allocate region failed");}
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
  if(n){require(::lseek(undo,static_cast<off_t>(offset-undo_capacity),SEEK_SET)>=0,"undo seek failed");write_all(undo,p,n);undo_spill_bytes+=n;}
}
void Storage::undo_read(char* p,size_t n,uint64_t offset){
  if(offset<undo_capacity){size_t m=std::min<uint64_t>(n,undo_capacity-offset);std::memcpy(p,undo_memory.data()+offset,m);offset+=m;p+=m;n-=m;}
  if(n)require(read_all(undo,p,n,offset-undo_capacity),"undo record missing");
}
void Storage::begin(){require(!closed,"storage is closed");require(!active,"transaction already active");require(::ftruncate(undo,0)==0,"undo reset failed");undo_top=0;redo_buffer.clear();transaction_crc=0;transaction_start=::lseek(wal,0,SEEK_END);record('B',"MV2");active=true;pool.transaction_active=true;}
void Storage::commit(){require(!closed&&active,"no active transaction");try{flush_redo();std::string payload;append_value(payload,transaction_crc);record('C',payload);sync_wal();
  active=false;pool.committed();regions.clear();undo_top=0;
  if(::lseek(wal,0,SEEK_END)>=64*1024*1024)checkpoint();
  }catch(...){closed=true;throw;}
}
void Storage::cache_written(const std::string& f,bool status){
  for(size_t i=0;i<written_count;++i)if(f==written_cache[i].file.data()){written_cache[i].status=status;return;}
  auto i=written_cursor++%written_count;std::memcpy(written_cache[i].file.data(),f.c_str(),f.size()+1);written_cache[i].status=status;
}
bool Storage::normal_written(const std::string& f){
  for(size_t i=0;i<written_count;++i)if(f==written_cache[i].file.data())return written_cache[i].status;
  // Both positive and negative results are safe until the next epoch. Every
  // normal redo/shadow path changes this exact cache entry before writing.
  bool found=std::filesystem::exists(written_directory/f)||std::filesystem::exists(shadow_index/f);
  cache_written(f,found);return found;
}
void Storage::mark_written(const std::string& f){
  require(!f.empty()&&f.size()<128&&f.find('/')==std::string::npos&&f.find("..") == std::string::npos,"unsafe written file");
  if(normal_written(f))return;
  // A bounded exact cache accelerates a persistent epoch guard. Retain the
  // marker across recovery: older committed redo may overwrite a reused tail.
  int fd=::open((written_directory/f).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
  if(fd>=0)::close(fd);else require(errno==EEXIST,"create written marker failed");
  cache_written(f,true);
}
void Storage::write_redo(const std::string& f,uint64_t p,const Page& bytes){auto old=pool.get(f,p);if(old==bytes)return;mark_written(f);record('P',delta(f,p,old,bytes));pool.put(f,p,bytes);}
void Storage::write(const std::string& f,uint64_t p,const Page& bytes){require(!closed&&active,"write outside transaction");auto before=pool.get(f,p);if(before==bytes)return;
  if(fresh_region(f)){write_new(f,p,bytes);return;}
  auto data=delta(f,p,bytes,before);uint32_t n=data.size();undo_write(data.data(),n,undo_top);undo_write(reinterpret_cast<const char*>(&n),4,undo_top+n);undo_top+=n+4;
  mark_written(f);record('P',delta(f,p,before,bytes));pool.put(f,p,bytes);
}
void Storage::append(const std::string& f,uint64_t p,const Page& bytes,size_t offset,size_t length){
  require(!closed&&active,"append outside transaction");
  require(length&&offset<=PAGE&&length<=PAGE-offset,"invalid append extent");
  require(!f.empty()&&f.size()<128&&f.find('/')==std::string::npos&&f.find("..") == std::string::npos,"unsafe append file");
  auto before=pool.get(f,p);
  require(std::memcmp(before.data(),bytes.data(),offset)==0&&std::memcmp(before.data()+offset+length,bytes.data()+offset+length,PAGE-offset-length)==0,"append modified visible prefix");
  if(before==bytes)return;
  // A known guarded file keeps ordinary buffered WAL writes; no forced flush
  // is needed because this path will never omit redo.
  if(normal_written(f)){write(f,p,bytes);return;}
  // Flush first: an uncommitted dirty page must acquire shadow protection, and
  // a committed dirty prefix must reach disk before the direct extent write.
  pool.flush_page(f,p);
  // Every shadow record carries a FILE length and restore truncates to it.
  // A page-only test would lose WAL-free appends beyond another page's length.
  if(normal_written(f)){write(f,p,bytes);return;}
  pool.append_extent(f,p,bytes,offset,length);
}
void Storage::append_bytes(const std::string& f,uint64_t p,size_t offset,const void* data,size_t length){
  require(!closed&&active,"append outside transaction");
  require(data&&length&&offset<=PAGE&&length<=PAGE-offset,"invalid append extent");
  require(!f.empty()&&f.size()<128&&f.find('/')==std::string::npos&&f.find("..") == std::string::npos,"unsafe append file");
  auto logged=[&]{auto bytes=pool.get(f,p);std::memcpy(bytes.data()+offset,data,length);write(f,p,bytes);};
  if(normal_written(f)){logged();return;}
  // A dirty prefix can acquire a file-wide shadow guard when flushed. Check
  // again after flush, exactly as the full-page append interface does.
  pool.flush_page(f,p);
  if(normal_written(f)){logged();return;}
  pool.append_extent_bytes(f,p,offset,data,length);
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
  fresh=::open(nextwal.c_str(),O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0600);require(fresh>=0,"create checkpoint WAL failed");sync_file(fresh);++wal_syncs;
  auto nextshadow=root/("shadow."+std::to_string(next));std::filesystem::remove_all(nextshadow);std::filesystem::create_directories(nextshadow);
  nextfd=::open((nextshadow/"journal").c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);require(nextfd>=0,"create shadow journal failed");sync_file(nextfd);sync_directory(nextshadow);
  auto pending=root/"CURRENT.tmp";int fd=::open(pending.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);require(fd>=0,"create checkpoint manifest failed");auto value="MV3 "+std::to_string(next)+"\n";write_all(fd,value.data(),value.size());sync_file(fd);::close(fd);sync_directory(root);if(stage)stage(2);
  std::filesystem::rename(pending,root/"CURRENT");sync_directory(root);
  auto oldwal=wal_path(root,generation),oldshadow=shadow_directory;
  ::close(wal);::close(shadow);wal=fresh;shadow=nextfd;fresh=nextfd=-1;generation=next;shadow_directory=nextshadow;shadow_index=nextshadow/"index";written_directory=nextshadow/"written";for(size_t i=0;i<written_count;++i)written_cache[i]={};written_cursor=0;std::filesystem::create_directory(shadow_index);std::filesystem::create_directory(written_directory);
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
