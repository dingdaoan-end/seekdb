#include "../src/sql.hpp"
#include "../src/engine.hpp"
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <cstdlib>
#include <filesystem>
#include <sys/mman.h>
#include <unistd.h>
#include <immintrin.h>

static void check(const float* rows,const float* query,bool safe){
  double out[4];mvdb::l2_squared_batch4_128(rows,query,out,safe);
  for(size_t i=0;i<4;++i){
    auto expected=mvdb::l2_squared_fast(rows+i*128,query,128,safe);
    assert(std::bit_cast<uint64_t>(out[i])==std::bit_cast<uint64_t>(expected));
  }
}
static void engine_checks(){
  auto path=std::filesystem::temp_directory_path()/("mvdb_structural_simd_"+std::to_string(::getpid()));
  assert(!std::filesystem::exists(path));
  const char* before=std::getenv("MVDB_PROGRESSIVE");std::string saved=before?before:"";bool had=before;
  {
    mvdb::Engine engine(path,8*1024*1024);
    engine.execute("CREATE TABLE t(id BIGINT PRIMARY KEY,category VARCHAR(8),v VECTOR(128));");
    auto vector=[](const std::string& x,size_t position){std::string out="[";for(size_t j=0;j<128;++j){if(j)out+=',';out+=j==position?x:"0";}return out+"]";};
    std::string insert="INSERT INTO t VALUES ";
    for(size_t i=0;i<1300;++i){
      if(i)insert+=',';
      std::string x=i%3?"1":"1.00000011920928955078125";
      if(i==1291)x="340282346638528859811704183484516925440";
      if(i==1292)x="0.000000000000000000000000000001";
      insert+='('+std::to_string(int64_t(500)-int64_t(i))+",'"+(i%2?"odd":"even")+"','"+vector(x,i%128)+"')";
    }
    engine.execute(insert+";");
    auto run=[&](bool reference,const std::string& where,const std::string& q){
      std::vector<std::vector<mvdb::Value>> rows;
      engine.execute("SELECT id,L2_DISTANCE(v,'"+q+"') AS d"+(reference?",id AS duplicate":"")+" FROM t"+where+" ORDER BY d,id LIMIT 17;").each([&](const auto& row){rows.push_back({row[0],row[1]});});return rows;
    };
    auto verify=[&]{for(auto policy:{"off","auto"}){
      ::setenv("MVDB_PROGRESSIVE",policy,1);
      for(auto q:{vector("0",0),vector("1",0),vector("0.00000000000000000001",0)})for(auto where:{""," WHERE category='odd'"}){
        auto expected=run(true,where,q);assert(expected.size()==17);assert(run(false,where,q)==expected);
      }
    }};
    verify();engine.execute("BEGIN;");
    // Break candidate adjacency at every fourth row, including page edges.
    for(size_t i=0;i<1300;i+=4)engine.execute("DELETE FROM t WHERE id="+std::to_string(int64_t(500)-int64_t(i))+";");
    verify();engine.execute("ROLLBACK;");verify();
    engine.execute("CREATE VECTOR INDEX vi ON t(v);");engine.execute("SET SEARCH_MODE = ann;");engine.execute("SET NPROBE = 8;");verify();
    engine.close();
  }
  std::filesystem::remove_all(path);
  if(had)::setenv("MVDB_PROGRESSIVE",saved.c_str(),1);else ::unsetenv("MVDB_PROGRESSIVE");
}
int main(){
  std::array<float,4*128+8> rows{};std::array<float,136> query{};
  std::mt19937 rng(29117);std::uniform_real_distribution<float> value(-1,1);
  unsigned csr=_mm_getcsr();
  for(unsigned mode:{0u,0x8040u}){
    _mm_setcsr((csr&~0x8040u)|mode);
    for(size_t trial=0;trial<10000;++trial){
      int exp=trial%7==0?-145:int(rng()%108)-54;
      for(auto& x:rows)x=std::ldexp(value(rng),exp);
      for(auto& x:query)x=std::ldexp(value(rng),exp);
      check(rows.data()+trial%8,query.data()+7-trial%8,true);
      if(trial%113==0)check(rows.data(),query.data(),false);
    }
    for(float x:{0.f,-0.f,1e-30f,std::numeric_limits<float>::denorm_min(),1e16f,std::numeric_limits<float>::max()}){
      rows.fill(x);query.fill(-x);check(rows.data(),query.data(),!mvdb::has_extreme(rows.data(),512));
    }
  }
  _mm_setcsr(csr);
  // A quartet and a query end immediately before inaccessible pages.
  auto guarded=[](size_t floats){
    auto page=size_t(::sysconf(_SC_PAGESIZE));assert(floats*sizeof(float)<=page);
    auto p=static_cast<char*>(::mmap(nullptr,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(p!=MAP_FAILED);assert(::mprotect(p+page,page,PROT_NONE)==0);
    return reinterpret_cast<float*>(p+page-floats*sizeof(float));
  };
  auto r=guarded(512),q=guarded(128);std::fill(r,r+512,1.f);std::fill(q,q+128,0.f);check(r,q,true);
  auto page=size_t(::sysconf(_SC_PAGESIZE));::munmap(reinterpret_cast<char*>(r)+2048-page,2*page);::munmap(reinterpret_cast<char*>(q)+512-page,2*page);
  engine_checks();
  std::cout<<"structural SIMD: batch survivors bit-identical, unsafe/tiny/FTZ and guard pages passed\n";
}
