#include "engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <bit>
#include <span>
#include <stdexcept>
#include <unistd.h>
namespace mvdb {
namespace {
uint64_t total_spill_bytes=0;
void require(bool yes,std::string_view s){if(!yes)throw std::runtime_error(std::string(s));}
struct Reservation {MemoryBudget& b;size_t n;std::weak_ptr<char> lifetime;Reservation(MemoryBudget& budget,size_t bytes):b(budget),n(bytes),lifetime(budget.lifetime){b.acquire(n);}~Reservation(){if(!lifetime.expired())b.release(n);}};
size_t column(const std::vector<Column>& cols,const std::string& name){for(size_t i=0;i<cols.size();++i)if(cols[i].name==name)return i;throw std::runtime_error("unknown column: "+name);}
size_t primary(const std::vector<Column>& cols){for(size_t i=0;i<cols.size();++i)if(cols[i].primary)return i;throw std::runtime_error("missing primary key");}
size_t stride(const Column& c){return ((c.width+7)/8)*32;}
uint64_t scalar_hash(const Value& value){
  uint64_t h=1469598103934665603ULL;
  auto feed=[&](const void* p,size_t n){auto bytes=static_cast<const unsigned char*>(p);for(size_t i=0;i<n;++i){h^=bytes[i];h*=1099511628211ULL;}};
  if(auto s=std::get_if<std::string>(&value))feed(s->data(),s->size());
  else if(auto n=std::get_if<int64_t>(&value))feed(n,sizeof(*n));
  else throw std::runtime_error("invalid sketch scalar");
  return h;
}
// Dense and sparse scans both load a validity word once; repeating a generic
// next-bit lookup for every live slot penalizes dense full-table UPDATEs.
template<class F>void each_set_bit(const std::array<uint64_t,64>& bits,size_t begin,size_t end,F&& visit){
  for(size_t wi=begin/64;wi<(end+63)/64;++wi){auto word=bits[wi];
    if(wi==begin/64)word&=UINT64_MAX<<(begin%64);
    if(wi==end/64&&end%64)word&=(uint64_t(1)<<(end%64))-1;
    while(word){auto bit=std::countr_zero(word);word&=word-1;visit(wi*64+bit);}
  }
}
constexpr off_t SPILL_CACHE_CHUNK=4*1024*1024;
// Offsets are uint32 slot ordinals in one shared file. Directory and posting
// payload share ordinary pages, so an empty list consumes no separate page.
struct PackedList {uint64_t offset=0;uint32_t count=0,capacity=0;};
struct PackedDirectory {uint64_t magic=0x31465649504dULL,next=0;PackedList lists[64]{};};
static_assert(sizeof(PackedDirectory)<PAGE);
void spill_progress(FILE* f,off_t before){auto after=::ftello(f);if(before/SPILL_CACHE_CHUNK!=after/SPILL_CACHE_CHUNK)::posix_fadvise(::fileno(f),0,after/PAGE*PAGE,POSIX_FADV_DONTNEED);}
// Sort/result files are disposable after process death. They need no durability
// syncs; advise the kernel only after several MiB of sequential work.
void spill_write(FILE* f,const std::vector<Value>& row){auto before=::ftello(f);auto s=pack_values(row);uint32_t n=static_cast<uint32_t>(s.size());require(std::fwrite(&n,4,1,f)==1&&std::fwrite(s.data(),1,s.size(),f)==s.size(),"result spill failed");total_spill_bytes+=4+s.size();spill_progress(f,before);}
bool spill_read(FILE* f,std::vector<Value>& row){auto before=::ftello(f);uint32_t n=0;auto count=std::fread(&n,1,4,f);if(!count){require(!std::ferror(f),"result read failed");return false;}require(count==4&&n<=16*1024*1024,"corrupt spill record");std::string s(n,'\0');require(std::fread(s.data(),1,n,f)==n,"truncated spill record");row=unpack_values(s);spill_progress(f,before);return true;}
void discard_cache(FILE* f){auto pos=::ftello(f);if(pos>0)::posix_fadvise(::fileno(f),0,pos/PAGE*PAGE,POSIX_FADV_DONTNEED);}
class Sorter {
  Storage& storage;
  Reservation memory;
  size_t used=0,capacity;
  std::vector<std::vector<Value>> chunk;
  std::vector<std::shared_ptr<FILE>> levels;
  std::function<bool(const std::vector<Value>&,const std::vector<Value>&)> less;
  std::shared_ptr<FILE> merge(std::shared_ptr<FILE> a,std::shared_ptr<FILE> b){
    std::rewind(a.get());std::rewind(b.get());auto out=storage.temporary();std::vector<Value> x,y;
    bool ax=spill_read(a.get(),x),by=spill_read(b.get(),y);
    while(ax||by){if(ax&&(!by||!less(y,x))){spill_write(out.get(),x);ax=spill_read(a.get(),x);}
      else{spill_write(out.get(),y);by=spill_read(b.get(),y);}}
    discard_cache(a.get());discard_cache(b.get());discard_cache(out.get());return out;
  }
  void run(){if(chunk.empty())return;std::sort(chunk.begin(),chunk.end(),less);auto f=storage.temporary();for(auto& r:chunk)spill_write(f.get(),r);chunk.clear();used=0;
    discard_cache(f.get());size_t i=0;
    while(i<levels.size()&&levels[i]){f=merge(levels[i],f);levels[i].reset();++i;}
    if(i==levels.size())levels.emplace_back();
    levels[i]=f;
  }
public:
  Sorter(Storage& s,decltype(less) fn):storage(s),memory(s.budget,(s.budget.limit-s.budget.used)/2),capacity(memory.n/2),less(std::move(fn)){}
  void add(std::vector<Value> values){size_t n=pack_values(values).size()*3+values.size()*sizeof(Value)+128;
    require(n<=capacity,"query row exceeds workspace budget");if(used+n>capacity)run();used+=n;chunk.push_back(std::move(values));}
  std::shared_ptr<FILE> finish(){run();std::shared_ptr<FILE> out;for(auto& f:levels)if(f)out=out?merge(out,f):f;if(!out)out=storage.temporary();std::rewind(out.get());return out;}
};
}
void Result::each(const std::function<void(const std::vector<Value>&)>& callback) const {if(numeric_count){std::vector<Value> row(2);for(size_t i=0;i<numeric_count;++i){row[numeric_distance_column]=numeric_rows[i].distance;row[1-numeric_distance_column]=numeric_rows[i].key;callback(row);}return;}if(memory_rows){for(auto& r:*memory_rows)callback(r);return;}if(!rows)return;std::rewind(rows.get());std::vector<Value> r;while(spill_read(rows.get(),r))callback(r);discard_cache(rows.get());}
uint64_t Engine::spill_bytes() const{return total_spill_bytes;}
Engine::Header Engine::header(const std::string& t){auto h=storage.load<Header>(t+".header",0);if(h.magic!=0x4d564442)throw std::runtime_error("table does not exist: "+t);return h;}
std::vector<Column> Engine::schema(const Header& h){require(h.schema_size<sizeof(h.schema),"corrupt schema");return parse(std::string_view(h.schema,h.schema_size)).columns;}
Engine::Meta Engine::meta(const std::string& t,SlotId s){auto p=storage.pool.view(t+".rows",s/(PAGE/sizeof(Meta)));Meta m;std::memcpy(&m,p+(s%(PAGE/sizeof(Meta)))*sizeof(Meta),sizeof(m));return m;}
void Engine::put_meta(const std::string& t,SlotId s,const Meta& m){auto id=s/(PAGE/sizeof(Meta));auto p=storage.read(t+".rows",id);std::memcpy(p.data()+(s%(PAGE/sizeof(Meta)))*sizeof(Meta),&m,sizeof(m));storage.write(t+".rows",id,p);}
std::string Engine::read_blob(const std::string& t,uint64_t offset,uint32_t n){std::string out;out.reserve(n);while(n){auto page=storage.pool.view(t+".blob",offset/PAGE);auto count=std::min<uint64_t>(n,PAGE-offset%PAGE);out.append(reinterpret_cast<const char*>(page+offset%PAGE),count);offset+=count;n-=count;}return out;}
void Engine::write_blob(const std::string& t,uint64_t offset,const std::string& s,bool append){size_t pos=0;while(pos<s.size()){auto p=offset/PAGE;auto page=storage.read(t+".blob",p);auto n=std::min<uint64_t>(s.size()-pos,PAGE-offset%PAGE);std::memcpy(page.data()+offset%PAGE,s.data()+pos,n);if(append)storage.append(t+".blob",p,page,offset%PAGE,n);else storage.write(t+".blob",p,page);offset+=n;pos+=n;}}
const float* Engine::vector_view(const std::string& t,SlotId s,size_t i,const Column& c){auto count=PAGE/stride(c);auto p=storage.pool.view(t+".v"+std::to_string(i),s/count);return reinterpret_cast<const float*>(p+(s%count)*stride(c));}
const float* Engine::center_view(const std::string& t,uint32_t b,size_t dim){auto bytes=((dim+7)/8)*32;auto count=PAGE/bytes;auto p=storage.pool.view(t+".centers",b/count);return reinterpret_cast<const float*>(p+(b%count)*bytes);}
std::vector<Value> Engine::row(const std::string& t,SlotId s,const Meta& m,const std::vector<Column>& cols,bool vectors){auto r=unpack_values(read_blob(t,m.offset,m.length));require(r.size()==cols.size(),"corrupt row width");if(vectors)for(size_t i=0;i<cols.size();++i)if(cols[i].type==Type::Vector){auto p=vector_view(t,s,i,cols[i]);r[i]=std::vector<float>(p,p+cols[i].width);}return r;}
void Engine::bitmap(const std::string& t,SlotId slot,bool active){auto p=slot/(PAGE*8);auto page=storage.read(t+".valid",p);auto bit=slot%(PAGE*8);auto x=std::to_integer<unsigned char>(page[bit/8]);x=active?x|(1U<<(bit%8)):x&~(1U<<(bit%8));page[bit/8]=std::byte(x);storage.write(t+".valid",p,page);}
void Engine::posting(const std::string& t,uint32_t bucket,SlotId slot,bool add){
  if(packed_index(t)){
    auto f=t+".ivf";auto directory=storage.load<PackedDirectory>(f,0);require(directory.magic==0x31465649504dULL&&bucket<64,"corrupt packed IVF directory");auto& list=directory.lists[bucket];
    auto get=[&](uint64_t i){auto offset=i*4;auto p=storage.pool.view(f,offset/PAGE);uint32_t x;std::memcpy(&x,p+offset%PAGE,4);return x;};
    auto put=[&](uint64_t i,uint32_t x){auto offset=i*4;auto p=storage.read(f,offset/PAGE);std::memcpy(p.data()+offset%PAGE,&x,4);storage.write(f,offset/PAGE,p);};
    if(add){require(list.count<UINT32_MAX,"IVF list full");if(list.count==list.capacity){
      uint32_t capacity=static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,std::max<uint64_t>(8,uint64_t(list.capacity)*2)));auto next=directory.next;
      for(uint32_t i=0;i<list.count;++i){auto value=get(list.offset+i);put(next+i,value);}list.offset=next;list.capacity=capacity;directory.next+=capacity;
    }put(list.offset+list.count++,slot);
    }else for(uint32_t i=0;i<list.count;++i)if(get(list.offset+i)==slot){auto last=get(list.offset+list.count-1);--list.count;if(i<list.count)put(list.offset+i,last);break;}
    // The directory shares page zero with payload; preserve the latest slots.
    auto page=storage.read(f,0);std::memcpy(page.data(),&directory,sizeof(directory));storage.write(f,0,page);return;
  }
  auto f=t+".ivf"+std::to_string(bucket);auto h=storage.read(f,0);uint32_t n;std::memcpy(&n,h.data(),4);
  auto get=[&](uint32_t i){uint64_t offset=16+uint64_t(i)*4;auto p=storage.read(f,offset/PAGE);uint32_t x;std::memcpy(&x,p.data()+offset%PAGE,4);return x;};
  auto put=[&](uint32_t i,uint32_t x){uint64_t offset=16+uint64_t(i)*4;auto p=storage.read(f,offset/PAGE);std::memcpy(p.data()+offset%PAGE,&x,4);storage.write(f,offset/PAGE,p);};
  if(add){require(n<UINT32_MAX,"IVF list full");put(n++,slot);}else{for(uint32_t i=0;i<n;++i)if(get(i)==slot){auto last=get(n-1);--n;if(i<n)put(i,last);break;}}
  // Slot writes can modify page zero; reload before publishing its count.
  h=storage.read(f,0);std::memcpy(h.data(),&n,4);storage.write(f,0,h);
}
bool Engine::packed_index(const std::string& t){return t.find(".index.p.r")!=std::string::npos;}
Engine::PostingList Engine::posting_list(const std::string& t,uint32_t b){
  if(packed_index(t)){auto d=storage.load<PackedDirectory>(t+".ivf",0);require(d.magic==0x31465649504dULL&&b<64,"corrupt packed IVF directory");return {t+".ivf",d.lists[b].offset*4,d.lists[b].count};}
  auto f=t+".ivf"+std::to_string(b);auto p=storage.pool.view(f,0);uint32_t count;std::memcpy(&count,p,4);return {f,16,count};
}
std::string Engine::index_base(const std::string& t,const Header& h) const {return h.index_region[0]?std::string(h.index_region):t;}
uint32_t Engine::bucket(const std::string& t,const Header& h,SlotId slot){
  if(!h.index_region[0])return meta(t,slot).bucket;
  auto p=storage.pool.view(index_base(t,h)+".buckets",slot/(PAGE/4));uint32_t b;std::memcpy(&b,p+(slot%(PAGE/4))*4,4);return b;
}
void Engine::put_bucket(const std::string& t,const Header& h,SlotId slot,uint32_t b){
  auto f=index_base(t,h)+".buckets";auto p=storage.read(f,slot/(PAGE/4));std::memcpy(p.data()+(slot%(PAGE/4))*4,&b,4);storage.write(f,slot/(PAGE/4),p);
}
uint32_t Engine::assign(const std::string& t,const Header& h,const std::vector<float>& v){uint32_t best=0;double distance=std::numeric_limits<double>::infinity();bool safe=!has_extreme(v.data(),v.size());for(uint32_t b=0;b<h.nlist;++b){auto p=center_view(index_base(t,h),b,v.size());auto d=l2_fast(v.data(),p,v.size(),safe&&!has_extreme(p,v.size()));if(d<distance){distance=d;best=b;}}return best;}
void Engine::write_row(const std::string& t,SlotId slot,std::vector<Value> values,const std::vector<Column>& cols,bool active){
  auto h=header(t);auto old=slot<h.slots?meta(t,slot):Meta{};PrimaryIndex pk(storage,t+".pk");int64_t key=std::get<int64_t>(values[primary(cols)]);
  const bool same_key=old.active&&active&&old.key==key;
  if(active&&!same_key){auto found=pk.find(key);require(!found||*found==slot,"duplicate primary key");}
  if(old.active&&!same_key)pk.erase(old.key);
  if(old.active&&!active&&h.nlist)posting(index_base(t,h),bucket(t,h,slot),slot,false);
  Meta m=old;m.active=active;m.key=key;m.reserved=0;
  if(active){
    if(h.nlist){auto& v=std::get<std::vector<float>>(values[h.ivector]);bool unchanged=old.active&&std::memcmp(vector_view(t,slot,h.ivector,cols[h.ivector]),v.data(),v.size()*sizeof(float))==0;
      if(!unchanged){auto prior=old.active?bucket(t,h,slot):h.nlist;m.bucket=assign(t,h,v);if(prior!=m.bucket){if(old.active)posting(index_base(t,h),prior,slot,false);put_bucket(t,h,slot,m.bucket);posting(index_base(t,h),m.bucket,slot,true);}}
    }
    for(size_t i=0;i<cols.size();++i)if(cols[i].type==Type::Vector){auto& v=std::get<std::vector<float>>(values[i]);m.reserved|=has_extreme(v.data(),v.size())?1:0;
      bool unchanged=old.active&&std::memcmp(vector_view(t,slot,i,cols[i]),v.data(),v.size()*sizeof(float))==0;
      if(!unchanged){auto count=PAGE/stride(cols[i]);auto p=slot/count;auto bytes=storage.read(t+".v"+std::to_string(i),p);std::memcpy(bytes.data()+(slot%count)*stride(cols[i]),v.data(),v.size()*4);if(slot>=h.slots)storage.append(t+".v"+std::to_string(i),p,bytes,(slot%count)*stride(cols[i]),v.size()*4);else storage.write(t+".v"+std::to_string(i),p,bytes);}values[i]=std::vector<float>{};}
    for(size_t i=0;i<cols.size();++i)if(cols[i].type!=Type::Vector&&!cols[i].primary){
      auto f=t+".s"+std::to_string(i);auto p=slot/(PAGE/8);auto hash=scalar_hash(values[i]);uint64_t prior=0;if(old.active)std::memcpy(&prior,storage.pool.view(f,p)+(slot%(PAGE/8))*8,8);
      if(!old.active||prior!=hash){auto bytes=storage.read(f,p);std::memcpy(bytes.data()+(slot%(PAGE/8))*8,&hash,8);storage.write(f,p,bytes);}
    }
    auto bytes=pack_values(values);require(bytes.size()<storage.budget.limit/4,"row exceeds workspace budget");
    if(!old.active||read_blob(t,old.offset,old.length)!=bytes){m.offset=old.active&&bytes.size()<=old.length?old.offset:h.blob_end;m.length=static_cast<uint32_t>(bytes.size());write_blob(t,m.offset,bytes,m.offset==h.blob_end);if(m.offset==h.blob_end)h.blob_end+=bytes.size();}
    if(!same_key)pk.insert(key,slot);
  }
  if(slot>=h.slots){require(slot<UINT32_MAX,"SlotId exhausted");h.slots=slot+1;}
  if(active&&!old.active)++h.active_count;
  if(!active&&old.active&&h.active_count)--h.active_count;
  // Header writes use the same statement/transaction undo path as row pages.
  // Count each affected row once, including scalar updates and repeated writes.
  if(h.nlist&&(active||old.active)&&h.mutations_since_calibration<UINT64_MAX)++h.mutations_since_calibration;
  if(std::memcmp(&m,&old,sizeof(m))!=0)put_meta(t,slot,m);
  if(bool(old.active)!=active)bitmap(t,slot,active);
  storage.store(t+".header",0,h);
}
void Engine::create_index(const Statement& s,const std::vector<Column>& cols){
  auto h=header(s.table);require(!h.nlist,"vector index already exists");auto col=column(cols,s.value);require(cols[col].type==Type::Vector,"index requires VECTOR column");
  uint32_t count=0;for(SlotId slot=0;slot<h.slots;++slot)if(meta(s.table,slot).active)++count;
  h.nlist=std::min<uint32_t>(count>=4096?64:8,count);h.ivector=static_cast<uint32_t>(col);require(h.nlist>0,"cannot train IVF on empty table");
  prepare_vector_residency(s.table,h,cols);
  auto region=storage.begin_region(s.table+".index.p");require(region.size()<sizeof(h.index_region),"index namespace too long");
  std::memcpy(h.index_region,region.c_str(),region.size()+1);h.active_count=count;
  auto dim=cols[col].width;
  {
  constexpr size_t reseed_bytes=sizeof(std::pair<double,SlotId>)*64+sizeof(uint32_t)*64;
  size_t full=h.nlist*dim*(sizeof(double)+sizeof(float))+1024;
  bool narrow=full>storage.budget.limit-storage.budget.used;
  size_t training_bytes=narrow?h.nlist*dim*sizeof(float)*2+dim*sizeof(double)+1024:full;
  const bool reseed_enabled=training_bytes+reseed_bytes<=storage.budget.limit-storage.budget.used;
  if(reseed_enabled)training_bytes+=reseed_bytes;
  Reservation reserve(storage.budget,training_bytes);
  std::vector<std::vector<float>> centers;uint32_t ordinal=0,next=0;
  for(SlotId slot=0;slot<h.slots&&centers.size()<h.nlist;++slot)if(meta(s.table,slot).active){if(ordinal==next){auto p=vector_view(s.table,slot,col,cols[col]);centers.emplace_back(p,p+dim);next=static_cast<uint32_t>(centers.size()*count/h.nlist);}++ordinal;}
  for(int iteration=0;iteration<3;++iteration){
    std::vector<bool> safe;for(auto& center:centers)safe.push_back(!has_extreme(center.data(),dim));
    // Keep only at most nlist farthest input ordinals during the existing
    // assignment pass. Empty-cluster repair never adds a full vector scan.
    std::array<std::pair<double,SlotId>,64> farthest{};size_t farthest_count=0;
    auto farther=[](const auto& a,const auto& b){return a.first!=b.first?a.first>b.first:a.second<b.second;};
    auto remember=[&](double distance,SlotId slot){if(!reseed_enabled)return;auto entry=std::make_pair(distance,slot);
      if(farthest_count<h.nlist){farthest[farthest_count++]=entry;std::push_heap(farthest.begin(),farthest.begin()+farthest_count,farther);}
      else if(farther(entry,farthest[0])){std::pop_heap(farthest.begin(),farthest.begin()+farthest_count,farther);farthest[farthest_count-1]=entry;std::push_heap(farthest.begin(),farthest.begin()+farthest_count,farther);}
    };
    auto nearest=[&](const float* p,bool normal,double* chosen=nullptr){uint32_t best=0;double distance=INFINITY;for(uint32_t b=0;b<h.nlist;++b){auto d=l2_fast(p,centers[b].data(),dim,normal&&safe[b]);if(d<distance){distance=d;best=b;}}if(chosen)*chosen=distance;return best;};
    auto reseed=[&](auto& next,const auto& sizes){
      if(!reseed_enabled||std::none_of(sizes.begin(),sizes.end(),[](auto n){return n==0;}))return;
      std::sort(farthest.begin(),farthest.begin()+farthest_count,farther);
      size_t pick=0;for(uint32_t b=0;b<h.nlist;++b)if(!sizes[b]&&pick<farthest_count&&farthest[pick].first>0){
        auto slot=farthest[pick++].second;auto p=vector_view(s.table,slot,col,cols[col]);std::copy(p,p+dim,next[b].begin());++empty_cluster_reseeds;
      }
      // Coincident points have zero residual to every center: no reseeding
      // can manufacture distinct nonempty Euclidean clusters in that case.
    };
    if(narrow){
      auto next=centers;std::array<uint32_t,64> sizes{};
      for(uint32_t b=0;b<h.nlist;++b){std::vector<double> sum(dim);uint32_t count_in_bucket=0;
        for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(s.table,slot);if(!m.active)continue;auto p=vector_view(s.table,slot,col,cols[col]);double distance;auto best=nearest(p,m.reserved==0,&distance);if(b==0)remember(distance,slot);if(best!=b)continue;++count_in_bucket;for(size_t j=0;j<dim;++j)sum[j]+=p[j];}
        sizes[b]=count_in_bucket;
        if(count_in_bucket)for(size_t j=0;j<dim;++j)next[b][j]=static_cast<float>(sum[j]/count_in_bucket);
      }reseed(next,std::span<const uint32_t>(sizes.data(),h.nlist));centers.swap(next);
    }else{
      std::vector<std::vector<double>> sums(h.nlist,std::vector<double>(dim));std::vector<uint32_t> sizes(h.nlist);
      for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(s.table,slot);if(!m.active)continue;auto p=vector_view(s.table,slot,col,cols[col]);double distance;auto best=nearest(p,m.reserved==0,&distance);remember(distance,slot);++sizes[best];for(size_t j=0;j<dim;++j)sums[best][j]+=p[j];}
      for(uint32_t b=0;b<h.nlist;++b)if(sizes[b])for(size_t j=0;j<dim;++j)centers[b][j]=static_cast<float>(sums[b][j]/sizes[b]);
      reseed(centers,sizes);
    }
  }
  for(uint32_t b=0;b<h.nlist;++b){auto count=PAGE/stride(cols[col]);auto page=storage.read(region+".centers",b/count);std::memcpy(page.data()+(b%count)*stride(cols[col]),centers[b].data(),dim*4);storage.write_new(region+".centers",b/count,page);}
  std::vector<bool> safe;for(auto& center:centers)safe.push_back(!has_extreme(center.data(),dim));
  std::array<uint32_t,64> sizes{};Page assignments{};
  for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(s.table,slot);uint32_t b=UINT32_MAX;if(m.active){b=0;auto p=vector_view(s.table,slot,col,cols[col]);double nearest=INFINITY;
    for(uint32_t candidate=0;candidate<h.nlist;++candidate){auto distance=l2_fast(p,centers[candidate].data(),dim,m.reserved==0&&safe[candidate]);if(distance<nearest){nearest=distance;b=candidate;}}
    ++sizes[b];}std::memcpy(assignments.data()+(slot%(PAGE/4))*4,&b,4);
    if(slot%(PAGE/4)==PAGE/4-1||slot+1==h.slots){storage.write_new(region+".buckets",slot/(PAGE/4),assignments);assignments={};}}
  PackedDirectory directory;directory.next=(sizeof(directory)+3)/4;
  for(uint32_t b=0;b<h.nlist;++b){directory.lists[b]={directory.next,sizes[b],sizes[b]};directory.next+=sizes[b];}
  Page packed{};std::memcpy(packed.data(),&directory,sizeof(directory));uint64_t offset=(sizeof(directory)+3)/4*4,page_id=0;
  // Read only the small assignment map for each list. Vector payload is never
  // copied or rescanned, and each packed output page is written once.
  for(uint32_t b=0;b<h.nlist;++b)for(SlotId slot=0;slot<h.slots;++slot){if(bucket(s.table,h,slot)!=b)continue;
    std::memcpy(packed.data()+offset%PAGE,&slot,4);offset+=4;if(offset%PAGE==0){storage.write_new(region+".ivf",page_id++,packed);packed={};}}
  if(offset%PAGE)storage.write_new(region+".ivf",page_id,packed);
  }
  h.calibration_skipped=detail::skip_ivf_calibration_for_payload(count,uint64_t(count)*dim*sizeof(float),storage.budget.limit);
  h.calibration_queries=h.calibration_skipped?0:16;
  h.recommended_nprobe=h.calibration_skipped?std::min(h.nlist,std::max<uint32_t>(4,h.nlist/4)):calibrate(s.table,h,cols);h.calibration_stale=0;
  h.mutations_since_calibration=0;h.rows_at_calibration=count;
  last_lists=h.nlist;last_recommended=h.recommended_nprobe;
  // Publish only after all fresh-region pages and calibration are complete.
  storage.seal_region(region);
  storage.store(s.table+".header",0,h);
}
uint32_t Engine::calibrate(const std::string& t,Header& h,const std::vector<Column>& cols){
  // Keep 16 query vectors and bounded Top10 heaps. Exact ground truth is built
  // for all queries in one sequential pass over vector pages, never a matrix.
  const auto dim=cols[h.ivector].width;
  struct Neighbor {double distance;int64_t key;uint32_t bucket;};
  struct Query {
    std::vector<float> vector;
    std::array<uint32_t,64> ranks{};
    std::array<Neighbor,10> top{};
    uint32_t count=0,ordinal=0;
    SlotId sample=UINT32_MAX;
    int category=-1;
  };
  auto workspace_bytes=16*(dim*sizeof(float)+sizeof(Query))+8192+h.nlist*32;
  if(workspace_bytes>storage.budget.limit-storage.budget.used){h.calibration_skipped=1;h.calibration_queries=0;return std::min(h.nlist,std::max<uint32_t>(4,h.nlist/4));}
  Reservation workspace(storage.budget,workspace_bytes);
  struct Category {std::string value;uint32_t votes=0,count=0;};
  std::vector<Category> categories;size_t filter_col=cols.size();
  for(size_t c=0;c<cols.size();++c)if(cols[c].type==Type::Varchar&&cols[c].width<=64){filter_col=c;break;}
  if(filter_col<cols.size()){
    // Misra-Gries bounds the dictionary even on a unique-string column.
    for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(t,slot);if(!m.active)continue;
      auto values=row(t,slot,m,cols,false);auto& value=std::get<std::string>(values[filter_col]);
      auto it=std::find_if(categories.begin(),categories.end(),[&](const auto& c){return c.value==value;});
      if(it!=categories.end())++it->votes;
      else if(categories.size()<16)categories.push_back({value,1,0});
      else {for(auto& c:categories)--c.votes;categories.erase(std::remove_if(categories.begin(),categories.end(),[](const auto& c){return !c.votes;}),categories.end());}
    }
    for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(t,slot);if(!m.active)continue;auto values=row(t,slot,m,cols,false);
      for(auto& c:categories)if(std::get<std::string>(values[filter_col])==c.value)++c.count;
    }
    categories.erase(std::remove_if(categories.begin(),categories.end(),[&](const auto& c){return c.count<std::max<uint32_t>(10,h.active_count/100);}),categories.end());
    std::sort(categories.begin(),categories.end(),[](const auto& a,const auto& b){return a.count!=b.count?a.count>b.count:a.value<b.value;});
    if(categories.size()>4)categories.resize(4);
  }
  std::array<Query,16> queries;
  for(uint32_t qi=0;qi<queries.size();++qi){auto& q=queries[qi];if(qi>=8&&!categories.empty())q.category=static_cast<int>((qi-8)%categories.size());
    auto count=q.category>=0?categories[q.category].count:h.active_count;
    q.ordinal=static_cast<uint32_t>((uint64_t(qi+1)*2654435761ULL+1013904223ULL)%count);
  }
  // Select all samples together. No query causes another table-vector scan.
  for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(t,slot);if(!m.active)continue;int category=-1;
    if(!categories.empty()){auto values=row(t,slot,m,cols,false);for(size_t c=0;c<categories.size();++c)if(std::get<std::string>(values[filter_col])==categories[c].value){category=static_cast<int>(c);break;}}
    for(auto& q:queries)if(q.sample==UINT32_MAX&&(q.category<0||q.category==category)&&q.ordinal--==0)q.sample=slot;
  }
  for(auto& q:queries){require(q.sample!=UINT32_MAX,"calibration sample missing");auto p=vector_view(t,q.sample,h.ivector,cols[h.ivector]);q.vector.assign(p,p+dim);
    // Perturb the sample toward another live vector to avoid self-match bias.
    SlotId other=(q.sample+h.slots/2+1)%h.slots;
    for(uint32_t n=0;n<h.slots&&!meta(t,other).active;++n)other=(other+1)%h.slots;
    p=vector_view(t,other,h.ivector,cols[h.ivector]);for(size_t j=0;j<dim;++j)q.vector[j]=static_cast<float>(double(q.vector[j])*.99+double(p[j])*.01);
    std::vector<std::pair<double,uint32_t>> centers;for(uint32_t b=0;b<h.nlist;++b){p=center_view(index_base(t,h),b,dim);centers.emplace_back(l2(q.vector.data(),p,dim),b);}std::sort(centers.begin(),centers.end());
    for(uint32_t b=0;b<h.nlist;++b)q.ranks[centers[b].second]=b+1;
  }
  auto less=[](const Neighbor& a,const Neighbor& b){return a.distance!=b.distance?a.distance<b.distance:a.key<b.key;};
  BufferPool::PageGuard lease;uint64_t leased_page=UINT64_MAX;const auto per_page=PAGE/stride(cols[h.ivector]);
  for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(t,slot);if(!m.active)continue;int category=-1;
    if(!categories.empty()){auto values=row(t,slot,m,cols,false);for(size_t c=0;c<categories.size();++c)if(std::get<std::string>(values[filter_col])==categories[c].value){category=static_cast<int>(c);break;}}
    auto b=bucket(t,h,slot);auto page=slot/per_page;if(leased_page!=page){lease.reset();lease=storage.pool.lease(t+".v"+std::to_string(h.ivector),page);leased_page=page;}
    auto p=reinterpret_cast<const float*>(lease.data+(slot%per_page)*stride(cols[h.ivector]));
    for(auto& q:queries){if(q.category>=0&&q.category!=category)continue;Neighbor candidate{l2_exact(q.vector.data(),p,dim),m.key,b};
      if(q.count==10&&!less(candidate,q.top[q.count-1]))continue;
      auto pos=static_cast<size_t>(std::lower_bound(q.top.begin(),q.top.begin()+q.count,candidate,less)-q.top.begin());
      if(q.count<10)++q.count;
      for(size_t n=q.count-1;n>pos;--n)q.top[n]=q.top[n-1];
      q.top[pos]=candidate;
    }
  }
  std::array<std::array<uint64_t,65>,2> hits{};std::array<uint64_t,2> totals{};
  for(auto& q:queries){auto group=q.category>=0?1:0;totals[group]+=q.count;for(uint32_t n=0;n<q.count;++n)for(uint32_t probe=q.ranks[q.top[n].bucket];probe<=h.nlist;++probe)++hits[group][probe];}
  for(uint32_t probe=1;probe<=h.nlist;++probe){bool good=true;for(unsigned group=0;group<2;++group)if(totals[group]&&double(hits[group][probe])/totals[group]<.985)good=false;if(good)return probe;}
  return h.nlist;
}
void Engine::prepare_vector_residency(const std::string& t,const Header& h,const std::vector<Column>& cols){
  const char* setting=std::getenv("MVDB_EXCLUSIVE_CACHE");
  if(!preparation_cache_allowed||storage.budget.limit<=8*1024*1024||(setting&&std::string_view(setting)=="off"))return;
  const auto per_page=PAGE/stride(cols[h.ivector]);
  const auto pages=(uint64_t(h.slots)+per_page-1)/per_page;
  prepared_vectors=storage.pool.load_residency(t+".v"+std::to_string(h.ivector),pages,std::max<size_t>(32,storage.pool.frame_capacity()/8));
  if(prepared_vectors)prepared_table=t;
}
Engine::TableReadCache* Engine::cache(const std::string& t,const Header& h,const std::vector<Column>& cols){
  if(read_cache&&read_cache->table==t)return read_cache.get();
  read_cache.reset();
  if(prepared_table!=t){prepared_vectors.reset();prepared_table.clear();}
  if(h.slots>25000)return nullptr;
  size_t varchar_count=0;for(auto& c:cols)varchar_count+=c.type==Type::Varchar;
  size_t center_count=h.nlist?size_t(h.nlist)*cols[h.ivector].width:0;
  size_t vector_pages=0;for(auto& col:cols)if(col.type==Type::Vector){auto count=PAGE/stride(col);vector_pages=(h.slots+count-1)/count;}
  // Charge fixed capacities, including dictionary strings and container metadata.
  // High cardinality columns stop after 128 distinct values or 16 KiB UTF-8.
  size_t bytes=4096+size_t(h.slots)*17+center_count*4+size_t(h.nlist+2)*4+vector_pages*sizeof(BufferPool::ResidentHandle)+
    cols.size()*sizeof(TableReadCache::Dictionary)+varchar_count*(size_t(h.slots)*6+32768+129*sizeof(uint32_t));
  // Small budgets keep the original transient workspace admission. Retaining
  // a full reusable heap there could displace an otherwise admissible cache.
  const bool reuse=storage.budget.limit>=4*1024*1024;
  if(reuse)bytes+=size_t(h.slots)*sizeof(SlotId)+vector_pages*sizeof(const std::byte*)+size_t(h.nlist)*sizeof(std::pair<double,uint32_t>)+4096*sizeof(TableReadCache::Neighbor)+64*sizeof(std::pair<size_t,uint16_t>);
  if(bytes>(storage.budget.limit-storage.budget.used)/2)return nullptr;
  auto c=std::make_unique<TableReadCache>();c->reservation=std::make_shared<Reservation>(storage.budget,bytes);
  if(reuse){c->filters.reserve(64);c->bases.resize(vector_pages);c->candidates.reserve(h.slots);c->distances.reserve(h.nlist);c->heap.reserve(4096);}
  c->vector_pages.resize(vector_pages);c->table=t;c->slots=h.slots;c->keys.resize(h.slots);c->flags.resize(h.slots);c->dictionaries.resize(cols.size());c->active_slots.reserve(h.slots);
  for(size_t i=0;i<cols.size();++i)if(cols[i].type==Type::Varchar){auto& d=c->dictionaries[i];d.supported=true;d.ids.resize(h.slots);d.values.reserve(128);}
  std::vector<size_t> string_bytes(cols.size());
  auto lists=std::max(1U,h.nlist);c->offsets.resize(lists+1);
  for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(t,slot);c->keys[slot]=m.key;c->flags[slot]=(m.active?1:0)|(m.reserved?2:0);if(!m.active)continue;c->active_slots.push_back(slot);
    ++c->offsets[1+(h.nlist?bucket(t,h,slot):0)];
    bool needs=false;for(auto& d:c->dictionaries)needs|=d.supported;if(!needs)continue;
    auto values=row(t,slot,m,cols,false);
    for(size_t i=0;i<cols.size();++i){auto& d=c->dictionaries[i];if(!d.supported)continue;auto& value=std::get<std::string>(values[i]);auto it=std::find(d.values.begin(),d.values.end(),value);
      if(it==d.values.end()){
        if(d.values.size()==128||string_bytes[i]+value.size()>16384){d.supported=false;std::vector<uint16_t>().swap(d.ids);std::vector<std::string>().swap(d.values);continue;}
        string_bytes[i]+=value.size();d.values.push_back(value);it=d.values.end()-1;
      }
      d.ids[slot]=static_cast<uint16_t>(it-d.values.begin());
    }
  }
  for(auto& d:c->dictionaries)if(d.supported){
    d.offsets.resize(d.values.size()+1);
    for(auto slot:c->active_slots)++d.offsets[d.ids[slot]+1];
    for(size_t i=1;i<d.offsets.size();++i)d.offsets[i]+=d.offsets[i-1];
    d.slot_ids.resize(c->active_slots.size());auto next=d.offsets;
    for(auto slot:c->active_slots)d.slot_ids[next[d.ids[slot]]++]=slot;
  }
  for(size_t i=1;i<c->offsets.size();++i)c->offsets[i]+=c->offsets[i-1];
  c->postings.resize(c->offsets.back());auto next=c->offsets;
  for(SlotId slot=0;slot<h.slots;++slot)if(c->flags[slot]&1)c->postings[next[h.nlist?bucket(t,h,slot):0]++]=slot;
  c->centers.resize(center_count);
  for(uint32_t b=0;b<h.nlist;++b){auto dim=cols[h.ivector].width;auto p=center_view(index_base(t,h),b,dim);std::copy(p,p+dim,c->centers.begin()+size_t(b)*dim);}
  if(prepared_vectors&&prepared_table==t){c->vector_lease=std::move(prepared_vectors);prepared_vectors.reset();prepared_table.clear();
    const auto file=t+".v"+std::to_string(h.ivector);for(size_t page=0;page<c->vector_pages.size();++page)c->vector_pages[page]=storage.pool.handle(file,page);
  }
  // Encoded metadata can use the kernel copy if a later generic query needs it.
  // Never discard those kernel pages: future projection/DML can still read them.
  bool encoded=true;for(size_t i=0;i<cols.size();++i)if(cols[i].type==Type::Varchar)encoded&=c->dictionaries[i].supported;
  if(encoded){storage.pool.release_clean_file(t+".blob");for(size_t i=0;i<cols.size();++i)if(cols[i].type==Type::Varchar)storage.pool.release_clean_file(t+".s"+std::to_string(i));}
  read_cache=std::move(c);++cache_builds;return read_cache.get();
}
bool Engine::fast_topk(const Statement& s,const Header& h,const std::vector<Column>& cols,
 const std::vector<Expr>& expressions,const std::vector<size_t>& selected,
 const std::vector<std::vector<float>>& queries,const std::vector<std::pair<size_t,bool>>& order,
 const std::vector<std::pair<size_t,Value>>& predicates,Result& result){
  const auto pk=primary(cols);size_t distance=expressions.size();
  if(expressions.size()!=2||s.limit>4096||s.limit==0||order.empty()||order.size()>2||order[0].second)return false;
  for(size_t i=0;i<expressions.size();++i){if(expressions[i].distance){if(distance!=expressions.size())return false;distance=i;}else if(selected[i]!=pk)return false;}
  if(distance==expressions.size()||order[0].first!=distance)return false;
  if(order.size()==2){
    auto [position,descending]=order[1];
    bool primary_order=position==expressions.size()+pk||
      (position<expressions.size()&&!expressions[position].distance&&selected[position]==pk);
    if(descending||!primary_order)return false;
  }
  for(size_t j=0;j<predicates.size();++j)if(s.conditions[j].op!="="||(predicates[j].first!=pk&&cols[predicates[j].first].type!=Type::Varchar))return false;
  if(predicates.size()>64)return false;
  auto c=cache(s.table,h,cols);if(!c)return false;
  const bool reuse=c->heap.capacity()!=0;
  std::vector<std::pair<size_t,uint16_t>> local_filters;
  auto& filters=reuse?c->filters:local_filters;filters.clear();
  std::optional<int64_t> key_filter;bool impossible_key=false;
  for(auto& [i,v]:predicates)if(i==pk){auto key=std::get<int64_t>(v);impossible_key|=key_filter&&*key_filter!=key;key_filter=key;}
  for(auto& [i,v]:predicates)if(i!=pk){auto& d=c->dictionaries[i];if(!d.supported){read_cache.reset();return false;}auto it=std::find(d.values.begin(),d.values.end(),std::get<std::string>(v));if(it==d.values.end()){filters.emplace_back(i,UINT16_MAX);continue;}filters.emplace_back(i,static_cast<uint16_t>(it-d.values.begin()));}
  // Each value slice is in physical slot order. All remaining ANDs are still
  // checked by consume; the smallest slice is only a candidate source.
  const std::vector<SlotId>* slice=&c->active_slots;size_t slice_first=0,slice_last=slice->size();
  const char* dict_mode=std::getenv("MVDB_DICT_SLICES");
  if(!dict_mode||std::string_view(dict_mode)!="off")for(auto [i,id]:filters){
    auto& d=c->dictionaries[i];size_t first=id==UINT16_MAX?0:d.offsets[id];size_t last=id==UINT16_MAX?0:d.offsets[id+1];
    if(last-first<slice_last-slice_first){slice=&d.slot_ids;slice_first=first;slice_last=last;}
  }
  const size_t matching_bound=slice_last-slice_first;
  auto dim=cols[selected[distance]].width;const size_t vector_stride=stride(cols[selected[distance]]),count=PAGE/vector_stride;auto& query=queries[distance];bool safe=!has_extreme(query.data(),dim);
  bool ivf=ann&&h.nlist&&selected[distance]==h.ivector;route="exact";last_probes=0;
  if(ivf&&!nprobe&&(h.active_count<4096||stale(h)||h.calibration_skipped||!h.recommended_nprobe||h.recommended_nprobe>h.nlist/2)){ivf=false;route="exact_fallback";}
  if(ivf&&!filters.empty()&&matching_bound<=32){ivf=false;route="exact_fallback";}
  size_t workspace=size_t(s.limit)*(sizeof(Value)*2+sizeof(std::vector<Value>)+64)+4096;
  if(!reuse)workspace+=size_t(h.slots)*4+c->vector_pages.size()*sizeof(const std::byte*)+size_t(h.nlist)*sizeof(std::pair<double,uint32_t>)+size_t(s.limit)*sizeof(TableReadCache::Neighbor);
  if(workspace>storage.budget.limit-storage.budget.used){read_cache.reset();return false;}
  std::optional<Reservation> reservation;reservation.emplace(storage.budget,workspace);
  // Validate without loading: loading one missing page could evict any earlier
  // borrowed address. The all-resident scan below makes no pool calls at all.
  std::vector<const std::byte*> local_bases;
  auto& bases=reuse?c->bases:local_bases;bases.resize(c->vector_pages.size());bool all_resident=true;
  for(size_t i=0;i<bases.size();++i){bases[i]=storage.pool.resident(c->vector_pages[i]);all_resident&=bases[i]!=nullptr;}
  std::vector<SlotId> local_candidates;
  std::vector<std::pair<double,uint32_t>> local_distances;
  auto& candidates=reuse?c->candidates:local_candidates;candidates.clear();
  auto& distances=reuse?c->distances:local_distances;distances.clear();
  if(ivf){route="ivf";last_probes=std::min(nprobe?nprobe:h.recommended_nprobe,h.nlist);distances.reserve(h.nlist);
    for(uint32_t b=0;b<h.nlist;++b)distances.emplace_back(l2_squared_fast(query.data(),c->centers.data()+size_t(b)*dim,dim,safe&&!has_extreme(c->centers.data()+size_t(b)*dim,dim)),b);
    std::sort(distances.begin(),distances.end());
    // ANN need only the selected lists. Handles for never-probed lists can
    // remain cold even when every address this query uses is already resident.
    if(!all_resident){all_resident=true;for(size_t i=0;i<last_probes&&all_resident;++i){auto b=distances[i].second;for(auto at=c->offsets[b];at<c->offsets[b+1];++at)if(!bases[c->postings[at]/count]){all_resident=false;break;}}}
    if(!all_resident){candidates.reserve(h.slots);for(size_t i=0;i<last_probes;++i){auto b=distances[i].second;candidates.insert(candidates.end(),c->postings.begin()+c->offsets[b],c->postings.begin()+c->offsets[b+1]);}std::sort(candidates.begin(),candidates.end());}
  }
  using Neighbor=TableReadCache::Neighbor;std::vector<Neighbor> local_heap;auto& heap=reuse?c->heap:local_heap;heap.clear();heap.reserve(s.limit);
  auto less=[&](const Neighbor& a,const Neighbor& b){
    if(a.squared!=b.squared){
      // Preserve SQL's displayed double-distance tie order even when two
      // distinct squared sums round to the same square root.
      if(std::abs(a.squared-b.squared)<=4*std::numeric_limits<double>::epsilon()*std::max(a.squared,b.squared)){
        auto x=std::sqrt(a.squared),y=std::sqrt(b.squared);if(x!=y)return x<y;
      }else return a.squared<b.squared;
    }
    return c->keys[a.slot]<c->keys[b.slot];
  };
  const std::byte* vector_page=nullptr;uint64_t current=UINT64_MAX;auto file=s.table+".v"+std::to_string(selected[distance]);
  size_t prefix_trials=0,prefix_prunes=0,accepted_rows=0;
  const char* mode=std::getenv("MVDB_PROGRESSIVE");std::string_view policy=mode?mode:"auto";
  bool adaptive=policy=="auto"||policy=="retry"||policy=="strict";size_t prefix=policy=="64"?64:32;
  bool separated=ivf&&filters.empty()&&!distances.empty()&&distances.back().first>4*distances.front().first;
  bool progressive=dim==128&&all_resident&&safe&&policy!="off"&&(!ivf||separated);
  bool retry=(policy=="auto"||policy=="retry")&&progressive;double failed_threshold=0,bounded_cutoff=INFINITY;
  auto consume=[&]<bool Resident128,bool Bounded,bool CountAccepted>(SlotId slot,double computed=-1) __attribute__((always_inline)) {bool yes=true;for(auto [i,id]:filters)if(c->dictionaries[i].ids[slot]!=id){yes=false;break;}if(!yes)return;
    if(impossible_key||(key_filter&&c->keys[slot]!=*key_filter))return;
    if constexpr(CountAccepted)++accepted_rows;
    const float* p;
    if constexpr(Resident128)p=reinterpret_cast<const float*>(bases[slot>>5]+((slot&31U)<<9));
    else {auto page=slot/count;if(all_resident)vector_page=bases[page];else if(page!=current){
      auto& handle=c->vector_pages[page];vector_page=storage.pool.resident(handle);
      if(!vector_page){handle=storage.pool.handle(file,page);vector_page=storage.pool.resident(handle);}current=page;
    }
    // No pool operations occur while consuming this page. Handles validate
    // frame reuse on every page transition; no borrowed pointer crosses it.
    p=reinterpret_cast<const float*>(vector_page+(slot%count)*vector_stride);}
    if(heap.size()==s.limit){double fast;auto worst=heap.front().squared;
      if constexpr(Bounded){if(safe&&!(c->flags[slot]&2)){
        ++bounded_attempts;++prefix_trials;auto d=l2_squared_bounded_128(p,query.data(),bounded_cutoff,true,prefix);
        if(d.pruned){++bounded_pruned;++prefix_prunes;return;}fast=d.squared;
      }else fast=l2_squared_fast(p,query.data(),dim,false);}
      else fast=computed>=0?computed:l2_squared_fast(p,query.data(),dim,safe&&!(c->flags[slot]&2));
      auto error=std::max(1e-5,4.0*dim*std::numeric_limits<float>::epsilon());
      if(fast>worst+error*std::max({1.0,std::abs(fast),std::abs(worst)}))return;}
    Neighbor neighbor{l2_squared_exact(p,query.data(),dim),slot};
    bool changed=false;
    if(heap.size()<s.limit){heap.push_back(neighbor);std::push_heap(heap.begin(),heap.end(),less);changed=true;}else if(less(neighbor,heap.front())){std::pop_heap(heap.begin(),heap.end(),less);heap.back()=neighbor;std::push_heap(heap.begin(),heap.end(),less);changed=true;}
    if constexpr(Bounded)if(changed&&heap.size()==s.limit)bounded_cutoff=l2_bounded_cutoff(heap.front().squared);
  };
  // The probe window is bounded and is retried for each query. After a poor
  // window, the complete kernel runs without a per-row policy branch. A
  // fourfold tighter exact heap permits another window after a 256-slot chunk;
  // this recovers clustered scans whose first full heap was not yet selective.
  auto scan=[&]<bool Resident128,bool Prefetch,bool CountAccepted,bool Batch>(auto first,auto last){
    auto loop=[&]<bool Bounded>(auto begin,auto end){for(auto it=begin;it!=end;++it){
      if constexpr(Batch&&!Bounded){
        // Candidate order alone says nothing about vector adjacency: deleted
        // slots and ANN/dictionary postings may be scattered. Stay inside one
        // resident page and verify all four physical slots before sharing loads.
        auto slot=*it;
        if(safe&&heap.size()==s.limit&&end-it>=4&&(slot&31U)<=28&&
           *(it+1)==slot+1&&*(it+2)==slot+2&&*(it+3)==slot+3&&
           !((c->flags[slot]|c->flags[slot+1]|c->flags[slot+2]|c->flags[slot+3])&2)){
          double squared[4];
          l2_squared_batch4_128(reinterpret_cast<const float*>(bases[slot>>5]+((slot&31U)<<9)),query.data(),squared,true);
          // consume still rechecks each possible winner in double and updates
          // the heap in the original candidate order, including primary ties.
          for(size_t row=0;row<4;++row)consume.template operator()<true,false,CountAccepted>(slot+row,squared[row]);
          it+=3;continue;
        }
      }
#if defined(__GNUC__) || defined(__clang__)
      if constexpr(Prefetch){if(it+1!=end){auto next=*(it+1);__builtin_prefetch(bases[next>>5]+((next&31U)<<9),0,1);}}
#endif
      consume.template operator()<Resident128,Bounded,CountAccepted>(*it);
    }};
    if(!progressive&&!retry){loop.template operator()<false>(first,last);return;}
    auto it=first;
    while(it!=last){
      if(!progressive){
        auto end=it+std::min<decltype(last-it)>(256,last-it);loop.template operator()<false>(it,end);it=end;
        if(failed_threshold>0&&heap.size()==s.limit&&heap.front().squared<=failed_threshold*.25){progressive=true;prefix_trials=prefix_prunes=0;bounded_cutoff=l2_bounded_cutoff(heap.front().squared);}
        continue;
      }
      while(it!=last&&prefix_trials<64){consume.template operator()<Resident128,true,CountAccepted>(*it++);}
      if(adaptive&&prefix_trials>=64&&prefix_prunes<16){
        progressive=false;++bounded_fallbacks;failed_threshold=heap.front().squared;
        if(!retry){loop.template operator()<false>(it,last);return;}
      }else {loop.template operator()<true>(it,last);return;}
    }
  };
  auto scan_range=[&](auto first,auto last){
    auto dispatch=[&]<bool CountAccepted>(){
      if(all_resident&&dim==128){
        if(ivf)scan.template operator()<true,true,CountAccepted,false>(first,last);
        else if(filters.empty()&&!key_filter){
          // A failed initial 64-row window does not distinguish a uniform
          // workload from a clustered heap that has not tightened yet. Keep
          // the scalar/pruning loop for a bounded warm prefix before choosing
          // the contiguous quartet loop for a still-unselective remainder.
          auto split=first+std::min<decltype(last-first)>(1024,last-first);
          scan.template operator()<true,false,CountAccepted,false>(first,split);
          if(progressive)scan.template operator()<true,false,CountAccepted,false>(split,last);
          else scan.template operator()<true,false,CountAccepted,true>(split,last);
        }
        else scan.template operator()<true,false,CountAccepted,false>(first,last);
      }else scan.template operator()<false,false,CountAccepted,false>(first,last);
    };
    if(ivf&&!filters.empty())dispatch.template operator()<true>();else dispatch.template operator()<false>();
  };
  if(ivf&&all_resident){for(size_t i=0;i<last_probes;++i){auto b=distances[i].second;scan_range(c->postings.begin()+c->offsets[b],c->postings.begin()+c->offsets[b+1]);}}
  else if(ivf)scan_range(candidates.begin(),candidates.end());
  else scan_range(slice->begin()+slice_first,slice->begin()+slice_last);
  // Filter selectivity is not a recall certificate. Expand only a starved
  // filtered query, in center order, until K verified rows have been seen.
  const char* expand_mode=std::getenv("MVDB_FILTER_EXPAND");
  if(ivf&&!filters.empty()&&(!expand_mode||std::string_view(expand_mode)!="off")&&heap.size()<s.limit){
    const size_t quota=matching_bound<=256?std::min<size_t>(matching_bound,std::max<size_t>(s.limit,32)):s.limit;
    while(last_probes<h.nlist&&accepted_rows<quota){
      auto b=distances[last_probes++].second;auto first=c->postings.begin()+c->offsets[b],last=c->postings.begin()+c->offsets[b+1];
      // A cold added page may evict any cached address: validate the entire
      // slice before scanning, or use page-transition handles for this slice.
      bool resident=true;for(auto it=first;it!=last;++it){auto page=*it/count;bases[page]=storage.pool.resident(c->vector_pages[page]);resident&=bases[page]!=nullptr;}
      all_resident=resident;current=UINT64_MAX;
      scan_range(first,last);
    }
  }
  std::sort(heap.begin(),heap.end(),less);
  size_t retained=256;
  if(heap.size()<=result.numeric_rows.size()){
    result.numeric_count=heap.size();result.numeric_distance_column=distance;
    for(size_t i=0;i<heap.size();++i)result.numeric_rows[i]={c->keys[heap[i].slot],std::sqrt(heap[i].squared)};
  }else{
    result.memory_rows=std::make_shared<std::vector<std::vector<Value>>>();result.memory_rows->reserve(heap.size());
    for(auto& n:heap){std::vector<Value> values(2);values[distance]=std::sqrt(n.squared);values[1-distance]=c->keys[n.slot];result.memory_rows->push_back(std::move(values));}
    retained+=result.memory_rows->capacity()*(sizeof(std::vector<Value>)+2*sizeof(Value));
  }
  if(!reuse){std::vector<SlotId>().swap(local_candidates);std::vector<Neighbor>().swap(local_heap);std::vector<const std::byte*>().swap(local_bases);std::vector<std::pair<double,uint32_t>>().swap(local_distances);}
  reservation.reset();
  if(result.memory_rows)result.memory_reservation=std::make_shared<Reservation>(storage.budget,retained);
  return true;
}
bool Engine::prepare_select(std::string_view sql,Statement& s){
  if(storage.budget.limit<4*1024*1024)return false;
  if(s.kind!=Statement::Select||sql.size()>16384||s.limit==0||s.limit>10||s.expressions.size()!=2||s.conditions.size()>8||s.order.empty()||s.order.size()>2)return false;
  size_t distance=2;for(size_t i=0;i<2;++i)if(s.expressions[i].distance){if(distance!=2)return false;distance=i;}
  if(distance==2||s.expressions[1-distance].column=="*")return false;
  // Escaped literals retain the complete parser/converter route.
  Statement probe=s;if(!reuse_select(sql,sql,probe))return false;
  auto h=header(s.table);auto cols=schema(h);auto pk=primary(cols);
  auto vector_col=column(cols,s.expressions[distance].column);
  if(cols[vector_col].type!=Type::Vector||column(cols,s.expressions[1-distance].column)!=pk)return false;
  for(auto& condition:s.conditions){auto i=column(cols,condition.column);if(condition.op!="="||!condition.quoted||cols[i].type!=Type::Varchar)return false;}
  std::vector<std::pair<size_t,bool>> order;
  for(auto& [name,desc]:s.order){size_t i=0;for(;i<2;++i)if(s.expressions[i].alias==name)break;order.emplace_back(i<2?i:2+column(cols,name),desc);}
  if(order[0]!=std::pair<size_t,bool>{distance,false})return false;
  if(order.size()==2&&(order[1].second||(order[1].first!=1-distance&&order[1].first!=2+pk)))return false;
  const size_t bytes=98304+size_t(cols[vector_col].width)*8;
  if(bytes>(storage.budget.limit-storage.budget.used)/8)return false;
  auto w=std::make_unique<SelectWorkspace>();w->reservation=std::make_shared<Reservation>(storage.budget,bytes);
  w->statement=s;w->pattern=sql;w->header=h;w->columns=std::move(cols);w->order=std::move(order);
  w->selected.resize(2);w->selected[distance]=vector_col;w->selected[1-distance]=pk;w->queries.resize(2);
  w->query_value=std::vector<float>();std::get<std::vector<float>>(w->query_value).reserve(w->columns[vector_col].width);
  for(auto& c:s.conditions)w->predicates.emplace_back(column(w->columns,c.column),std::string());
  select_workspace=std::move(w);return true;
}
bool Engine::execute_select_workspace(Result& result){
  auto& w=*select_workspace;auto& s=w.statement;
  last_lists=w.header.nlist;last_recommended=w.header.recommended_nprobe;
  for(size_t i=0;i<2;++i){auto& e=s.expressions[i];result.columns.push_back(e.alias);
    if(e.distance){auto& v=std::get<std::vector<float>>(w.query_value);v.swap(w.queries[i]);
      convert_into(LiteralView{e.query,true,false},w.columns[w.selected[i]],w.query_value);v.swap(w.queries[i]);}
  }
  for(size_t j=0;j<w.predicates.size();++j){auto& c=s.conditions[j];auto type=w.columns[w.predicates[j].first];type.width=UINT32_MAX;
    convert_into(LiteralView{c.literal,c.quoted,false},type,w.predicates[j].second);}
  return fast_topk(s,w.header,w.columns,s.expressions,w.selected,w.queries,w.order,w.predicates,result);
}
Result Engine::execute(std::string_view sql){
  try {
  last_probes=last_lists=last_recommended=0;route="none";
  if(is_insert(sql)){
    select_workspace.reset();read_cache.reset();prepared_vectors.reset();prepared_table.clear();Reservation input(storage.budget,sql.size()+512);
    bool automatic=!storage.in_transaction();if(automatic)storage.begin();auto point=storage.savepoint();
    try{auto result=insert_stream(sql);if(automatic)storage.commit();return result;}
    catch(...){storage.rollback_to(point);if(automatic)storage.rollback();throw;}
  }
  if(sql.size()*4+512>storage.budget.limit-storage.budget.used){select_workspace.reset();read_cache.reset();prepared_vectors.reset();prepared_table.clear();}
  Reservation input(storage.budget,sql.size()*4+512);
  if(select_workspace&&sql.size()<=16384&&reuse_select(select_workspace->pattern,sql,select_workspace->statement)){
    ++reused_select_queries;
    Result result;if(execute_select_workspace(result))return result;
    return execute_inner(select_workspace->statement);
  }
  select_workspace.reset();auto s=parse(sql);
  if(prepare_select(sql,s)){Result result;if(execute_select_workspace(result))return result;select_workspace.reset();}
  // Full projections/sorts cannot use the bounded resident TopK path. Do not
  // let its preparation lease reserve frames across an unrelated scan.
  if(s.kind==Statement::Select&&(s.limit>4096||s.expressions.size()!=2||s.order.empty())){read_cache.reset();prepared_vectors.reset();prepared_table.clear();}
  if(s.kind!=Statement::Select&&s.kind!=Statement::Set){read_cache.reset();prepared_vectors.reset();prepared_table.clear();}
  if(s.kind==Statement::Begin){storage.begin();return {};}
  if(s.kind==Statement::Commit){storage.commit();return {};}
  if(s.kind==Statement::Rollback){storage.rollback();return {};}
  if(s.kind==Statement::Set){if(s.setting=="search_mode"){require(s.value=="exact"||s.value=="ann","invalid local search mode");ann=s.value=="ann";}else if(s.setting=="nprobe"){auto value=convert({s.value,false},{"",Type::Bigint});auto n=std::get<int64_t>(value);require(n>=0&&n<=64,"nprobe must be 0..64");nprobe=static_cast<uint32_t>(n);}else throw std::runtime_error("unknown local setting");return {};}
  bool writes=s.kind!=Statement::Select;bool automatic=writes&&!storage.in_transaction();if(automatic)storage.begin();auto point=storage.savepoint();
  preparation_cache_allowed=automatic&&s.kind==Statement::CreateIndex;
  try{auto result=execute_inner(s);if(automatic)storage.commit();preparation_cache_allowed=false;return result;}
  catch(...){preparation_cache_allowed=false;if(writes){storage.rollback_to(point);if(automatic)storage.rollback();}throw;}
  }catch(...){select_workspace.reset();read_cache.reset();prepared_vectors.reset();prepared_table.clear();throw;}
}
Result Engine::insert_stream(std::string_view sql){
  return insert_is_ascending(sql)?insert_sequential(sql):insert_ordered(sql);
}
bool Engine::insert_is_ascending(std::string_view sql){
  // Read-only preflight: never write a speculative prefix. Retain schema and
  // one previous key, not batch keys or borrowed literal views. A disorder
  // stops this scan early; the ordered helper validates the remaining SQL.
  struct Disorder {};
  std::vector<Column> cols;size_t position=0,pkcol=0;int64_t previous=0;bool have_previous=false;
  try{
    parse_insert_rows(sql,[&](const Statement& s){cols=schema(header(s.table));pkcol=primary(cols);position=pkcol;
      if(!s.insert_columns.empty()){
        require(s.insert_columns.size()==cols.size(),"all columns must have values");
        uint64_t seen=0;
        for(size_t i=0;i<s.insert_columns.size();++i){auto c=column(cols,s.insert_columns[i]);
          require(!(seen&(uint64_t(1)<<c)),"duplicate INSERT column");seen|=uint64_t(1)<<c;if(c==pkcol)position=i;
        }
      }
    },[&](const std::vector<LiteralView>& tuple){
      require(tuple.size()==cols.size(),"INSERT width mismatch");
      const auto key=std::get<int64_t>(convert(tuple[position],cols[pkcol]));
      if(have_previous&&key<=previous)throw Disorder{};
      previous=key;have_previous=true;
    },64);
  }catch(const Disorder&){return false;}
  return true;
}
Result Engine::insert_sequential(std::string_view sql){
  // Owned page images remain valid while PK/index writes replace pool frames.
  // Only new, header-invisible extents use append; bitmap bytes use normal undo.
  struct Pending {std::string file;uint64_t page=0;Page bytes{};size_t first=PAGE,last=0;bool append=true;};
  const size_t unit=sizeof(Pending)+128;
  size_t capacity=std::min<size_t>(8,(storage.budget.limit-storage.budget.used)/2/unit);
  Reservation pages_memory(storage.budget,capacity*unit);
  std::vector<Pending> pages;pages.reserve(capacity);
  auto flush=[&](Pending& p){if(p.append)storage.append_bytes(p.file,p.page,p.first,p.bytes.data()+p.first,p.last-p.first);else storage.write(p.file,p.page,p.bytes);};
  auto edit=[&](const std::string& file,uint64_t page,size_t offset,const void* data,size_t length,bool append){
    size_t i=0;for(;i<pages.size();++i)if(pages[i].file==file&&pages[i].page==page)break;
    if(i==pages.size()){
      if(pages.size()==capacity){flush(pages.front());pages.erase(pages.begin());}
      Pending p;p.file=file;p.page=page;p.bytes=storage.read(file,page);p.append=append;pages.push_back(std::move(p));i=pages.size()-1;
    }
    auto& p=pages[i];std::memcpy(p.bytes.data()+offset,data,length);p.first=std::min(p.first,offset);p.last=std::max(p.last,offset+length);
  };
  auto set_valid=[&](const std::string& file,SlotId slot){auto number=slot/(PAGE*8);size_t bit=slot%(PAGE*8);std::byte byte{};
    bool found=false;for(auto& p:pages)if(p.file==file&&p.page==number){byte=p.bytes[bit/8];found=true;break;}
    if(!found)byte=storage.read(file,number)[bit/8];
    byte|=std::byte(1U<<(bit%8));edit(file,number,bit/8,&byte,1,false);
  };
  Header h;std::string table;std::vector<Column> cols;std::vector<size_t> order;size_t pkcol=0;Result result;
  std::vector<Value> values;std::optional<Reservation> scratch_memory;size_t scratch_bytes=0;
  std::string packed;std::optional<Reservation> packed_memory;
  parse_insert_rows(sql,[&](const Statement& s){table=s.table;h=header(table);cols=schema(h);pkcol=primary(cols);
    if(s.insert_columns.empty()){for(size_t i=0;i<cols.size();++i)order.push_back(i);}else for(auto& name:s.insert_columns)order.push_back(column(cols,name));
    require(order.size()==cols.size(),"all columns must have values");auto sorted=order;std::sort(sorted.begin(),sorted.end());require(std::adjacent_find(sorted.begin(),sorted.end())==sorted.end(),"duplicate INSERT column");
    if(capacity){scratch_bytes=cols.size()*sizeof(Value);for(auto& c:cols)if(c.type==Type::Vector)scratch_bytes+=size_t(c.width)*4;
      scratch_memory.emplace(storage.budget,scratch_bytes);values.resize(cols.size());}
  },[&](const std::vector<LiteralView>& tuple){
    require(tuple.size()==cols.size(),"INSERT width mismatch");
    size_t bytes=512+cols.size()*(sizeof(Value)+sizeof(LiteralView));for(size_t i=0;i<tuple.size();++i){auto& col=cols[order[i]];bytes+=col.type==Type::Vector?size_t(col.width)*4+tuple[i].text.size():(col.type==Type::Varchar?tuple[i].text.size()*4:0);}
    // Optional retained output must never prevent admission of a wider row.
    if(packed_memory&&bytes-scratch_bytes+2*PAGE>storage.budget.limit-storage.budget.used){std::string().swap(packed);packed_memory.reset();}
    Reservation row_memory(storage.budget,bytes-scratch_bytes);
    if(values.size()!=cols.size())values.resize(cols.size());
    for(size_t j=0;j<tuple.size();++j)convert_into(tuple[j],cols[order[j]],values[order[j]]);
    if(!capacity){write_row(table,h.slots,std::move(values),cols,true);h=header(table);++result.affected_rows;return;}
    require(h.slots<UINT32_MAX,"SlotId exhausted");SlotId slot=h.slots;auto key=std::get<int64_t>(values[pkcol]);PrimaryIndex pk(storage,table+".pk");require(!pk.find(key),"duplicate primary key");
    Meta m;m.active=1;m.key=key;
    if(h.nlist){m.bucket=assign(table,h,std::get<std::vector<float>>(values[h.ivector]));auto base=index_base(table,h);if(h.index_region[0])edit(base+".buckets",slot/(PAGE/4),(slot%(PAGE/4))*4,&m.bucket,4,true);posting(base,m.bucket,slot,true);}
    for(size_t i=0;i<cols.size();++i){
      if(cols[i].type==Type::Vector){auto& v=std::get<std::vector<float>>(values[i]);m.reserved|=has_extreme(v.data(),v.size())?1:0;auto count=PAGE/stride(cols[i]);edit(table+".v"+std::to_string(i),slot/count,(slot%count)*stride(cols[i]),v.data(),v.size()*4,true);v.clear();}
      else if(!cols[i].primary){auto hash=scalar_hash(values[i]);edit(table+".s"+std::to_string(i),slot/(PAGE/8),(slot%(PAGE/8))*8,&hash,8,true);}
    }
    size_t packed_size=4;for(auto& value:values)packed_size+=std::holds_alternative<std::string>(value)?5+std::get<std::string>(value).size():(std::holds_alternative<std::vector<float>>(value)?5:9);
    // Growth uses the current row reservation, as the original temporary did.
    // Only a capacity that survives this row receives a separate reservation.
    if(packed_size>packed.capacity()){std::string().swap(packed);packed_memory.reset();}
    pack_values_into(values,packed);require(packed.size()<storage.budget.limit/4,"row exceeds workspace budget");m.offset=h.blob_end;m.length=static_cast<uint32_t>(packed.size());
    for(size_t at=0;at<packed.size();){auto offset=h.blob_end+at;auto n=std::min<size_t>(packed.size()-at,PAGE-offset%PAGE);edit(table+".blob",offset/PAGE,offset%PAGE,packed.data()+at,n,true);at+=n;}
    h.blob_end+=packed.size();edit(table+".rows",slot/(PAGE/sizeof(Meta)),(slot%(PAGE/sizeof(Meta)))*sizeof(Meta),&m,sizeof(m),true);set_valid(table+".valid",slot);
    pk.insert(key,slot);++h.slots;++h.active_count;if(h.nlist&&h.mutations_since_calibration<UINT64_MAX)++h.mutations_since_calibration;++result.affected_rows;
    for(size_t i=0;i<cols.size();++i)if(cols[i].type!=Type::Vector)values[i]=int64_t{};
    if(!packed_memory){if(storage.budget.limit>=4*1024*1024&&packed.capacity()+1+2*PAGE<=storage.budget.limit-storage.budget.used)packed_memory.emplace(storage.budget,packed.capacity()+1);else std::string().swap(packed);}
  },64);
  for(auto& p:pages)flush(p);
  if(capacity)storage.store(table+".header",0,h);
  return result;
}
Result Engine::insert_ordered(std::string_view sql){
  // Owned page images remain valid while PK/index writes replace pool frames.
  // Only new, header-invisible extents use append; bitmap bytes use normal undo.
  struct Pending {std::string file;uint64_t page=0;Page bytes{};size_t first=PAGE,last=0;bool append=true;};
  const size_t unit=sizeof(Pending)+128;
  size_t capacity=std::min<size_t>(8,(storage.budget.limit-storage.budget.used)/2/unit);
  Reservation pages_memory(storage.budget,capacity*unit);
  std::vector<Pending> pages;pages.reserve(capacity);
  auto flush=[&](Pending& p){if(p.append)storage.append_bytes(p.file,p.page,p.first,p.bytes.data()+p.first,p.last-p.first);else storage.write(p.file,p.page,p.bytes);};
  auto edit=[&](const std::string& file,uint64_t page,size_t offset,const void* data,size_t length,bool append){
    size_t i=0;for(;i<pages.size();++i)if(pages[i].file==file&&pages[i].page==page)break;
    if(i==pages.size()){
      if(pages.size()==capacity){flush(pages.front());pages.erase(pages.begin());}
      Pending p;p.file=file;p.page=page;p.bytes=storage.read(file,page);p.append=append;pages.push_back(std::move(p));i=pages.size()-1;
    }
    auto& p=pages[i];std::memcpy(p.bytes.data()+offset,data,length);p.first=std::min(p.first,offset);p.last=std::max(p.last,offset+length);
  };
  auto set_valid=[&](const std::string& file,SlotId slot){auto number=slot/(PAGE*8);size_t bit=slot%(PAGE*8);std::byte byte{};
    bool found=false;for(auto& p:pages)if(p.file==file&&p.page==number){byte=p.bytes[bit/8];found=true;break;}
    if(!found)byte=storage.read(file,number)[bit/8];
    byte|=std::byte(1U<<(bit%8));edit(file,number,bit/8,&byte,1,false);
  };
  Header h;std::string table;std::vector<Column> cols;std::vector<size_t> order;size_t pkcol=0;Result result;
  auto setup=[&](const Statement& s){table=s.table;h=header(table);cols=schema(h);pkcol=primary(cols);order.clear();
    if(s.insert_columns.empty()){for(size_t i=0;i<cols.size();++i)order.push_back(i);}else for(auto& name:s.insert_columns)order.push_back(column(cols,name));
    require(order.size()==cols.size(),"all columns must have values");auto sorted=order;std::sort(sorted.begin(),sorted.end());require(std::adjacent_find(sorted.begin(),sorted.end())==sorted.end(),"duplicate INSERT column");
  };
  // Only bounded keys and borrowed literal views are staged, never payloads.
  // Preinserted PK entries are invisible until rows/header publish and use the
  // ordinary statement savepoint, including failure in a later vector literal.
  constexpr size_t KEY_BATCH=128;
  std::array<std::pair<int64_t,uint32_t>,KEY_BATCH> keys{};
  constexpr size_t LITERAL_BATCH=512;
  std::vector<LiteralView> literals;bool literals_complete=false;
  std::optional<Reservation> literal_memory;
  size_t key_count=0;bool ordered_batch=false;
  const bool key_workspace=capacity&&sizeof(keys)+4096<storage.budget.limit-storage.budget.used;
  std::optional<Reservation> key_memory;
  if(key_workspace){key_memory.emplace(storage.budget,sizeof(keys));
    const size_t literal_bytes=LITERAL_BATCH*sizeof(LiteralView)+128;
    if(literal_bytes+4096<storage.budget.limit-storage.budget.used){literal_memory.emplace(storage.budget,literal_bytes);literals.reserve(LITERAL_BATCH);literals_complete=true;}
    parse_insert_rows(sql,setup,[&](const std::vector<LiteralView>& tuple){
      require(tuple.size()==cols.size(),"INSERT width mismatch");
      if(literals_complete){if(literals.size()+tuple.size()<=LITERAL_BATCH&&key_count<KEY_BATCH)literals.insert(literals.end(),tuple.begin(),tuple.end());else literals_complete=false;}
      if(key_count<KEY_BATCH){size_t position=0;while(order[position]!=pkcol)++position;
        keys[key_count]={std::get<int64_t>(convert(tuple[position],cols[pkcol])),static_cast<uint32_t>(key_count)};}
      ++key_count;
    },64);
    if(key_count>1&&key_count<=KEY_BATCH){
      require(key_count<=UINT32_MAX-h.slots,"SlotId exhausted");
      std::sort(keys.begin(),keys.begin()+key_count,[](const auto& a,const auto& b){return a.first<b.first;});
      PrimaryIndex pk(storage,table+".pk");
      for(size_t i=0;i<key_count;++i){require(!i||keys[i-1].first!=keys[i].first,"duplicate primary key");require(!pk.find(keys[i].first),"duplicate primary key");}
      for(size_t i=0;i<key_count;++i)pk.insert(keys[i].first,h.slots+keys[i].second);
      ordered_batch=true;++ordered_pk_batches;
    }
  }
  // The keys are no longer used once the ordered index edits have been logged.
  // Releasing optional workspace here preserves R9 admission of wide rows.
  key_memory.reset();
  if(literals_complete){size_t max_row=0;
    for(size_t row=0;row<key_count;++row){size_t bytes=512+cols.size()*(sizeof(Value)+sizeof(LiteralView));
      for(size_t i=0;i<cols.size();++i){const auto& v=literals[row*cols.size()+i];const auto& col=cols[order[i]];bytes+=col.type==Type::Vector?size_t(col.width)*4+v.text.size():(col.type==Type::Varchar?v.text.size()*4:0);}max_row=std::max(max_row,bytes);
    }
    if(max_row>storage.budget.limit-storage.budget.used)literals_complete=false;
  }
  if(!literals_complete){std::vector<LiteralView>().swap(literals);literal_memory.reset();}
  std::vector<Value> values;std::optional<Reservation> scratch_memory;size_t scratch_bytes=0;
  std::string packed;std::optional<Reservation> packed_memory;
  auto insert_tuple=[&](std::span<const LiteralView> tuple){
    require(tuple.size()==cols.size(),"INSERT width mismatch");
    size_t bytes=512+cols.size()*(sizeof(Value)+sizeof(LiteralView));for(size_t i=0;i<tuple.size();++i){auto& col=cols[order[i]];bytes+=col.type==Type::Vector?size_t(col.width)*4+tuple[i].text.size():(col.type==Type::Varchar?tuple[i].text.size()*4:0);}
    if(capacity&&!scratch_memory){scratch_bytes=cols.size()*sizeof(Value);for(auto& c:cols)if(c.type==Type::Vector)scratch_bytes+=size_t(c.width)*4;scratch_memory.emplace(storage.budget,scratch_bytes);}
    // Optional retained output must never prevent admission of a wider row.
    if(packed_memory&&bytes-scratch_bytes+2*PAGE>storage.budget.limit-storage.budget.used){std::string().swap(packed);packed_memory.reset();}
    Reservation row_memory(storage.budget,bytes-scratch_bytes);
    if(values.size()!=cols.size())values.resize(cols.size());
    for(size_t j=0;j<tuple.size();++j)convert_into(tuple[j],cols[order[j]],values[order[j]]);
    if(!capacity){write_row(table,h.slots,std::move(values),cols,true);h=header(table);++result.affected_rows;return;}
    require(h.slots<UINT32_MAX,"SlotId exhausted");SlotId slot=h.slots;auto key=std::get<int64_t>(values[pkcol]);PrimaryIndex pk(storage,table+".pk");if(!ordered_batch)require(!pk.find(key),"duplicate primary key");
    Meta m;m.active=1;m.key=key;
    if(h.nlist){m.bucket=assign(table,h,std::get<std::vector<float>>(values[h.ivector]));auto base=index_base(table,h);if(h.index_region[0])edit(base+".buckets",slot/(PAGE/4),(slot%(PAGE/4))*4,&m.bucket,4,true);posting(base,m.bucket,slot,true);}
    for(size_t i=0;i<cols.size();++i){
      if(cols[i].type==Type::Vector){auto& v=std::get<std::vector<float>>(values[i]);m.reserved|=has_extreme(v.data(),v.size())?1:0;auto count=PAGE/stride(cols[i]);edit(table+".v"+std::to_string(i),slot/count,(slot%count)*stride(cols[i]),v.data(),v.size()*4,true);v.clear();}
      else if(!cols[i].primary){auto hash=scalar_hash(values[i]);edit(table+".s"+std::to_string(i),slot/(PAGE/8),(slot%(PAGE/8))*8,&hash,8,true);}
    }
    size_t packed_size=4;for(auto& value:values)packed_size+=std::holds_alternative<std::string>(value)?5+std::get<std::string>(value).size():(std::holds_alternative<std::vector<float>>(value)?5:9);
    // Growth uses the current row reservation, as the original temporary did.
    // Only a capacity that survives this row receives a separate reservation.
    if(packed_size>packed.capacity()){std::string().swap(packed);packed_memory.reset();}
    pack_values_into(values,packed);require(packed.size()<storage.budget.limit/4,"row exceeds workspace budget");m.offset=h.blob_end;m.length=static_cast<uint32_t>(packed.size());
    for(size_t at=0;at<packed.size();){auto offset=h.blob_end+at;auto n=std::min<size_t>(packed.size()-at,PAGE-offset%PAGE);edit(table+".blob",offset/PAGE,offset%PAGE,packed.data()+at,n,true);at+=n;}
    h.blob_end+=packed.size();edit(table+".rows",slot/(PAGE/sizeof(Meta)),(slot%(PAGE/sizeof(Meta)))*sizeof(Meta),&m,sizeof(m),true);set_valid(table+".valid",slot);
    if(!ordered_batch)pk.insert(key,slot);
    ++h.slots;++h.active_count;if(h.nlist&&h.mutations_since_calibration<UINT64_MAX)++h.mutations_since_calibration;++result.affected_rows;
    for(size_t i=0;i<cols.size();++i)if(cols[i].type!=Type::Vector)values[i]=int64_t{};
    if(!packed_memory){if(storage.budget.limit>=4*1024*1024&&packed.capacity()+1+2*PAGE<=storage.budget.limit-storage.budget.used)packed_memory.emplace(storage.budget,packed.capacity()+1);else std::string().swap(packed);}
  };
  if(literals_complete){++borrowed_insert_batches;for(size_t row=0;row<key_count;++row)insert_tuple(std::span<const LiteralView>(literals.data()+row*cols.size(),cols.size()));}
  else parse_insert_rows(sql,setup,[&](const std::vector<LiteralView>& tuple){insert_tuple(tuple);},64);
  for(auto& p:pages)flush(p);
  if(capacity)storage.store(table+".header",0,h);
  return result;
}
Result Engine::execute_inner(const Statement& s){
  Result result;
  if(s.kind==Statement::Create){auto existing=storage.load<Header>(s.table+".header",0);require(existing.magic==0,"table already exists");require(!s.columns.empty()&&s.columns.size()<=64,"invalid column count");
    size_t keys=0,vectors=0;for(size_t i=0;i<s.columns.size();++i){auto& c=s.columns[i];if(c.primary){++keys;require(c.type==Type::Bigint,"primary key must be BIGINT");}if(c.type==Type::Vector){++vectors;require(c.width<=4096,"VECTOR dimension exceeds page capacity");}for(size_t j=0;j<i;++j)require(s.columns[j].name!=c.name,"duplicate column");}require(keys==1,"exactly one primary key required");require(vectors<=1,"at most one VECTOR column per table");
    std::string definition="CREATE TABLE "+s.table+" (";for(auto& c:s.columns){if(definition.back()!='(')definition+=',';definition+=c.name+' ';if(c.type==Type::Bigint)definition+="BIGINT";else definition+=(c.type==Type::Varchar?"VARCHAR(":"VECTOR(")+std::to_string(c.width)+")";if(c.primary)definition+=" PRIMARY KEY";}definition+=");";
    Header h;h.magic=0x4d564442;require(definition.size()<sizeof(h.schema),"schema too large");h.schema_size=static_cast<uint32_t>(definition.size());std::memcpy(h.schema,definition.data(),definition.size());storage.store(s.table+".header",0,h);return result;}
  auto h=header(s.table);last_lists=h.nlist;last_recommended=h.recommended_nprobe;auto cols=schema(h);auto keycol=primary(cols);
  if(s.kind==Statement::CreateIndex){create_index(s,cols);return result;}
  std::vector<std::pair<size_t,Value>> predicates;for(auto& c:s.conditions){auto i=column(cols,c.column);auto type=cols[i];if(type.type==Type::Varchar)type.width=UINT32_MAX;predicates.emplace_back(i,convert({c.literal,c.quoted},type));}
  auto matches=[&](const std::vector<Value>& values){for(size_t j=0;j<predicates.size();++j){auto& [i,v]=predicates[j];auto n=compare(values[i],v);auto op=s.conditions[j].op;bool yes=(op=="="?n==0:op=="!="||op=="<>"?n!=0:op=="<"?n<0:op==">"?n>0:op=="<="?n<=0:n>=0);if(!yes)return false;}return true;};
  // Only converted primary-key equalities establish a singleton candidate set.
  // Keep all predicates for exact verification, including contradictory ANDs.
  std::optional<int64_t> point_key;bool conflicting_key=false;
  for(size_t j=0;j<predicates.size();++j)if(predicates[j].first==keycol&&s.conditions[j].op=="="){
    auto key=std::get<int64_t>(predicates[j].second);
    if(point_key&&*point_key!=key)conflicting_key=true;
    else point_key=key;
  }
  auto point_slot=[&]()->std::optional<SlotId>{
    if(conflicting_key)return {};
    ++point_lookups;
    auto slot=PrimaryIndex(storage,s.table+".pk").find(*point_key);
    if(!slot||*slot>=h.slots)return {};
    ++bitmap_blocks;
    auto valid=storage.pool.view(s.table+".valid",*slot/(PAGE*8));
    if(!(std::to_integer<uint8_t>(valid[(*slot%(PAGE*8))/8])&(uint8_t(1)<<(*slot%8))))return {};
    ++candidate_slots;
    auto m=meta(s.table,*slot);
    if(!m.active||m.key!=*point_key)return {};
    return slot;
  };
  std::vector<std::pair<size_t,uint64_t>> equality_sketches;
  for(size_t j=0;j<predicates.size();++j)if(s.conditions[j].op=="="&&predicates[j].first!=keycol&&cols[predicates[j].first].type!=Type::Vector)equality_sketches.emplace_back(predicates[j].first,scalar_hash(predicates[j].second));
  auto signature_filter=[&](uint64_t start,std::array<uint64_t,64>& candidates){
    const auto end=std::min<uint64_t>(4096,h.slots-start);
    for(auto [c,hash]:equality_sketches){auto f=s.table+".s"+std::to_string(c);
      for(size_t part=0;part<end;part+=PAGE/8){
        auto limit=std::min<size_t>(part+PAGE/8,end);
        bool any=false;for(size_t wi=part/64;wi<(limit+63)/64;++wi)any|=candidates[wi]!=0;if(!any)continue;
        const auto* sketch=storage.pool.view(f,(start+part)/(PAGE/8));
        each_set_bit(candidates,part,limit,[&](size_t at){
          uint64_t stored;std::memcpy(&stored,sketch+((start+at)%(PAGE/8))*8,8);
          if(stored!=hash){candidates[at/64]&=~(uint64_t(1)<<(at%64));++signature_rejections;}
        });
      }
    }
  };
  if(s.kind==Statement::Insert){std::vector<size_t> order;if(s.insert_columns.empty()){for(size_t i=0;i<cols.size();++i)order.push_back(i);}else for(auto& name:s.insert_columns)order.push_back(column(cols,name));
    require(order.size()==cols.size(),"all columns must have values");auto sorted=order;std::sort(sorted.begin(),sorted.end());require(std::adjacent_find(sorted.begin(),sorted.end())==sorted.end(),"duplicate INSERT column");
    for(auto& tuple:s.rows){require(tuple.size()==cols.size(),"INSERT width mismatch");std::vector<Value> values(cols.size());for(size_t j=0;j<tuple.size();++j)values[order[j]]=convert(tuple[j],cols[order[j]]);auto slot=header(s.table).slots;write_row(s.table,slot,std::move(values),cols,true);++result.affected_rows;}return result;}
  if(s.kind==Statement::Update||s.kind==Statement::Delete){std::vector<std::pair<size_t,Value>> changes;for(auto& [name,v]:s.assignments){auto i=column(cols,name);changes.emplace_back(i,convert(v,cols[i]));}
    if(point_key){
      if(auto slot=point_slot()){
        auto m=meta(s.table,*slot);auto values=row(s.table,*slot,m,cols,s.kind==Statement::Update);
        if(matches(values)){for(auto& [i,v]:changes)values[i]=v;
          write_row(s.table,*slot,std::move(values),cols,s.kind==Statement::Update);++result.affected_rows;}
      }
      return result;
    }
    for(uint64_t start=0;start<h.slots;start+=4096){std::array<uint64_t,64> candidates{};
      ++bitmap_blocks;
      auto valid=storage.pool.view(s.table+".valid",start/(PAGE*8));std::memcpy(candidates.data(),valid+(start%(PAGE*8))/8,sizeof(candidates));
      signature_filter(start,candidates);auto end=std::min<uint64_t>(4096,h.slots-start);
      each_set_bit(candidates,0,end,[&](size_t at){
        ++candidate_slots;auto slot=static_cast<SlotId>(start+at);auto m=meta(s.table,slot);if(!m.active)return;
        auto values=row(s.table,slot,m,cols,s.kind==Statement::Update);if(!matches(values))return;
        for(auto& [i,v]:changes)values[i]=v;
        write_row(s.table,slot,std::move(values),cols,s.kind==Statement::Update);++result.affected_rows;
      });
    }return result;}
  require(s.kind==Statement::Select,"invalid execution kind");
  std::vector<Expr> expressions;for(auto& e:s.expressions){if(e.column=="*")for(auto& c:cols)expressions.push_back({c.name,c.name,false,{}});else expressions.push_back(e);}
  std::vector<size_t> selected;std::vector<std::vector<float>> queries;
  for(auto& e:expressions){auto i=column(cols,e.column);selected.push_back(i);result.columns.push_back(e.alias);if(e.distance){require(cols[i].type==Type::Vector,"distance requires VECTOR");queries.push_back(std::get<std::vector<float>>(convert({e.query,true},cols[i])));}else queries.emplace_back();}
  std::vector<std::pair<size_t,bool>> order;
  for(auto& [name,desc]:s.order){size_t i=0;for(;i<expressions.size();++i)if(expressions[i].alias==name)break;if(i<expressions.size())order.emplace_back(i,desc);else order.emplace_back(expressions.size()+column(cols,name),desc);}
  if(point_key){
    read_cache.reset();last_probes=0;route="primary_point";
    result.memory_rows=std::make_shared<std::vector<std::vector<Value>>>();
    if(s.limit==0)return result;
    bool need_vectors=false;
    for(size_t i=0;i<expressions.size();++i)need_vectors|=expressions[i].distance||cols[selected[i]].type==Type::Vector;
    if(auto slot=point_slot()){
      auto m=meta(s.table,*slot);auto values=row(s.table,*slot,m,cols,need_vectors);
      if(matches(values)){
        std::vector<Value> output;
        for(size_t i=0;i<expressions.size();++i){auto c=selected[i];
          if(expressions[i].distance){auto& v=std::get<std::vector<float>>(values[c]);output.emplace_back(l2_exact(v.data(),queries[i].data(),v.size()));}
          else output.push_back(values[c]);
        }
        size_t retained=sizeof(std::vector<Value>)+output.size()*sizeof(Value)+pack_values(output).size()+256;
        result.memory_reservation=std::make_shared<Reservation>(storage.budget,retained);
        result.memory_rows->push_back(std::move(output));
      }
    }
    return result;
  }
  if(fast_topk(s,h,cols,expressions,selected,queries,order,predicates,result))return result;
  read_cache.reset();prepared_vectors.reset();prepared_table.clear();
  const size_t projected=expressions.size(),pkpos=projected+cols.size();
  auto less=[&](const std::vector<Value>& a,const std::vector<Value>& b){for(auto [i,desc]:order){int n=compare(a[i],b[i]);if(n)return desc?n>0:n<0;}return compare(a[pkpos],b[pkpos])<0;};
  bool need_values=false;
  size_t retained=(projected+cols.size()+1)*(sizeof(Value)+16)+256;
  for(size_t i=0;i<selected.size();++i){auto c=selected[i];if(!expressions[i].distance&&c!=keycol){need_values=true;if(cols[c].type==Type::Varchar)retained+=cols[c].width*4;else if(cols[c].type==Type::Vector)retained+=cols[c].width*4;}}
  for(auto [i,desc]:order){(void)desc;if(i>=projected&&i-projected!=keycol)need_values=true;}
  if(need_values)for(auto& c:cols)if(c.type==Type::Varchar)retained+=c.width*4;
  bool use_heap=s.limit<=4096&&s.limit<=((storage.budget.limit-storage.budget.used)*3/4)/retained;
  std::shared_ptr<Reservation> heap_reservation;
  std::vector<std::vector<Value>> heap;
  std::unique_ptr<Sorter> sorter;
  if(use_heap){heap_reservation=std::make_shared<Reservation>(storage.budget,retained*s.limit);heap.reserve(s.limit);}
  else sorter=std::make_unique<Sorter>(storage,less);
  std::vector<uint32_t> buckets;size_t distance_expr=0;bool found=false;for(size_t i=0;i<expressions.size();++i)if(expressions[i].distance&&selected[i]==h.ivector){distance_expr=i;found=true;break;}
  bool use_ivf=ann&&h.nlist&&found&&!order.empty()&&order[0].first==distance_expr&&!order[0].second&&s.limit!=UINT64_MAX;
  last_probes=0;route="exact";
  if(use_ivf&&!nprobe&&(h.active_count<4096||stale(h)||h.calibration_skipped||!h.recommended_nprobe||h.recommended_nprobe>h.nlist/2)){use_ivf=false;route="exact_fallback";}
  std::vector<std::pair<double,uint32_t>> distances;
  if(use_ivf){route="ivf";last_probes=std::min(nprobe?nprobe:h.recommended_nprobe,h.nlist);for(uint32_t b=0;b<h.nlist;++b){auto p=center_view(index_base(s.table,h),b,queries[distance_expr].size());distances.emplace_back(l2(queries[distance_expr].data(),p,queries[distance_expr].size()),b);}std::sort(distances.begin(),distances.end());for(size_t i=0;i<distances.size();++i)buckets.push_back(distances[i].second);}
  if(s.limit==0)return result;
  std::vector<bool> query_safe;for(auto& q:queries)query_safe.push_back(!has_extreme(q.data(),q.size()));
  size_t preferred=cols.size();
  for(auto c:selected)if(cols[c].type==Type::Vector){preferred=c;break;}
  if(use_ivf)preferred=h.ivector;
  BufferPool::PageGuard lease;
  uint64_t leased_page=UINT64_MAX;
  auto payload=[&](SlotId slot,size_t c){
    if(c!=preferred)return vector_view(s.table,slot,c,cols[c]);
    auto count=PAGE/stride(cols[c]);auto page=slot/count;
    if(leased_page!=page){lease.reset();lease=storage.pool.lease(s.table+".v"+std::to_string(c),page);leased_page=page;}
    return reinterpret_cast<const float*>(lease.data+(slot%count)*stride(cols[c]));
  };
  uint64_t accepted_rows=0;uint32_t first_bucket=0,scan_end=last_probes;
  const char* expand_mode=std::getenv("MVDB_FILTER_EXPAND");
  do {
  for(uint64_t start=0;start<h.slots;start+=4096){std::array<uint64_t,64> candidates{};
    if(use_ivf){for(uint32_t bi=first_bucket;bi<scan_end;++bi){auto b=buckets[bi];auto list=posting_list(index_base(s.table,h),b);uint64_t loaded=UINT64_MAX;Page page{};for(uint32_t i=0;i<list.count;++i){uint64_t offset=list.offset+uint64_t(i)*4;if(loaded!=offset/PAGE){loaded=offset/PAGE;page=storage.read(list.file,loaded);}uint32_t slot;std::memcpy(&slot,page.data()+offset%PAGE,4);if(slot>=start&&slot<start+4096)candidates[(slot-start)/64]|=uint64_t(1)<<((slot-start)%64);}}}
    else candidates.fill(UINT64_MAX);
    std::array<uint64_t,64> visible{};
    ++bitmap_blocks;
    auto valid=storage.pool.view(s.table+".valid",start/(PAGE*8));
    std::memcpy(visible.data(),valid+(start%(PAGE*8))/8,sizeof(visible));
    for(size_t word=0;word<64;++word)candidates[word]&=visible[word];
    signature_filter(start,candidates);
    // Finish the bitmap intersection before touching vector payload pages.
    auto end=std::min<uint64_t>(4096,h.slots-start);
    each_set_bit(candidates,0,end,[&](size_t at){auto slot=static_cast<SlotId>(start+at);auto mask=uint64_t(1)<<(at%64);auto& word=candidates[at/64];
      ++candidate_slots;bool yes=true;
      if(yes&&!predicates.empty()){auto m=meta(s.table,slot);yes=m.active&&matches(row(s.table,slot,m,cols,false));}if(!yes)word&=~mask;
    });
    each_set_bit(candidates,0,end,[&](size_t at){auto slot=static_cast<SlotId>(start+at);
      ++accepted_rows;
      auto m=meta(s.table,slot);
      // SIMD rejects clearly losing candidates. Heap entries and potential
      // winners use float64 recomputation, preserving strict exact tie order.
      if(use_heap&&heap.size()==s.limit&&!order.empty()&&order[0].first<projected&&expressions[order[0].first].distance){
        auto i=order[0].first,c=selected[i];auto p=payload(slot,c);auto d=l2_fast(p,queries[i].data(),cols[c].width,m.reserved==0&&query_safe[i]);
        auto worst=std::get<double>(heap.front()[i]);auto margin=1e-5*std::max({1.0,std::abs(d),std::abs(worst)});
        if((!order[0].second&&d>worst+margin)||(order[0].second&&d<worst-margin))return;
      }
      auto values=need_values?row(s.table,slot,m,cols,false):std::vector<Value>(cols.size());values[keycol]=m.key;
      std::vector<Value> output;for(size_t i=0;i<expressions.size();++i){auto c=selected[i];if(expressions[i].distance){auto p=payload(slot,c);output.emplace_back(l2_exact(p,queries[i].data(),cols[c].width));}
        else if(cols[c].type==Type::Vector){auto p=payload(slot,c);output.emplace_back(std::vector<float>(p,p+cols[c].width));}else output.push_back(values[c]);}
      output.insert(output.end(),values.begin(),values.end());output.emplace_back(m.key);
      if(use_heap){if(heap.size()<s.limit){heap.push_back(std::move(output));std::push_heap(heap.begin(),heap.end(),less);}else if(less(output,heap.front())){std::pop_heap(heap.begin(),heap.end(),less);heap.back()=std::move(output);std::push_heap(heap.begin(),heap.end(),less);}}
      else sorter->add(std::move(output));
    });
  }
    if(!use_ivf||predicates.empty()||accepted_rows>=s.limit||scan_end>=h.nlist||(expand_mode&&std::string_view(expand_mode)=="off"))break;
    first_bucket=scan_end;scan_end=++last_probes;
    lease.reset();leased_page=UINT64_MAX;
  } while(true);
  if(use_heap){std::sort(heap.begin(),heap.end(),less);for(auto& r:heap)r.resize(projected);result.memory_rows=std::make_shared<std::vector<std::vector<Value>>>(std::move(heap));result.memory_reservation=std::move(heap_reservation);return result;}
  auto sorted=sorter->finish();result.rows=storage.temporary();std::vector<Value> values;uint64_t returned=0;
  while(returned<s.limit&&spill_read(sorted.get(),values)){values.resize(projected);spill_write(result.rows.get(),values);++returned;}
  discard_cache(sorted.get());discard_cache(result.rows.get());return result;
}
}
