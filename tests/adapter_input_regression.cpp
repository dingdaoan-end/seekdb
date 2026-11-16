// Differential oracle: unchanged strict JSON protocol parser.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <new>
#include <random>
namespace input_allocations {thread_local bool active=false;thread_local size_t calls=0;}
[[gnu::noinline]] void* operator new(std::size_t n){auto p=std::malloc(n?n:1);if(!p)throw std::bad_alloc();if(input_allocations::active)++input_allocations::calls;return p;}
[[gnu::noinline]] void* operator new[](std::size_t n){return ::operator new(n);}
[[gnu::noinline]] void operator delete(void* p) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete[](void* p) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete(void* p,std::size_t) noexcept {std::free(p);}
[[gnu::noinline]] void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
#define main adapter_program_main
#include "../src/local_adapter.cpp"
#undef main
namespace {
struct Observed {std::string id="null",op,sql,mode,error;bool operator==(const Observed&) const=default;};
Observed original(std::string_view input){
  Observed out;
  try{
    auto request=mvdb::protocol::Parser(input,true).parse();
    if(request.kind!='{')throw std::runtime_error("request must be a JSON object");
    auto ident=request.fields.find("id");if(ident==request.fields.end())throw std::runtime_error("request requires id");out.id=ident->second.raw;
    out.op=request.fields.contains("op")?mvdb::protocol::required_string(request,"op"):"execute";
    if(out.op=="execute"){
      out.sql=mvdb::protocol::required_string(request,"sql");out.mode=request.fields.contains("search_mode")?mvdb::protocol::required_string(request,"search_mode"):"exact";
      if(out.mode=="approx")out.mode="ann";
      if(out.mode!="exact"&&out.mode!="ann")throw std::runtime_error("search_mode must be exact, ann, or approx");
    }
  }catch(const std::exception& e){out.error=e.what();}return out;
}
Observed current(std::string_view input){
  Observed out;RequestInput request;
  try{
    request.parse(input);out.id=request.id();out.op=request.operation();
    if(out.op=="execute"){
      out.sql=request.sql();out.mode=request.mode();if(out.mode=="approx")out.mode="ann";
      if(out.mode!="exact"&&out.mode!="ann")throw std::runtime_error("search_mode must be exact, ann, or approx");
    }
  }catch(const std::exception& e){out.error=e.what();}return out;
}
void differential(const std::string& input){assert(original(input)==current(input));}
void correctness(){
  std::vector<std::string> corpus={
    R"({"id":90071992547409931234567890,"sql":"SELECT 中文😀 FROM t;"})",
    R"( { "sql" : "SELECT 1;", "search_mode":"approx", "op":"execute", "id": -0.0001E+9999 } )",
    R"({"id":"中文😀","op":"ping"})",R"({"id":true,"op":"shutdown"})",R"({"id":null,"op":"close"})",
    R"({"id":1})",R"({"id":1,"sql":"","search_mode":"invalid"})",R"({"id":1,"op":"unknown"})",
    R"({"id":1,"sql":false})",R"({"id":1,"op":3})",R"({"id":1,"sql":"x","search_mode":null})",
    R"({"id":1,"id":2,"op":"ping"})",R"({"id":1,"op":"ping","mode":"ann"})",
    R"({"id":{"opaque":[true,9007199254740993]},"op":"ping"})",R"({"id":"a\nb","sql":"SELECT \"x\";"})",
    R"({"\u0069d":1,"op":"ping"})",R"({"id":1,"\u0069d":2})",R"({"id":1,"sql":"\ud800"})",
    "{}","[]","null","",R"({"id":1,"op":"ping",})",R"({"id":1,"op":"ping"}garbage)"
  };
  for(auto id:{"01","-01","1.",".1","+1","1e","1e+","--1","NaN","Infinity","truex","nul","-","1e+-2"})corpus.push_back(std::string("{\"id\":")+id+",\"op\":\"ping\"}");
  for(auto bad:{std::string("\x80"),std::string("\xc0\xaf"),std::string("\xc2"),std::string("\xe0\x80\x80"),std::string("\xed\xa0\x80"),std::string("\xf0\x8f\xbf\xbf"),std::string("\xf4\x90\x80\x80"),std::string("\xf5\x80\x80\x80"),std::string(1,'\0'),std::string(1,'\n')}){
    for(auto key:{"id","sql","op","search_mode"})corpus.push_back(std::string("{\"id\":1,\"")+key+"\":\""+bad+"\"}");
    corpus.push_back(std::string("{\"id\":\"")+bad+"\",\"op\":\"ping\"}");
  }
  for(const auto& input:corpus)differential(input);
  // Byte mutations check truncations, controls, separators and UTF-8 boundaries.
  std::mt19937 rng(0x1a223ca3);
  for(size_t i=0;i<30000;++i){auto input=corpus[rng()%corpus.size()];for(unsigned j=0,n=1+rng()%3;j<n;++j){size_t at=rng()%(input.size()+1);switch(rng()%3){case 0:input.insert(at,1,char(rng()%256));break;case 1:if(at<input.size())input.erase(at,1);break;default:if(at<input.size())input[at]=char(rng()%256);}}differential(input);}
  for(const auto& input:std::vector<std::string>{R"({"id":1,"op":"ping","mode":"ann"})",R"({"id":[1],"op":"ping"})",R"({"id":"a\u0062","op":"ping"})",R"({"id":1,"id":2})"}){FlatRequest flat;assert(!flat.parse(input));}
  RequestInput reusable;reusable.parse(R"({"id":1,"op":"ping"})");assert(reusable.fast);reusable.parse(R"({"id":2,"sql":"SELECT 1;"})");assert(reusable.fast&&reusable.id()=="2"&&reusable.operation()=="execute");
}
size_t checksum=0;
void benchmark(const char* name,const std::string& input,size_t iterations){
  RequestInput warm;warm.parse(input);assert(warm.fast);differential(input);
  input_allocations::calls=0;input_allocations::active=true;
  for(size_t i=0;i<iterations;++i){RequestInput request;request.parse(input);assert(request.fast);checksum+=request.id().size()+request.operation().size()+request.flat.sql.size()+request.flat.search_mode.size();}
  input_allocations::active=false;size_t fast_calls=input_allocations::calls;assert(fast_calls==0);
  input_allocations::calls=0;input_allocations::active=true;
  for(size_t i=0;i<iterations;++i){auto request=mvdb::protocol::Parser(input,true).parse();checksum+=request.fields.size();}
  input_allocations::active=false;size_t old_calls=input_allocations::calls;assert(old_calls>0);
  auto start=std::chrono::steady_clock::now();
  for(size_t i=0;i<iterations;++i){RequestInput request;request.parse(input);checksum+=request.id().size()+request.operation().size()+request.flat.sql.size()+request.flat.search_mode.size();}
  auto middle=std::chrono::steady_clock::now();
  for(size_t i=0;i<iterations;++i){auto request=mvdb::protocol::Parser(input,true).parse();checksum+=request.fields.size();}
  auto end=std::chrono::steady_clock::now();
  std::cout<<"{\"name\":\""<<name<<"\",\"iterations\":"<<iterations<<",\"bytes\":"<<input.size()<<",\"fast_allocations\":"<<fast_calls<<",\"parser_allocations\":"<<old_calls<<",\"fast_ms\":"<<std::chrono::duration<double,std::milli>(middle-start).count()<<",\"parser_ms\":"<<std::chrono::duration<double,std::milli>(end-middle).count()<<"}";
}
}
int main(int argc,char** argv){
  size_t iterations=1000;if(argc==3&&std::string_view(argv[1])=="--benchmark"){auto last=argv[2]+std::char_traits<char>::length(argv[2]);auto r=std::from_chars(argv[2],last,iterations);if(r.ec!=std::errc{}||r.ptr!=last||!iterations||iterations>1000000)return 2;}else if(argc!=1)return 2;
  correctness();std::cout<<"{\"scope\":\"input parsing only; engine, SQL materialization, response and I/O excluded\",\"differential_mutations\":30000,\"samples\":[";
  benchmark("flat_ping",R"({"id":9007199254740993,"op":"ping"})",iterations);std::cout<<',';
  benchmark("flat_short",R"({"id":9007199254740993,"sql":"SELECT id FROM items;","op":"execute","search_mode":"exact"})",iterations);std::cout<<',';
  std::string vector_sql=R"({"id":"large query 中文😀","sql":"SELECT id FROM items ORDER BY L2_DISTANCE(v,[)";
  for(size_t i=0;i<128;++i){if(i)vector_sql+=',';vector_sql+="0.123456";}
  vector_sql+=R"(]) LIMIT 10;","search_mode":"approx"})";
  benchmark("flat_vector_sql",vector_sql,iterations);std::cout<<"],\"checksum\":"<<checksum<<"}\n";
}