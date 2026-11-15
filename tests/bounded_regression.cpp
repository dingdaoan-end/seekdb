#include "../src/sql.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <immintrin.h>
#include <iostream>
#include <limits>
#include <random>

using Vector=std::array<float,128>;
static uint64_t comparisons=0,pruned=0;
static void check(const float* a,const float* b,double worst,bool safe,size_t prefix){
  double exact=mvdb::l2_squared_exact(a,b,128);
  auto bounded=mvdb::l2_squared_bounded_128(a,b,mvdb::l2_bounded_cutoff(worst),safe,prefix);
  ++comparisons;
  if(bounded.pruned){++pruned;assert(exact>worst);}
  else {
    double old=mvdb::l2_squared_fast(a,b,128,safe);
    assert(std::bit_cast<uint64_t>(old)==std::bit_cast<uint64_t>(bounded.squared));
  }
}
int main(){
  Vector a{},b{};a[0]=10;
  for(size_t prefix:{32u,64u}){
    auto r=mvdb::l2_squared_bounded_128(a.data(),b.data(),mvdb::l2_bounded_cutoff(1),true,prefix);
    assert(r.pruned==mvdb::avx2_available());
    auto scalar=mvdb::l2_squared_bounded_128(a.data(),b.data(),1,false,prefix);
    assert(!scalar.pruned&&scalar.squared==100);
    check(a.data(),b.data(),100,true,prefix); // tie must survive
    check(a.data(),b.data(),std::nextafter(100.,0.),true,prefix);
    check(a.data(),b.data(),std::nextafter(100.,INFINITY),true,prefix);
    for(double cutoff:{-1.,double(INFINITY),std::numeric_limits<double>::quiet_NaN()}){
      auto disabled=mvdb::l2_squared_bounded_128(a.data(),b.data(),cutoff,true,prefix);
      assert(!disabled.pruned&&disabled.squared==100);
    }
  }
  assert(std::isinf(mvdb::l2_bounded_cutoff(std::numeric_limits<double>::max())));
  assert(std::isinf(mvdb::l2_bounded_cutoff(-1)));
  assert(std::isinf(mvdb::l2_bounded_cutoff(std::numeric_limits<double>::quiet_NaN())));
  for(float v:{std::numeric_limits<float>::max(),1e30f,1e-30f,std::numeric_limits<float>::denorm_min()}){
    a.fill(v);b.fill(-v);
    for(size_t prefix:{32u,64u})check(a.data(),b.data(),1,!mvdb::has_extreme(a.data(),128)&&!mvdb::has_extreme(b.data(),128),prefix);
  }
  auto invalid=mvdb::l2_squared_bounded_128(a.data(),b.data(),0,true,17);
  assert(!invalid.pruned&&invalid.squared==mvdb::l2_squared_exact(a.data(),b.data(),128));

  std::mt19937 rng(812231);std::uniform_real_distribution<float> values(-1,1);
  unsigned csr=_mm_getcsr();
  for(unsigned mode:{0u,0x8040u}){
    _mm_setcsr((csr&~0x8040u)|mode); // DAZ and FTZ, on and off
    for(unsigned trial=0;trial<100000;++trial){
      int exponent=trial%7==0?-145:int(rng()%108)-54;
      for(size_t i=0;i<128;++i){
        a[i]=std::ldexp(values(rng),exponent);
        b[i]=trial%3?std::ldexp(values(rng),exponent):std::nextafter(a[i],0.f);
        if(trial%11==0&&i>=32)a[i]=b[i]; // adversarial all energy in prefix
      }
      double exact=mvdb::l2_squared_exact(a.data(),b.data(),128);
      double worst=trial%4==0?exact:(trial%4==1?std::nextafter(exact,0.):(trial%4==2?exact*.01:exact*1.01));
      bool safe=!mvdb::has_extreme(a.data(),128)&&!mvdb::has_extreme(b.data(),128);
      for(size_t prefix:{32u,64u})check(a.data(),b.data(),worst,safe,prefix);
      if(trial%1000==0)for(size_t prefix:{32u,64u})check(a.data(),b.data(),worst,false,prefix);
    }
    // Unaligned operands, signed zeros and threshold crossings.
    std::array<float,136> x{},y{};
    for(size_t offset=0;offset<8;++offset){
      for(size_t i=0;i<x.size();++i){x[i]=values(rng);y[i]=values(rng);}
      for(size_t prefix:{32u,64u})check(x.data()+offset,y.data()+7-offset,1,true,prefix);
    }
    a.fill(-0.f);b.fill(0.f);for(size_t prefix:{32u,64u})check(a.data(),b.data(),0,true,prefix);
  }
  _mm_setcsr(csr);
  assert(!mvdb::avx2_available()||pruned>1000);
  extern void bounded_engine_regression();
  bounded_engine_regression();
  std::cout<<"bounded regression: "<<comparisons<<" comparisons, "<<pruned<<" proven rejects; heap ties, survivors, extremes, DAZ/FTZ passed\n";
}
