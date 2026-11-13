#include "simd.hpp"
#include "engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <unistd.h>
namespace mvdb {
namespace {
uint64_t total_spill_bytes=0;
void require(bool yes,const std::string& s){if(!yes)throw std::runtime_error(s);}
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
constexpr off_t SPILL_CACHE_CHUNK=4*1024*1024;
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
void Result::each(const std::function<void(const std::vector<Value>&)>& callback) const {if(memory_rows){for(auto& r:*memory_rows)callback(r);return;}if(!rows)return;std::rewind(rows.get());std::vector<Value> r;while(spill_read(rows.get(),r))callback(r);discard_cache(rows.get());}
uint64_t Engine::spill_bytes() const{return total_spill_bytes;}
Engine::Header Engine::header(const std::string& t){auto h=storage.load<Header>(t+".header",0);require(h.magic==0x4d564442,"table does not exist: "+t);return h;}
std::vector<Column> Engine::schema(const Header& h){require(h.schema_size<sizeof(h.schema),"corrupt schema");return parse(std::string(h.schema,h.schema_size)).columns;}
Engine::Meta Engine::meta(const std::string& t,SlotId s){auto p=storage.pool.view(t+".rows",s/(PAGE/sizeof(Meta)));Meta m;std::memcpy(&m,p+(s%(PAGE/sizeof(Meta)))*sizeof(Meta),sizeof(m));return m;}
void Engine::put_meta(const std::string& t,SlotId s,const Meta& m){auto id=s/(PAGE/sizeof(Meta));auto p=storage.read(t+".rows",id);std::memcpy(p.data()+(s%(PAGE/sizeof(Meta)))*sizeof(Meta),&m,sizeof(m));storage.write(t+".rows",id,p);}
std::string Engine::read_blob(const std::string& t,uint64_t offset,uint32_t n){std::string out;out.reserve(n);while(n){auto page=storage.pool.view(t+".blob",offset/PAGE);auto count=std::min<uint64_t>(n,PAGE-offset%PAGE);out.append(reinterpret_cast<const char*>(page+offset%PAGE),count);offset+=count;n-=count;}return out;}
void Engine::write_blob(const std::string& t,uint64_t offset,const std::string& s,bool append){size_t pos=0;while(pos<s.size()){auto p=offset/PAGE;auto page=storage.read(t+".blob",p);auto n=std::min<uint64_t>(s.size()-pos,PAGE-offset%PAGE);std::memcpy(page.data()+offset%PAGE,s.data()+pos,n);if(append)storage.append(t+".blob",p,page,offset%PAGE,n);else storage.write(t+".blob",p,page);offset+=n;pos+=n;}}
const float* Engine::vector_view(const std::string& t,SlotId s,size_t i,const Column& c){auto count=PAGE/stride(c);auto p=storage.pool.view(t+".v"+std::to_string(i),s/count);return reinterpret_cast<const float*>(p+(s%count)*stride(c));}
const float* Engine::center_view(const std::string& t,uint32_t b,size_t dim){auto bytes=((dim+7)/8)*32;auto count=PAGE/bytes;auto p=storage.pool.view(t+".centers",b/count);return reinterpret_cast<const float*>(p+(b%count)*bytes);}
std::vector<Value> Engine::row(const std::string& t,SlotId s,const Meta& m,const std::vector<Column>& cols,bool vectors){auto r=unpack_values(read_blob(t,m.offset,m.length));require(r.size()==cols.size(),"corrupt row width");if(vectors)for(size_t i=0;i<cols.size();++i)if(cols[i].type==Type::Vector){auto p=vector_view(t,s,i,cols[i]);r[i]=std::vector<float>(p,p+cols[i].width);}return r;}
void Engine::bitmap(const std::string& t,SlotId slot,bool active){auto p=slot/(PAGE*8);auto page=storage.read(t+".valid",p);auto bit=slot%(PAGE*8);auto x=std::to_integer<unsigned char>(page[bit/8]);x=active?x|(1U<<(bit%8)):x&~(1U<<(bit%8));page[bit/8]=std::byte(x);storage.write(t+".valid",p,page);}
void Engine::posting(const std::string& t,uint32_t bucket,SlotId slot,bool add){
  auto f=t+".ivf"+std::to_string(bucket);auto h=storage.read(f,0);uint32_t n;std::memcpy(&n,h.data(),4);
  auto get=[&](uint32_t i){uint64_t offset=16+uint64_t(i)*4;auto p=storage.read(f,offset/PAGE);uint32_t x;std::memcpy(&x,p.data()+offset%PAGE,4);return x;};
  auto put=[&](uint32_t i,uint32_t x){uint64_t offset=16+uint64_t(i)*4;auto p=storage.read(f,offset/PAGE);std::memcpy(p.data()+offset%PAGE,&x,4);storage.write(f,offset/PAGE,p);};
  if(add){require(n<UINT32_MAX,"IVF list full");put(n++,slot);}else{for(uint32_t i=0;i<n;++i)if(get(i)==slot){auto last=get(n-1);--n;if(i<n)put(i,last);break;}}
  // Slot writes can modify page zero; reload before publishing its count.
  h=storage.read(f,0);std::memcpy(h.data(),&n,4);storage.write(f,0,h);
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
  auto region=storage.begin_region(s.table+".index");require(region.size()<sizeof(h.index_region),"index namespace too long");
  std::memcpy(h.index_region,region.c_str(),region.size()+1);h.active_count=count;
  auto dim=cols[col].width;
  {
  size_t full=h.nlist*dim*(sizeof(double)+sizeof(float))+1024;
  bool narrow=full>storage.budget.limit-storage.budget.used;
  size_t training_bytes=narrow?h.nlist*dim*sizeof(float)*2+dim*sizeof(double)+1024:full;
  Reservation reserve(storage.budget,training_bytes);
  std::vector<std::vector<float>> centers;uint32_t ordinal=0,next=0;
  for(SlotId slot=0;slot<h.slots&&centers.size()<h.nlist;++slot)if(meta(s.table,slot).active){if(ordinal==next){auto p=vector_view(s.table,slot,col,cols[col]);centers.emplace_back(p,p+dim);next=static_cast<uint32_t>(centers.size()*count/h.nlist);}++ordinal;}
  for(int iteration=0;iteration<3;++iteration){
    std::vector<bool> safe;for(auto& center:centers)safe.push_back(!has_extreme(center.data(),dim));
    auto nearest=[&](const float* p,bool normal){uint32_t best=0;double distance=INFINITY;for(uint32_t b=0;b<h.nlist;++b){auto d=l2_fast(p,centers[b].data(),dim,normal&&safe[b]);if(d<distance){distance=d;best=b;}}return best;};
    if(narrow){
      auto next=centers;
      for(uint32_t b=0;b<h.nlist;++b){std::vector<double> sum(dim);uint32_t count_in_bucket=0;
        for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(s.table,slot);if(!m.active)continue;auto p=vector_view(s.table,slot,col,cols[col]);if(nearest(p,m.reserved==0)!=b)continue;++count_in_bucket;for(size_t j=0;j<dim;++j)sum[j]+=p[j];}
        if(count_in_bucket)for(size_t j=0;j<dim;++j)next[b][j]=static_cast<float>(sum[j]/count_in_bucket);
      }centers.swap(next);
    }else{
      std::vector<std::vector<double>> sums(h.nlist,std::vector<double>(dim));std::vector<uint32_t> sizes(h.nlist);
      for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(s.table,slot);if(!m.active)continue;auto p=vector_view(s.table,slot,col,cols[col]);auto best=nearest(p,m.reserved==0);++sizes[best];for(size_t j=0;j<dim;++j)sums[best][j]+=p[j];}
      for(uint32_t b=0;b<h.nlist;++b)if(sizes[b])for(size_t j=0;j<dim;++j)centers[b][j]=static_cast<float>(sums[b][j]/sizes[b]);
    }
  }
  for(uint32_t b=0;b<h.nlist;++b){auto count=PAGE/stride(cols[col]);auto page=storage.read(region+".centers",b/count);std::memcpy(page.data()+(b%count)*stride(cols[col]),centers[b].data(),dim*4);storage.write_new(region+".centers",b/count,page);}
  std::vector<bool> safe;for(auto& center:centers)safe.push_back(!has_extreme(center.data(),dim));
  for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(s.table,slot);if(!m.active)continue;auto p=vector_view(s.table,slot,col,cols[col]);uint32_t b=0;double nearest=INFINITY;
    for(uint32_t candidate=0;candidate<h.nlist;++candidate){auto distance=l2_fast(p,centers[candidate].data(),dim,m.reserved==0&&safe[candidate]);if(distance<nearest){nearest=distance;b=candidate;}}
    put_bucket(s.table,h,slot,b);posting(region,b,slot,true);}
  }
  h.calibration_skipped=detail::skip_ivf_calibration_for_payload(count,uint64_t(count)*dim*sizeof(float),storage.budget.limit);
  h.calibration_queries=h.calibration_skipped?0:16;
  h.recommended_nprobe=h.calibration_skipped?std::min(h.nlist,std::max<uint32_t>(4,h.nlist/4)):calibrate(s.table,h,cols);h.calibration_stale=0;
  h.mutations_since_calibration=0;h.rows_at_calibration=count;
  last_lists=h.nlist;last_recommended=h.recommended_nprobe;
  // Publish only after all fresh-region pages and calibration are complete.
  storage.store(s.table+".header",0,h);
  storage.seal_region(region);
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
Engine::TableReadCache* Engine::cache(const std::string& t,const Header& h,const std::vector<Column>& cols){
  if(read_cache&&read_cache->table==t)return read_cache.get();
  read_cache.reset();
  if(h.slots>25000)return nullptr;
  size_t varchar_count=0;for(auto& c:cols)varchar_count+=c.type==Type::Varchar;
  size_t center_count=h.nlist?size_t(h.nlist)*cols[h.ivector].width:0;
  size_t vector_pages=0;for(auto& col:cols)if(col.type==Type::Vector){auto count=PAGE/stride(col);vector_pages=(h.slots+count-1)/count;}
  // Charge fixed capacities, including dictionary strings and container metadata.
  // High cardinality columns stop after 128 distinct values or 16 KiB UTF-8.
  size_t bytes=4096+size_t(h.slots)*17+center_count*4+size_t(h.nlist+2)*4+vector_pages*sizeof(BufferPool::ResidentHandle)+
    cols.size()*sizeof(TableReadCache::Dictionary)+varchar_count*(size_t(h.slots)*6+32768+129*sizeof(uint32_t));
  if(bytes>(storage.budget.limit-storage.budget.used)/2)return nullptr;
  auto c=std::make_unique<TableReadCache>();c->reservation=std::make_shared<Reservation>(storage.budget,bytes);
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
  for(size_t i=1;i<c->offsets.size();++i)c->offsets[i]+=c->offsets[i-1];
  c->postings.resize(c->offsets.back());auto next=c->offsets;
  for(SlotId slot=0;slot<h.slots;++slot)if(c->flags[slot]&1)c->postings[next[h.nlist?bucket(t,h,slot):0]++]=slot;
  // Each dictionary range contains active slots in storage order. Flat ranges
  // avoid per-value allocations and allow page-ordered scans after a cache miss.
  for(auto& d:c->dictionaries)if(d.supported){
    d.offsets.resize(d.values.size()+1);
    for(auto slot:c->active_slots)++d.offsets[d.ids[slot]+1];
    for(size_t i=1;i<d.offsets.size();++i)d.offsets[i]+=d.offsets[i-1];
    d.postings.resize(c->active_slots.size());auto next=d.offsets;
    for(auto slot:c->active_slots)d.postings[next[d.ids[slot]]++]=slot;
  }
  c->centers.resize(center_count);
  for(uint32_t b=0;b<h.nlist;++b){auto dim=cols[h.ivector].width;auto p=center_view(index_base(t,h),b,dim);std::copy(p,p+dim,c->centers.begin()+size_t(b)*dim);}
  read_cache=std::move(c);return read_cache.get();
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
  auto c=cache(s.table,h,cols);if(!c)return false;
  std::vector<std::pair<size_t,uint16_t>> filters;
  std::optional<int64_t> key_filter;bool impossible_key=false;
  for(auto& [i,v]:predicates)if(i==pk){auto key=std::get<int64_t>(v);impossible_key|=key_filter&&*key_filter!=key;key_filter=key;}
  for(auto& [i,v]:predicates)if(i!=pk){auto& d=c->dictionaries[i];if(!d.supported){read_cache.reset();return false;}auto it=std::find(d.values.begin(),d.values.end(),std::get<std::string>(v));if(it==d.values.end()){filters.emplace_back(i,UINT16_MAX);continue;}filters.emplace_back(i,static_cast<uint16_t>(it-d.values.begin()));}
  auto dim=cols[selected[distance]].width;const size_t vector_stride=stride(cols[selected[distance]]),count=PAGE/vector_stride;auto& query=queries[distance];bool safe=!has_extreme(query.data(),dim);
  bool ivf=ann&&h.nlist&&selected[distance]==h.ivector;route="exact";last_probes=0;
  if(ivf&&!nprobe&&(h.active_count<4096||stale(h)||h.calibration_skipped||!h.recommended_nprobe||h.recommended_nprobe>h.nlist/2)){ivf=false;route="exact_fallback";}
  const TableReadCache::Dictionary* subset=nullptr;uint32_t subset_begin=0,subset_end=0;
  if(predicates.size()==1&&filters.size()==1){
    auto [column,id]=filters.front();auto& d=c->dictionaries[column];
    if(id!=UINT16_MAX){subset_begin=d.offsets[id];subset_end=d.offsets[id+1];}
    const auto probes=std::min(nprobe?nprobe:h.recommended_nprobe,h.nlist);
    const auto threshold=std::max(uint64_t(64),h.nlist?uint64_t(probes)*h.active_count/(uint64_t(h.nlist)*4):0);
    if(!ivf||subset_end-subset_begin<=threshold){
      subset=&d;ivf=false;route=ann?"ann_filter_exact":"exact_filter";filters.clear();
    }
  }
  size_t workspace=size_t(h.slots)*4+c->vector_pages.size()*sizeof(const std::byte*)+size_t(h.nlist)*sizeof(std::pair<double,uint32_t>)+size_t(s.limit)*(sizeof(double)+sizeof(SlotId)+sizeof(Value)*2+sizeof(std::vector<Value>)+64)+4096;
  if(workspace>storage.budget.limit-storage.budget.used){read_cache.reset();return false;}
  auto reservation=std::make_shared<Reservation>(storage.budget,workspace);
  // Validate without loading: loading one missing page could evict any earlier
  // borrowed address. The all-resident scan below makes no pool calls at all.
  std::vector<const std::byte*> bases(c->vector_pages.size());bool all_resident=true;
  if(subset){
    for(auto at=subset_begin;at<subset_end;++at){auto page=subset->postings[at]/count;
      if(!bases[page])bases[page]=storage.pool.resident(c->vector_pages[page]);
      if(!bases[page]){all_resident=false;break;}
    }
  }else for(size_t i=0;i<bases.size();++i){bases[i]=storage.pool.resident(c->vector_pages[i]);all_resident&=bases[i]!=nullptr;}
  std::vector<SlotId> candidates;
  std::vector<std::pair<double,uint32_t>> distances;
  if(ivf){route="ivf";last_probes=std::min(nprobe?nprobe:h.recommended_nprobe,h.nlist);distances.reserve(h.nlist);
    for(uint32_t b=0;b<h.nlist;++b)distances.emplace_back(l2_squared_fast(query.data(),c->centers.data()+size_t(b)*dim,dim,safe&&!has_extreme(c->centers.data()+size_t(b)*dim,dim)),b);
    std::sort(distances.begin(),distances.end());
    // ANN need only the selected lists. Handles for never-probed lists can
    // remain cold even when every address this query uses is already resident.
    if(!all_resident){all_resident=true;for(size_t i=0;i<last_probes&&all_resident;++i){auto b=distances[i].second;for(auto at=c->offsets[b];at<c->offsets[b+1];++at)if(!bases[c->postings[at]/count]){all_resident=false;break;}}}
    if(!all_resident){candidates.reserve(h.slots);for(size_t i=0;i<last_probes;++i){auto b=distances[i].second;candidates.insert(candidates.end(),c->postings.begin()+c->offsets[b],c->postings.begin()+c->offsets[b+1]);}std::sort(candidates.begin(),candidates.end());}
  }
  struct Neighbor{double squared;SlotId slot;};std::vector<Neighbor> heap;heap.reserve(s.limit);
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
  auto accepts=[&](SlotId slot) __attribute__((always_inline)) {
    for(auto [i,id]:filters)if(c->dictionaries[i].ids[slot]!=id)return false;
    return !impossible_key&&(!key_filter||c->keys[slot]==*key_filter);
  };
  auto finish=[&](SlotId slot,const float* p,double fast) __attribute__((always_inline)) {
    if(heap.size()==s.limit){auto worst=heap.front().squared;
      auto error=std::max(1e-5,4.0*dim*std::numeric_limits<float>::epsilon());
      if(fast>worst+error*std::max({1.0,std::abs(fast),std::abs(worst)}))return;}
    // Every potential winner is widened, preserving underflow and SQL PK ties.
    Neighbor neighbor{l2_squared_exact(p,query.data(),dim),slot};
    if(heap.size()<s.limit){heap.push_back(neighbor);std::push_heap(heap.begin(),heap.end(),less);}else if(less(neighbor,heap.front())){std::pop_heap(heap.begin(),heap.end(),less);heap.back()=neighbor;std::push_heap(heap.begin(),heap.end(),less);}
  };
  auto consume=[&]<bool Resident128>(SlotId slot) __attribute__((always_inline)) {
    if(!accepts(slot))return;
    const float* p;
    if constexpr(Resident128)p=reinterpret_cast<const float*>(bases[slot>>5]+((slot&31U)<<9));
    else {auto page=slot/count;if(all_resident)vector_page=bases[page];else if(page!=current){
      auto& handle=c->vector_pages[page];vector_page=storage.pool.resident(handle);
      if(!vector_page){handle=storage.pool.handle(file,page);vector_page=storage.pool.resident(handle);}current=page;
    }
    // Borrowed pointers never survive a pool operation in the paged path.
    p=reinterpret_cast<const float*>(vector_page+(slot%count)*vector_stride);}
    auto fast=heap.size()==s.limit?l2_squared_fast(p,query.data(),dim,safe&&!(c->flags[slot]&2)):0;
    finish(slot,p,fast);
  };
  auto scan=[&]<bool Resident128,bool Prefetch>(auto first,auto last){
    // Gather only accepted active slots. All addresses are validated before
    // entering this path; batching never introduces a pool load or eviction.
    if(all_resident){
      SlotId slots[4];const float* vectors[4];size_t filled=0;
      auto flush=[&]() __attribute__((always_inline)) {double fast[4];bool batch_safe=safe;
        for(size_t j=0;j<4;++j)batch_safe&=!(c->flags[slots[j]]&2);
        l2_sq_f32_batch4(query.data(),vectors,dim,fast,batch_safe);
        for(size_t j=0;j<4;++j)finish(slots[j],vectors[j],fast[j]);
        filled=0;
      };
      for(auto it=first;it!=last;++it){auto slot=*it;if(!accepts(slot))continue;
        slots[filled]=slot;
        if constexpr(Resident128)vectors[filled]=reinterpret_cast<const float*>(bases[slot>>5]+((slot&31U)<<9));
        else vectors[filled]=reinterpret_cast<const float*>(bases[slot/count]+(slot%count)*vector_stride);
        ++filled;
#if defined(__GNUC__) || defined(__clang__)
        if constexpr(Prefetch){if(it+1!=last){auto next=*(it+1);
          if constexpr(Resident128)__builtin_prefetch(bases[next>>5]+((next&31U)<<9),0,1);
          else __builtin_prefetch(bases[next/count]+(next%count)*vector_stride,0,1);}}
#endif
        if(filled==4)flush();
      }
      for(size_t j=0;j<filled;++j)consume.template operator()<Resident128>(slots[j]);
    }else for(auto it=first;it!=last;++it)consume.template operator()<Resident128>(*it);
  };
  auto run=[&]<bool Resident128,bool Prefetch>(){
    if(subset)scan.template operator()<Resident128,Prefetch>(subset->postings.begin()+subset_begin,subset->postings.begin()+subset_end);
    else if(ivf&&all_resident){for(size_t i=0;i<last_probes;++i){auto b=distances[i].second;scan.template operator()<Resident128,Prefetch>(c->postings.begin()+c->offsets[b],c->postings.begin()+c->offsets[b+1]);}}
    else if(ivf)scan.template operator()<Resident128,Prefetch>(candidates.begin(),candidates.end());
    else scan.template operator()<Resident128,Prefetch>(c->active_slots.begin(),c->active_slots.end());
  };
  // The common 128D resident path uses fixed shifts/masks. Exact scans already
  // visit slots in storage order; prefetching every next slot adds no value.
  if(all_resident&&dim==128){if(ivf)run.template operator()<true,true>();else run.template operator()<true,false>();}
  else run.template operator()<false,false>();
  std::sort(heap.begin(),heap.end(),less);result.memory_rows=std::make_shared<std::vector<std::vector<Value>>>();result.memory_rows->reserve(heap.size());
  for(auto& n:heap){std::vector<Value> values(2);values[distance]=std::sqrt(n.squared);values[1-distance]=c->keys[n.slot];result.memory_rows->push_back(std::move(values));}
  std::vector<SlotId>().swap(candidates);std::vector<Neighbor>().swap(heap);std::vector<const std::byte*>().swap(bases);std::vector<std::pair<double,uint32_t>>().swap(distances);
  auto retained=result.memory_rows->capacity()*(sizeof(std::vector<Value>)+2*sizeof(Value))+256;
  storage.budget.release(reservation->n-retained);reservation->n=retained;
  result.memory_reservation=std::move(reservation);return true;
}
Result Engine::execute(const std::string& sql){
  try {
  last_probes=last_lists=last_recommended=0;route="none";
  if(is_insert(sql)){
    read_cache.reset();Reservation input(storage.budget,sql.size()+512);
    bool automatic=!storage.in_transaction();if(automatic)storage.begin();auto point=storage.savepoint();
    try{auto result=insert_stream(sql);if(automatic)storage.commit();return result;}
    catch(...){storage.rollback_to(point);if(automatic)storage.rollback();throw;}
  }
  if(sql.size()*4+512>storage.budget.limit-storage.budget.used)read_cache.reset();
  Reservation input(storage.budget,sql.size()*4+512);auto s=parse(sql);
  if(s.kind!=Statement::Select&&s.kind!=Statement::Set)read_cache.reset();
  if(s.kind==Statement::Begin){storage.begin();return {};}
  if(s.kind==Statement::Commit){storage.commit();return {};}
  if(s.kind==Statement::Rollback){storage.rollback();return {};}
  if(s.kind==Statement::Set){if(s.setting=="search_mode"){require(s.value=="exact"||s.value=="ann","invalid local search mode");ann=s.value=="ann";}else if(s.setting=="nprobe"){auto value=convert({s.value,false},{"",Type::Bigint});auto n=std::get<int64_t>(value);require(n>=0&&n<=64,"nprobe must be 0..64");nprobe=static_cast<uint32_t>(n);}else throw std::runtime_error("unknown local setting");return {};}
  bool writes=s.kind!=Statement::Select;bool automatic=writes&&!storage.in_transaction();if(automatic)storage.begin();auto point=storage.savepoint();
  try{auto result=execute_inner(s);if(automatic)storage.commit();return result;}
  catch(...){if(writes){storage.rollback_to(point);if(automatic)storage.rollback();}throw;}
  }catch(...){read_cache.reset();throw;}
}
Result Engine::insert_stream(const std::string& sql){
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
  parse_insert_rows(sql,[&](const Statement& s){table=s.table;h=header(table);cols=schema(h);pkcol=primary(cols);
    if(s.insert_columns.empty()){for(size_t i=0;i<cols.size();++i)order.push_back(i);}else for(auto& name:s.insert_columns)order.push_back(column(cols,name));
    require(order.size()==cols.size(),"all columns must have values");auto sorted=order;std::sort(sorted.begin(),sorted.end());require(std::adjacent_find(sorted.begin(),sorted.end())==sorted.end(),"duplicate INSERT column");
  },[&](const std::vector<LiteralView>& tuple){
    require(tuple.size()==cols.size(),"INSERT width mismatch");
    size_t bytes=512+cols.size()*(sizeof(Value)+sizeof(LiteralView));for(size_t i=0;i<tuple.size();++i){auto& col=cols[order[i]];bytes+=col.type==Type::Vector?size_t(col.width)*4+tuple[i].text.size():(col.type==Type::Varchar?tuple[i].text.size()*4:0);}
    Reservation row_memory(storage.budget,bytes);
    std::vector<Value> values(cols.size());for(size_t j=0;j<tuple.size();++j)values[order[j]]=convert(tuple[j],cols[order[j]]);
    if(!capacity){write_row(table,h.slots,std::move(values),cols,true);h=header(table);++result.affected_rows;return;}
    require(h.slots<UINT32_MAX,"SlotId exhausted");SlotId slot=h.slots;auto key=std::get<int64_t>(values[pkcol]);PrimaryIndex pk(storage,table+".pk");require(!pk.find(key),"duplicate primary key");
    Meta m;m.active=1;m.key=key;
    if(h.nlist){m.bucket=assign(table,h,std::get<std::vector<float>>(values[h.ivector]));auto base=index_base(table,h);if(h.index_region[0])edit(base+".buckets",slot/(PAGE/4),(slot%(PAGE/4))*4,&m.bucket,4,true);posting(base,m.bucket,slot,true);}
    for(size_t i=0;i<cols.size();++i){
      if(cols[i].type==Type::Vector){auto& v=std::get<std::vector<float>>(values[i]);m.reserved|=has_extreme(v.data(),v.size())?1:0;auto count=PAGE/stride(cols[i]);edit(table+".v"+std::to_string(i),slot/count,(slot%count)*stride(cols[i]),v.data(),v.size()*4,true);values[i]=std::vector<float>{};}
      else if(!cols[i].primary){auto hash=scalar_hash(values[i]);edit(table+".s"+std::to_string(i),slot/(PAGE/8),(slot%(PAGE/8))*8,&hash,8,true);}
    }
    auto packed=pack_values(values);require(packed.size()<storage.budget.limit/4,"row exceeds workspace budget");m.offset=h.blob_end;m.length=static_cast<uint32_t>(packed.size());
    for(size_t at=0;at<packed.size();){auto offset=h.blob_end+at;auto n=std::min<size_t>(packed.size()-at,PAGE-offset%PAGE);edit(table+".blob",offset/PAGE,offset%PAGE,packed.data()+at,n,true);at+=n;}
    h.blob_end+=packed.size();edit(table+".rows",slot/(PAGE/sizeof(Meta)),(slot%(PAGE/sizeof(Meta)))*sizeof(Meta),&m,sizeof(m),true);set_valid(table+".valid",slot);
    pk.insert(key,slot);++h.slots;++h.active_count;if(h.nlist&&h.mutations_since_calibration<UINT64_MAX)++h.mutations_since_calibration;++result.affected_rows;
  },64);
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
  if(s.kind==Statement::Insert){std::vector<size_t> order;if(s.insert_columns.empty()){for(size_t i=0;i<cols.size();++i)order.push_back(i);}else for(auto& name:s.insert_columns)order.push_back(column(cols,name));
    require(order.size()==cols.size(),"all columns must have values");auto sorted=order;std::sort(sorted.begin(),sorted.end());require(std::adjacent_find(sorted.begin(),sorted.end())==sorted.end(),"duplicate INSERT column");
    for(auto& tuple:s.rows){require(tuple.size()==cols.size(),"INSERT width mismatch");std::vector<Value> values(cols.size());for(size_t j=0;j<tuple.size();++j)values[order[j]]=convert(tuple[j],cols[order[j]]);auto slot=header(s.table).slots;write_row(s.table,slot,std::move(values),cols,true);++result.affected_rows;}return result;}
  if(s.kind==Statement::Update||s.kind==Statement::Delete){std::vector<std::pair<size_t,Value>> changes;for(auto& [name,v]:s.assignments){auto i=column(cols,name);changes.emplace_back(i,convert(v,cols[i]));}
    for(SlotId slot=0;slot<h.slots;++slot){auto m=meta(s.table,slot);if(!m.active)continue;auto values=row(s.table,slot,m,cols,true);if(!matches(values))continue;for(auto& [i,v]:changes)values[i]=v;write_row(s.table,slot,std::move(values),cols,s.kind==Statement::Update);++result.affected_rows;}return result;}
  require(s.kind==Statement::Select,"invalid execution kind");
  std::vector<Expr> expressions;for(auto& e:s.expressions){if(e.column=="*")for(auto& c:cols)expressions.push_back({c.name,c.name,false,{}});else expressions.push_back(e);}
  std::vector<size_t> selected;std::vector<std::vector<float>> queries;
  for(auto& e:expressions){auto i=column(cols,e.column);selected.push_back(i);result.columns.push_back(e.alias);if(e.distance){require(cols[i].type==Type::Vector,"distance requires VECTOR");queries.push_back(std::get<std::vector<float>>(convert({e.query,true},cols[i])));}else queries.emplace_back();}
  std::vector<std::pair<size_t,bool>> order;
  for(auto& [name,desc]:s.order){size_t i=0;for(;i<expressions.size();++i)if(expressions[i].alias==name)break;if(i<expressions.size())order.emplace_back(i,desc);else order.emplace_back(expressions.size()+column(cols,name),desc);}
  if(fast_topk(s,h,cols,expressions,selected,queries,order,predicates,result))return result;
  read_cache.reset();
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
  if(use_ivf){route="ivf";last_probes=std::min(nprobe?nprobe:h.recommended_nprobe,h.nlist);std::vector<std::pair<double,uint32_t>> distances;for(uint32_t b=0;b<h.nlist;++b){auto p=center_view(index_base(s.table,h),b,queries[distance_expr].size());distances.emplace_back(l2(queries[distance_expr].data(),p,queries[distance_expr].size()),b);}std::sort(distances.begin(),distances.end());for(size_t i=0;i<std::min<size_t>(last_probes,distances.size());++i)buckets.push_back(distances[i].second);}
  std::optional<SlotId> exact_slot;bool pk_filter=false;
  for(size_t j=0;j<s.conditions.size();++j)if(predicates[j].first==keycol&&s.conditions[j].op=="="){pk_filter=true;exact_slot=PrimaryIndex(storage,s.table+".pk").find(std::get<int64_t>(predicates[j].second));}
  if(s.limit==0)return result;
  std::vector<std::pair<size_t,uint64_t>> equality_sketches;
  for(size_t j=0;j<predicates.size();++j)if(s.conditions[j].op=="="&&predicates[j].first!=keycol&&cols[predicates[j].first].type!=Type::Vector)equality_sketches.emplace_back(predicates[j].first,scalar_hash(predicates[j].second));
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
  for(uint64_t start=0;start<h.slots;start+=4096){std::array<uint64_t,64> candidates{};
    if(use_ivf){for(auto b:buckets){auto f=index_base(s.table,h)+".ivf"+std::to_string(b);auto page=storage.read(f,0);uint32_t count;std::memcpy(&count,page.data(),4);for(uint32_t i=0;i<count;++i){uint64_t offset=16+uint64_t(i)*4;if(offset%PAGE==0)page=storage.read(f,offset/PAGE);uint32_t slot;std::memcpy(&slot,page.data()+offset%PAGE,4);if(slot>=start&&slot<start+4096)candidates[(slot-start)/64]|=uint64_t(1)<<((slot-start)%64);}}}
    else candidates.fill(UINT64_MAX);
    std::array<uint64_t,64> visible{};
    auto valid=storage.pool.view(s.table+".valid",start/(PAGE*8));
    std::memcpy(visible.data(),valid+(start%(PAGE*8))/8,sizeof(visible));
    for(size_t word=0;word<64;++word)candidates[word]&=visible[word];
    for(auto [c,hash]:equality_sketches){
      auto f=s.table+".s"+std::to_string(c);
      for(uint64_t part=start;part<std::min<uint64_t>(start+4096,h.slots);part+=PAGE/8){
        auto sketch=storage.pool.view(f,part/(PAGE/8));
        for(uint64_t number=part;number<std::min<uint64_t>(part+PAGE/8,h.slots);++number){uint64_t stored;std::memcpy(&stored,sketch+(number%(PAGE/8))*8,8);if(stored!=hash)candidates[(number-start)/64]&=~(uint64_t(1)<<((number-start)%64));}
      }
    }
    // Finish the bitmap intersection before touching vector payload pages.
    for(uint64_t number=start;number<std::min<uint64_t>(start+4096,h.slots);++number){auto slot=static_cast<SlotId>(number);auto mask=uint64_t(1)<<((number-start)%64);auto& word=candidates[(number-start)/64];if(!(word&mask))continue;
      bool yes=!(pk_filter&&(!exact_slot||slot!=*exact_slot));
      if(yes&&!predicates.empty()){auto m=meta(s.table,slot);yes=m.active&&matches(row(s.table,slot,m,cols,false));}if(!yes)word&=~mask;
    }
    for(uint64_t number=start;number<std::min<uint64_t>(start+4096,h.slots);++number){auto slot=static_cast<SlotId>(number);if(!(candidates[(number-start)/64]&(uint64_t(1)<<((number-start)%64))))continue;
      auto m=meta(s.table,slot);
      // SIMD rejects clearly losing candidates. Heap entries and potential
      // winners use float64 recomputation, preserving strict exact tie order.
      if(use_heap&&heap.size()==s.limit&&!order.empty()&&order[0].first<projected&&expressions[order[0].first].distance){
        auto i=order[0].first,c=selected[i];auto p=payload(slot,c);auto d=l2_fast(p,queries[i].data(),cols[c].width,m.reserved==0&&query_safe[i]);
        auto worst=std::get<double>(heap.front()[i]);auto margin=1e-5*std::max({1.0,std::abs(d),std::abs(worst)});
        if((!order[0].second&&d>worst+margin)||(order[0].second&&d<worst-margin))continue;
      }
      auto values=need_values?row(s.table,slot,m,cols,false):std::vector<Value>(cols.size());values[keycol]=m.key;
      std::vector<Value> output;for(size_t i=0;i<expressions.size();++i){auto c=selected[i];if(expressions[i].distance){auto p=payload(slot,c);output.emplace_back(l2_exact(p,queries[i].data(),cols[c].width));}
        else if(cols[c].type==Type::Vector){auto p=payload(slot,c);output.emplace_back(std::vector<float>(p,p+cols[c].width));}else output.push_back(values[c]);}
      output.insert(output.end(),values.begin(),values.end());output.emplace_back(m.key);
      if(use_heap){if(heap.size()<s.limit){heap.push_back(std::move(output));std::push_heap(heap.begin(),heap.end(),less);}else if(less(output,heap.front())){std::pop_heap(heap.begin(),heap.end(),less);heap.back()=std::move(output);std::push_heap(heap.begin(),heap.end(),less);}}
      else sorter->add(std::move(output));
    }
  }
  if(use_heap){std::sort(heap.begin(),heap.end(),less);for(auto& r:heap)r.resize(projected);result.memory_rows=std::make_shared<std::vector<std::vector<Value>>>(std::move(heap));result.memory_reservation=std::move(heap_reservation);return result;}
  auto sorted=sorter->finish();result.rows=storage.temporary();std::vector<Value> values;uint64_t returned=0;
  while(returned<s.limit&&spill_read(sorted.get(),values)){values.resize(projected);spill_write(result.rows.get(),values);++returned;}
  discard_cache(sorted.get());discard_cache(result.rows.get());return result;
}
}
