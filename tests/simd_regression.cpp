#include "../src/sql.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
static double reference(const float* a,const float* b,size_t n){double sum=0;for(size_t i=0;i<n;++i){double d=double(a[i])-b[i];sum+=d*d;}return std::sqrt(sum);}
int main(){
  std::mt19937 random(123);std::uniform_real_distribution<float> value(-100,100);
  for(size_t n:{size_t(0),size_t(1),size_t(7),size_t(8),size_t(31),size_t(32),size_t(128),size_t(4096),size_t(4097)}){
    std::vector<float> a(n),b(n);
    for(int trial=0;trial<100;++trial){for(size_t i=0;i<n;++i){a[i]=value(random);b[i]=value(random);}auto expected=reference(a.data(),b.data(),n);
      for(double actual:{mvdb::l2(a.data(),b.data(),n),mvdb::l2_fast(a.data(),b.data(),n,true),mvdb::l2_fast(a.data(),b.data(),n,false)})assert(std::abs(actual-expected)<=1e-5+expected*1e-5);
    }
  }
  for(float x:{1e30f,std::numeric_limits<float>::max(),1e-30f}){std::vector<float>a(128,x),b(128,-x);auto expected=reference(a.data(),b.data(),128),actual=mvdb::l2(a.data(),b.data(),128);assert(std::isfinite(actual));assert(std::abs(actual-expected)<=expected*1e-12);}
  for(int exponent=-145;exponent<=53;exponent+=3){std::vector<float>a(128),b(128);for(size_t i=0;i<a.size();++i){a[i]=std::ldexp(value(random)/100,exponent);b[i]=i%2?std::nextafter(a[i],0.f):-a[i];}auto expected=reference(a.data(),b.data(),a.size()),actual=mvdb::l2(a.data(),b.data(),a.size());assert(std::abs(actual-expected)<=1e-5+expected*1e-5);}
  std::cout<<"SIMD scalar/AVX2/FMA dispatch, tails, extremes and tiny values passed\n";
}
