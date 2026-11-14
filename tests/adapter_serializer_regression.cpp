// The public formatter oracle remains the unchanged R9 SQL JSON formatter.
// Include the private adapter assembly API without running its process loop.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <vector>

namespace allocation_probe {
thread_local bool active=false;
thread_local size_t calls=0;
}
[[gnu::noinline]] void* operator new(std::size_t size){
  void* p=std::malloc(size?size:1);
  if(!p)throw std::bad_alloc();
  if(allocation_probe::active)++allocation_probe::calls;
  return p;
}
[[gnu::noinline]] void* operator new[](std::size_t size){return ::operator new(size);}
[[gnu::noinline]] void operator delete(void* p) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete[](void* p) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete(void* p,std::size_t) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

#define main adapter_program_main
#include "../src/local_adapter.cpp"
#undef main

namespace {
struct HistoricalResponse {
  std::string data;
  void clear(){if(data.capacity()>256*1024)std::string{}.swap(data);else data.clear();}
  void add(std::string_view text){
    if(text.size()>16*1024*1024-data.size())throw std::runtime_error("response exceeds protocol size limit");
    data+=text;
  }
};
// Exact pre-change R9 assembly, with emit deliberately omitted. It uses the
// unchanged mvdb::json/quote functions rather than reimplementing numbers or
// escapes, and retains the old output-string validation and temporary strings.
std::string historical_value(const mvdb::Value& value){
  if(auto number=std::get_if<double>(&value))if(!std::isfinite(*number))throw std::runtime_error("non-finite result value");
  if(auto vector=std::get_if<std::vector<float>>(&value))for(float number:*vector)if(!std::isfinite(number))throw std::runtime_error("non-finite vector result value");
  if(auto text=std::get_if<std::string>(&value))mvdb::protocol::Parser(mvdb::quote(*text)).parse();
  return mvdb::json(value);
}
void historical_assemble(HistoricalResponse& response,const std::string& id,bool legacy,const mvdb::Result& result){
  response.clear();response.add(legacy?"{\"ok\":true":"{\"id\":"+id+",\"ok\":true");
  if(legacy||result.columns.empty())response.add(",\"affected_rows\":"+std::to_string(result.affected_rows));
  if(legacy||!result.columns.empty()){
    response.add(",\"columns\":[");bool first=true;for(const auto& name:result.columns){if(!first)response.add(",");first=false;response.add(mvdb::quote(name));}response.add("],\"rows\":[");first=true;
    result.each([&](const auto& row){if(!first)response.add(",");first=false;response.add("[");bool cell=true;for(const auto& value:row){if(!cell)response.add(",");cell=false;response.add(historical_value(value));}response.add("]");});response.add("]");
  }
  response.add("}");
}
void same_value(Response& response,const mvdb::Value& value){
  response.clear();response.value(value);
  assert(response.view()==mvdb::json(value));
}
template<class Action> void rejects(Action action){
  bool failed=false;try{action();}catch(const std::exception&){failed=true;}
  assert(failed);
}
void scalar_and_string_compatibility(){
  Response response;
  for(int64_t value:{std::numeric_limits<int64_t>::min(),int64_t(-9007199254740993LL),int64_t(-1),int64_t(0),int64_t(1),int64_t(9007199254740993LL),std::numeric_limits<int64_t>::max()})same_value(response,value);
  response.clear();response.integer(std::numeric_limits<uint64_t>::max());
  assert(response.view()==std::to_string(std::numeric_limits<uint64_t>::max()));
  for(double value:{0.0,-0.0,1.0,-1.0,std::numeric_limits<double>::denorm_min(),-std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::min(),-std::numeric_limits<double>::min(),std::numeric_limits<double>::max(),-std::numeric_limits<double>::max(),std::nextafter(1.0,0.0),std::nextafter(1.0,2.0),1e-7,1e20})same_value(response,value);
  std::mt19937_64 random(0x523953455249414cULL);
  for(size_t i=0;i<20000;++i){auto value=std::bit_cast<double>(random());if(std::isfinite(value))same_value(response,value);}
  std::string controls;for(int c=0;c<32;++c)controls+=char(c);
  for(const auto& text:std::vector<std::string>{"",controls,"quotes\" and slash\\ and /","中文 😀 café",std::string("\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"),std::string(70000,'a')+controls+"中文\"\\😀"}){
    same_value(response,text);response.clear();response.string(text);assert(response.view()==mvdb::quote(text));
  }
  const std::vector<std::string> invalid={std::string("\x80"),std::string("\xc0\xaf"),std::string("\xc1\xbf"),std::string("\xc2"),std::string("\xc2x"),std::string("\xe0\x80\x80"),std::string("\xe2\x82"),std::string("\xed\xa0\x80"),std::string("\xf0\x8f\xbf\xbf"),std::string("\xf4\x90\x80\x80"),std::string("\xf5\x80\x80\x80"),std::string("\xfe"),std::string("\xff")};
  for(const auto& bad:invalid){
    rejects([&]{response.clear();response.string(bad);});
    rejects([&]{response.clear();response.value(std::string("valid prefix 中文")+bad);});
  }
  for(double bad:{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
    rejects([&]{response.clear();response.number(bad);});
    rejects([&]{response.clear();response.value(bad);});
  }
  same_value(response,std::string("valid after rejected values"));
}
void vector_compatibility(){
  Response response;
  std::vector<float> extremes={0.f,-0.f,std::numeric_limits<float>::denorm_min(),-std::numeric_limits<float>::denorm_min(),std::numeric_limits<float>::min(),-std::numeric_limits<float>::min(),std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),std::nextafter(1.f,0.f),std::nextafter(1.f,2.f)};
  same_value(response,extremes);
  std::mt19937 random(0x52395645);
  for(size_t dim:{size_t(0),size_t(1),size_t(7),size_t(32),size_t(127),size_t(128),size_t(129),size_t(4096)}){
    std::vector<float> values;values.reserve(dim);
    while(values.size()<dim){float value=std::bit_cast<float>(uint32_t(random()));if(std::isfinite(value))values.push_back(value);}
    same_value(response,values);
  }
  for(float bad:{std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
    auto values=extremes;values.push_back(bad);
    rejects([&]{response.clear();response.value(values);});
  }
}
void assembly_rejections(){
  Response response;mvdb::Result result;result.columns={"id","value"};
  result.memory_rows=std::make_shared<std::vector<std::vector<mvdb::Value>>>();
  result.memory_rows->push_back({int64_t(1),std::string("valid first row")});
  result.memory_rows->push_back({int64_t(2),std::string("bad UTF8 \xed\xa0\x80")});
  rejects([&]{assemble_success(response,"1",false,result);});
  result.memory_rows->back()[1]=std::numeric_limits<double>::infinity();
  rejects([&]{assemble_success(response,"1",false,result);});
  result.memory_rows->back()[1]=std::vector<float>{1,2,std::numeric_limits<float>::quiet_NaN()};
  rejects([&]{assemble_success(response,"1",false,result);});
  result.memory_rows->back()[1]=std::string("valid again");
  result.columns.back()="bad UTF8 \xf4\x90\x80\x80";
  rejects([&]{assemble_success(response,"1",false,result);});
  // assemble_success is build-only: any accidental emission above would also
  // make this executable's single JSON report fail the Python wrapper parser.
}
struct Fixture {const char* name;std::string id;bool legacy;mvdb::Result result;bool common_zero_allocation_required=true;};
std::vector<Fixture> fixtures(){
  std::vector<Fixture> out;
  Fixture top{"topk10","9007199254740993",false,{}};top.result.columns={"id","distance"};top.result.memory_rows=std::make_shared<std::vector<std::vector<mvdb::Value>>>();
  for(int i=0;i<10;++i)top.result.memory_rows->push_back({int64_t(9007199254740993LL+i),std::nextafter(1.0+i,2.0+i)});
  out.push_back(std::move(top));
  Fixture affected{"affected_rows","{\"opaque\":[true,9007199254740993]}",false,{}};affected.result.affected_rows=std::numeric_limits<int64_t>::max();out.push_back(std::move(affected));
  Fixture vectors{"multirow_vectors","\"rows 中文\\u000a😀\"",false,{}};vectors.result.columns={"id","vector","quoted\"\\\n中文😀"};vectors.result.memory_rows=std::make_shared<std::vector<std::vector<mvdb::Value>>>();
  std::mt19937 random(12345);
  for(int row=0;row<32;++row){std::vector<float> values;values.reserve(128);while(values.size()<128){float value=std::bit_cast<float>(uint32_t(random()));if(std::isfinite(value))values.push_back(value);}vectors.result.memory_rows->push_back({int64_t(row),std::move(values),std::string("quotes\" slash\\ controls\n\t 中文😀")});}
  out.push_back(std::move(vectors));
  Fixture large=out.back();large.name="large_warmed_response";large.common_zero_allocation_required=false;
  large.result.memory_rows=std::make_shared<std::vector<std::vector<mvdb::Value>>>();
  for(int i=0;i<160;++i)large.result.memory_rows->push_back((*out.back().result.memory_rows)[i%32]);
  out.push_back(std::move(large));
  Fixture legacy{"legacy_topk10","null",true,out.front().result};legacy.result.affected_rows=-7;out.push_back(std::move(legacy));
  Fixture empty{"legacy_empty","null",true,{}};out.push_back(std::move(empty));
  return out;
}
struct Sample {const char* name;size_t bytes,iterations,new_calls,old_calls;double new_ms,old_ms;bool common_zero_allocation_required;};
Sample probe(const Fixture& fixture,size_t iterations,bool timing){
  Response response;HistoricalResponse historical;
  // Fixture setup and initial growth precede counting. As in R9, oversized
  // capacity is released on clear, so oversized responses may grow again.
  assemble_success(response,fixture.id,fixture.legacy,fixture.result);
  historical_assemble(historical,fixture.id,fixture.legacy,fixture.result);
  assert(response.view()==historical.data);
  const auto bytes=response.view().size();
  allocation_probe::calls=0;allocation_probe::active=true;
  for(size_t i=0;i<iterations;++i)assemble_success(response,fixture.id,fixture.legacy,fixture.result);
  allocation_probe::active=false;const auto new_calls=allocation_probe::calls;
  if(fixture.common_zero_allocation_required)assert(new_calls==0);
  else assert(new_calls<=iterations*(4+std::bit_width(bytes)));
  assert(response.view()==historical.data);
  allocation_probe::calls=0;allocation_probe::active=true;
  for(size_t i=0;i<iterations;++i)historical_assemble(historical,fixture.id,fixture.legacy,fixture.result);
  allocation_probe::active=false;const auto old_calls=allocation_probe::calls;
  assert(response.view()==historical.data);
  if(!fixture.common_zero_allocation_required)assert(new_calls<old_calls);
  double new_ms=0,old_ms=0;
  if(timing){
    auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<iterations;++i)assemble_success(response,fixture.id,fixture.legacy,fixture.result);
    auto middle=std::chrono::steady_clock::now();
    for(size_t i=0;i<iterations;++i)historical_assemble(historical,fixture.id,fixture.legacy,fixture.result);
    auto end=std::chrono::steady_clock::now();
    new_ms=std::chrono::duration<double,std::milli>(middle-start).count();old_ms=std::chrono::duration<double,std::milli>(end-middle).count();
  }
  return {fixture.name,bytes,iterations,new_calls,old_calls,new_ms,old_ms,fixture.common_zero_allocation_required};
}
}
int main(int argc,char** argv){
  size_t iterations=3;bool timing=false;
  if(argc==3&&std::string_view(argv[1])=="--benchmark"){
    auto end=argv[2]+std::char_traits<char>::length(argv[2]);
    auto parsed=std::from_chars(argv[2],end,iterations);
    if(parsed.ec!=std::errc{}||parsed.ptr!=end||iterations==0||iterations>1000000)return 2;
    timing=true;
  }else if(argc!=1)return 2;
  scalar_and_string_compatibility();vector_compatibility();assembly_rejections();
  auto cases=fixtures();std::vector<Sample> samples;
  for(const auto& fixture:cases)samples.push_back(probe(fixture,iterations,timing));
  std::cout<<"{\"scope\":\"warmed response assembly; prebuilt in-memory Result; request parsing, engine and I/O excluded\",\"samples\":[";
  bool first=true;for(const auto& sample:samples){if(!first)std::cout<<',';first=false;
    std::cout<<"{\"name\":\""<<sample.name<<"\",\"bytes\":"<<sample.bytes<<",\"iterations\":"<<sample.iterations<<",\"response_allocations\":"<<sample.new_calls<<",\"r9_allocations\":"<<sample.old_calls<<",\"common_zero_allocation_required\":"<<(sample.common_zero_allocation_required?"true":"false");
    if(timing)std::cout<<",\"response_ms\":"<<sample.new_ms<<",\"r9_ms\":"<<sample.old_ms;
    std::cout<<'}';
  }
  std::cout<<"],\"formatter_compatibility\":true}\n";
}
