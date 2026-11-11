#include "sql.hpp"
#include <cmath>
#include <immintrin.h>

namespace mvdb {
namespace {
// Bound float accumulation to 128 terms per lane at the supported 4096D limit.
// Keep FMA in its own target function: AVX2 does not imply FMA support.
#define L2_BODY(ACCUMULATE) \
  __m256 sums[4]={_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps()}; \
  size_t i=0; \
  for(;i+32<=n;i+=32)for(size_t j=0;j<4;++j){ \
    __m256 d=_mm256_sub_ps(_mm256_loadu_ps(a+i+j*8),_mm256_loadu_ps(b+i+j*8)); \
    sums[j]=ACCUMULATE; \
  } \
  for(;i+8<=n;i+=8){ \
    __m256 d=_mm256_sub_ps(_mm256_loadu_ps(a+i),_mm256_loadu_ps(b+i)); \
    size_t j=(i/8)%4;sums[j]=ACCUMULATE; \
  } \
  __m256 merged=_mm256_add_ps(_mm256_add_ps(sums[0],sums[1]),_mm256_add_ps(sums[2],sums[3])); \
  __m128 halves=_mm_add_ps(_mm256_castps256_ps128(merged),_mm256_extractf128_ps(merged,1)); \
  halves=_mm_hadd_ps(halves,halves);halves=_mm_hadd_ps(halves,halves); \
  double result=_mm_cvtss_f32(halves); \
  for(;i<n;++i){double d=static_cast<double>(a[i])-b[i];result+=d*d;} \
  return result;
__attribute__((target("avx2"))) double simd_squared(const float* a,const float* b,size_t n) {
  L2_BODY(_mm256_add_ps(sums[j],_mm256_mul_ps(d,d)))
}
__attribute__((target("avx2,fma"))) double simd_squared_fma(const float* a,const float* b,size_t n) {
  L2_BODY(_mm256_fmadd_ps(d,d,sums[j]))
}
#undef L2_BODY
__attribute__((target("avx2"))) bool simd_extreme(const float* a,size_t n) {
  const auto sign=_mm256_set1_ps(-0.0f),limit=_mm256_set1_ps(1e16f);size_t i=0;
  for(;i+8<=n;i+=8){auto x=_mm256_andnot_ps(sign,_mm256_loadu_ps(a+i));
    if(_mm256_movemask_ps(_mm256_cmp_ps(x,limit,_CMP_NLE_UQ)))return true;}
  for(;i<n;++i)if(!std::isfinite(a[i])||std::abs(a[i])>1e16f)return true;
  return false;
}
}
bool avx2_available(){static const bool available=[] {__builtin_cpu_init();return bool(__builtin_cpu_supports("avx2"));}();return available;}
bool fma_available(){static const bool available=[] {__builtin_cpu_init();return bool(__builtin_cpu_supports("fma"));}();return available;}
bool has_extreme(const float* a,size_t n){
  if(n>4096)return true;
  if(avx2_available())return simd_extreme(a,n);
  for(size_t i=0;i<n;++i)if(!std::isfinite(a[i])||std::abs(a[i])>1e16f)return true;
  return false;
}
double l2_squared_exact(const float* a,const float* b,size_t n){
  double sum=0;for(size_t i=0;i<n;++i){double d=static_cast<double>(a[i])-b[i];sum+=d*d;}return sum;
}
double l2_exact(const float* a,const float* b,size_t n){return std::sqrt(l2_squared_exact(a,b,n));}
double l2_squared_fast(const float* a,const float* b,size_t n,bool safe){
  if(!safe||n>4096||!avx2_available())return l2_squared_exact(a,b,n);
  double sum=fma_available()?simd_squared_fma(a,b,n):simd_squared(a,b,n);
  // Recompute tiny sums to avoid float square underflow (including FTZ mode).
  if(!std::isfinite(sum)||sum<=1e-20)return l2_squared_exact(a,b,n);
  return sum;
}
double l2_fast(const float* a,const float* b,size_t n,bool safe){return std::sqrt(l2_squared_fast(a,b,n,safe));}
double l2(const float* a,const float* b,size_t n){return l2_fast(a,b,n,!has_extreme(a,n)&&!has_extreme(b,n));}
}
