#include "sql.hpp"
#include <cmath>
#include <immintrin.h>
#include <limits>

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
// Work on two adjacent rows at a time so the eight independent accumulators
// and the shared query load fit in AVX2's sixteen registers. Each row retains
// the full kernel's four accumulation chains and reduction order.
#define BATCH_REDUCE(S,OUT) \
  {__m256 merged=_mm256_add_ps(_mm256_add_ps(S[0],S[1]),_mm256_add_ps(S[2],S[3])); \
  __m128 halves=_mm_add_ps(_mm256_castps256_ps128(merged),_mm256_extractf128_ps(merged,1)); \
  halves=_mm_hadd_ps(halves,halves);halves=_mm_hadd_ps(halves,halves); \
  OUT=_mm_cvtss_f32(halves);}
#define BATCH_BODY(ACCUMULATE) \
  for(size_t row=0;row<4;row+=2){ \
    __m256 sums[2][4];for(auto& s:sums)for(auto& x:s)x=_mm256_setzero_ps(); \
    for(size_t i=0;i<128;i+=32)for(size_t j=0;j<4;++j){ \
      __m256 q=_mm256_loadu_ps(query+i+j*8); \
      for(size_t r=0;r<2;++r){ \
        __m256 d=_mm256_sub_ps(_mm256_loadu_ps(rows+(row+r)*128+i+j*8),q); \
        sums[r][j]=ACCUMULATE; \
      } \
    } \
    BATCH_REDUCE(sums[0],out[row]) BATCH_REDUCE(sums[1],out[row+1]) \
  }
__attribute__((target("avx2"))) void simd_batch4(const float* rows,const float* query,double* out){
  BATCH_BODY(_mm256_add_ps(sums[r][j],_mm256_mul_ps(d,d)))
}
__attribute__((target("avx2,fma"))) void simd_batch4_fma(const float* rows,const float* query,double* out){
  BATCH_BODY(_mm256_fmadd_ps(d,d,sums[r][j]))
}
#undef BATCH_BODY
#undef BATCH_REDUCE
// Keep the four original accumulators live across the check, then finish in
// exactly the original order. The checkpoint reduction never feeds back into
// them, so survivors remain bit-identical to simd_squared[_fma](...,128).
#define BOUNDED_REDUCE() \
  __m256 merged=_mm256_add_ps(_mm256_add_ps(sums[0],sums[1]),_mm256_add_ps(sums[2],sums[3])); \
  __m128 halves=_mm_add_ps(_mm256_castps256_ps128(merged),_mm256_extractf128_ps(merged,1)); \
  halves=_mm_hadd_ps(halves,halves);halves=_mm_hadd_ps(halves,halves); \
  double reduced=_mm_cvtss_f32(halves);
#define BOUNDED_BODY(ACCUMULATE) \
  __m256 sums[4]={_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps()}; \
  for(size_t i=0;i<Prefix;i+=32)for(size_t j=0;j<4;++j){ \
    __m256 d=_mm256_sub_ps(_mm256_loadu_ps(a+i+j*8),_mm256_loadu_ps(b+i+j*8)); \
    sums[j]=ACCUMULATE; \
  } \
  {BOUNDED_REDUCE() if(reduced>cutoff&&reduced>1e-20)return {reduced,true};} \
  for(size_t i=Prefix;i<128;i+=32)for(size_t j=0;j<4;++j){ \
    __m256 d=_mm256_sub_ps(_mm256_loadu_ps(a+i+j*8),_mm256_loadu_ps(b+i+j*8)); \
    sums[j]=ACCUMULATE; \
  } \
  BOUNDED_REDUCE() \
  if(!std::isfinite(reduced)||reduced<=1e-20)reduced=l2_squared_exact(a,b,128); \
  return {reduced,false};
template<size_t Prefix>
__attribute__((target("avx2"))) BoundedDistance simd_bounded(const float* a,const float* b,double cutoff){
  BOUNDED_BODY(_mm256_add_ps(sums[j],_mm256_mul_ps(d,d)))
}
template<size_t Prefix>
__attribute__((target("avx2,fma"))) BoundedDistance simd_bounded_fma(const float* a,const float* b,double cutoff){
  BOUNDED_BODY(_mm256_fmadd_ps(d,d,sums[j]))
}
#undef BOUNDED_BODY
#undef BOUNDED_REDUCE
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
double l2_bounded_cutoff(double exact_squared){
  const double infinity=std::numeric_limits<double>::infinity();
  if(!std::isfinite(exact_squared)||exact_squared<0)return infinity;
  // At 64D a lane sums two products and the reduction has five additions.
  // Including subtraction and multiplication, gamma(32), u=2^-24, bounds
  // relative overestimation by <2e-6 under normal round-to-nearest arithmetic.
  // 1e-4 is deliberately looser. DAZ/FTZ's absolute lost-input/product error
  // for 128 safe coordinates is far below 1e-5, covered by this additive term.
  // The safe <=1e16 coordinate bound keeps all intermediates below FLT_MAX.
  double margin=exact_squared*1.0001+1e-5;
  if(!std::isfinite(margin)||margin>=std::numeric_limits<float>::max())return infinity;
  return std::nextafter(static_cast<float>(margin),std::numeric_limits<float>::infinity());
}
BoundedDistance l2_squared_bounded_128(const float* a,const float* b,double cutoff,bool safe,size_t prefix){
  if(!safe||!avx2_available()||(prefix!=32&&prefix!=64))return {l2_squared_exact(a,b,128),false};
  if(!std::isfinite(cutoff)||cutoff<0)return {l2_squared_fast(a,b,128,true),false};
  if(fma_available())return prefix==32?simd_bounded_fma<32>(a,b,cutoff):simd_bounded_fma<64>(a,b,cutoff);
  return prefix==32?simd_bounded<32>(a,b,cutoff):simd_bounded<64>(a,b,cutoff);
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

void l2_squared_batch4_128(const float* rows,const float* query,double* out,bool safe){
  if(!safe||!avx2_available()){
    for(size_t row=0;row<4;++row)out[row]=l2_squared_exact(rows+row*128,query,128);
    return;
  }
  if(fma_available())simd_batch4_fma(rows,query,out);else simd_batch4(rows,query,out);
  for(size_t row=0;row<4;++row)if(!std::isfinite(out[row])||out[row]<=1e-20)
    out[row]=l2_squared_exact(rows+row*128,query,128);
}
double l2(const float* a,const float* b,size_t n){return l2_fast(a,b,n,!has_extreme(a,n)&&!has_extreme(b,n));}
}
