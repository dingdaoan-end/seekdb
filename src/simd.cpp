#include "sql.hpp"
#include "simd.hpp"
#include <cmath>
#include <bit>
#include <limits>
#include <immintrin.h>

namespace mvdb {
namespace {
// Bound float accumulation to 128 terms per lane at the supported 4096D limit.
// Keep FMA in its own target function: AVX2 does not imply FMA support.
#define L2_BODY(ACCUMULATE,CHECK) \
  __m256 sums[4]={_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps()}; \
  size_t i=0; \
  for(;i+32<=n;i+=32){for(size_t j=0;j<4;++j){ \
    __m256 d=_mm256_sub_ps(_mm256_loadu_ps(a+i+j*8),_mm256_loadu_ps(b+i+j*8)); \
    sums[j]=ACCUMULATE; \
  } CHECK; } \
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
  L2_BODY(_mm256_add_ps(sums[j],_mm256_mul_ps(d,d)),)
}
__attribute__((target("avx2,fma"))) double simd_squared_fma(const float* a,const float* b,size_t n) {
  L2_BODY(_mm256_fmadd_ps(d,d,sums[j]),)
}
// Prefix sums use the same positive float operations as the full kernels.
// A deliberately loose gamma bound covers subtraction, squaring, accumulation
// and reduction (including non-FMA dispatch). The absolute allowance covers
// subnormal/FTZ/DAZ effects. Inflate the threshold ONCE per kernel; prefix
// checks themselves need only a comparison, with no division or libm call.
double next_positive(double value){
  if(value==std::numeric_limits<double>::infinity())return value;
  return std::bit_cast<double>(std::bit_cast<uint64_t>(value==0?0.0:value)+1);
}
struct PruneLimits {double at32,at64,rejected;};
double prefix_limit(double threshold,size_t count){
  const double error=8.0*(count+8)*std::numeric_limits<float>::epsilon();
  // Round each positive operation upward, so this is at least the exact
  // (threshold + absolute_allowance) * (1 + relative_error). In particular,
  // a rounded-down double add/multiply cannot accidentally enable a rejection.
  return next_positive(next_positive(threshold+count*1e-30)*(1.0+error));
}
PruneLimits prune_limits(double threshold){
  return {prefix_limit(threshold,32),prefix_limit(threshold,64),next_positive(threshold)};
}
bool prune_prefix(double partial,size_t count,const PruneLimits& limits,
                  L2PruneCounters* counters,double& bound){
  if(!(partial>(count==32?limits.at32:limits.at64))||!std::isfinite(partial))return false;
  // The actual distance is strictly above threshold. Its next representable
  // double is therefore a certified bound, not the candidate's full distance.
  bound=limits.rejected;
  if(counters){if(count==32)++counters->pruned32;else ++counters->pruned64;}
  return true;
}
__attribute__((target("avx2"))) double reduce_sum(__m256 sum){
  auto halves=_mm_add_ps(_mm256_castps256_ps128(sum),_mm256_extractf128_ps(sum,1));
  halves=_mm_hadd_ps(halves,halves);halves=_mm_hadd_ps(halves,halves);
  return _mm_cvtss_f32(halves);
}
#define SINGLE_CHECK \
  if(i==0||i==32){double bound; \
    auto merged=_mm256_add_ps(_mm256_add_ps(sums[0],sums[1]),_mm256_add_ps(sums[2],sums[3])); \
    if(prune_prefix(reduce_sum(merged),i+32,limits,counters,bound))return bound;}
__attribute__((target("avx2"))) double simd_pruned(const float* a,const float* b,size_t n,double threshold,L2PruneCounters* counters){
  const auto limits=prune_limits(threshold);
  L2_BODY(_mm256_add_ps(sums[j],_mm256_mul_ps(d,d)),SINGLE_CHECK)
}
__attribute__((target("avx2,fma"))) double simd_pruned_fma(const float* a,const float* b,size_t n,double threshold,L2PruneCounters* counters){
  const auto limits=prune_limits(threshold);
  L2_BODY(_mm256_fmadd_ps(d,d,sums[j]),SINGLE_CHECK)
}
#undef SINGLE_CHECK
#undef L2_BODY
// Four candidates share each query load, with independent SIMD accumulators.
#define BATCH4_BODY(ACCUMULATE,ACTIVE,CHECK) \
  __m256 sums[4]={_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps()}; \
  __m256 other[4]={_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps(),_mm256_setzero_ps()}; \
  size_t i=0; \
  for(;i+16<=n;i+=16){ \
    auto q=_mm256_loadu_ps(query+i); \
    for(size_t j=0;j<4;++j)if(ACTIVE){auto d=_mm256_sub_ps(q,_mm256_loadu_ps(vectors[j]+i));auto& sum=sums[j];sum=ACCUMULATE;} \
    q=_mm256_loadu_ps(query+i+8); \
    for(size_t j=0;j<4;++j)if(ACTIVE){auto d=_mm256_sub_ps(q,_mm256_loadu_ps(vectors[j]+i+8));auto& sum=other[j];sum=ACCUMULATE;} \
    CHECK; \
  } \
  for(;i+8<=n;i+=8){auto q=_mm256_loadu_ps(query+i); \
    for(size_t j=0;j<4;++j)if(ACTIVE){auto d=_mm256_sub_ps(q,_mm256_loadu_ps(vectors[j]+i));auto& sum=sums[j];sum=ACCUMULATE;}} \
  for(size_t j=0;j<4;++j)if(ACTIVE){ \
    auto merged=_mm256_add_ps(sums[j],other[j]); \
    auto halves=_mm_add_ps(_mm256_castps256_ps128(merged),_mm256_extractf128_ps(merged,1)); \
    halves=_mm_hadd_ps(halves,halves);halves=_mm_hadd_ps(halves,halves);output[j]=_mm_cvtss_f32(halves); \
    for(size_t k=i;k<n;++k){double d=double(query[k])-vectors[j][k];output[j]+=d*d;} }
__attribute__((target("avx2"))) void simd_batch4(const float* query,const float* const vectors[4],size_t n,double output[4]){
  BATCH4_BODY(_mm256_add_ps(sum,_mm256_mul_ps(d,d)),true,)
}
__attribute__((target("avx2,fma"))) void simd_batch4_fma(const float* query,const float* const vectors[4],size_t n,double output[4]){
  BATCH4_BODY(_mm256_fmadd_ps(d,d,sum),true,)
}
// The float cutoff is rounded upward from the conservative double limit.
// Comparing four reduced prefixes together avoids four double reductions and
// scalar certification branches. Safe vectors cannot overflow their sums.
float prefix_float_limit(double limit){
  if(limit>=std::numeric_limits<float>::max())return std::numeric_limits<float>::infinity();
  float rounded=static_cast<float>(limit);
  if(double(rounded)<limit)rounded=std::bit_cast<float>(std::bit_cast<uint32_t>(rounded)+1);
  return rounded;
}
#define BATCH_CHECK \
  if(i+16==32||i+16==64){ \
    auto pair01=_mm256_hadd_ps(_mm256_add_ps(sums[0],other[0]),_mm256_add_ps(sums[1],other[1])); \
    auto pair23=_mm256_hadd_ps(_mm256_add_ps(sums[2],other[2]),_mm256_add_ps(sums[3],other[3])); \
    auto pairs=_mm256_hadd_ps(pair01,pair23); \
    auto partials=_mm_add_ps(_mm256_castps256_ps128(pairs),_mm256_extractf128_ps(pairs,1)); \
    auto cutoff=_mm_set1_ps(i+16==32?cut32:cut64); \
    unsigned rejected=unsigned(_mm_movemask_ps(_mm_cmpgt_ps(partials,cutoff)))&active; \
    if(rejected){ \
      if(counters){auto count=uint64_t(__builtin_popcount(rejected));if(i+16==32)counters->pruned32+=count;else counters->pruned64+=count;} \
      for(size_t j=0;j<4;++j)if(rejected&(1U<<j))output[j]=limits.rejected; \
      active&=~rejected;if(!active)return; \
    } \
  }
__attribute__((target("avx2"))) void simd_batch4_pruned(const float* query,const float* const vectors[4],size_t n,double threshold,double output[4],L2PruneCounters* counters){
  const auto limits=prune_limits(threshold);
  const float cut32=prefix_float_limit(limits.at32),cut64=prefix_float_limit(limits.at64);
  unsigned active=15;
  BATCH4_BODY(_mm256_add_ps(sum,_mm256_mul_ps(d,d)),active&(1U<<j),BATCH_CHECK)
}
__attribute__((target("avx2,fma"))) void simd_batch4_pruned_fma(const float* query,const float* const vectors[4],size_t n,double threshold,double output[4],L2PruneCounters* counters){
  const auto limits=prune_limits(threshold);
  const float cut32=prefix_float_limit(limits.at32),cut64=prefix_float_limit(limits.at64);
  unsigned active=15;
  BATCH4_BODY(_mm256_fmadd_ps(d,d,sum),active&(1U<<j),BATCH_CHECK)
}
#undef BATCH_CHECK
#undef BATCH4_BODY
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
void l2_sq_f32_batch4(const float* query,const float* const vectors[4],size_t n,double output[4],bool safe){
  if(!safe||n>4096||!avx2_available()){
    for(size_t j=0;j<4;++j)output[j]=l2_squared_exact(query,vectors[j],n);
    return;
  }
  if(fma_available())simd_batch4_fma(query,vectors,n,output);
  else simd_batch4(query,vectors,n,output);
  for(size_t j=0;j<4;++j)if(!std::isfinite(output[j])||output[j]<=1e-20)
    output[j]=l2_squared_exact(query,vectors[j],n);
}
double l2_sq_f32_pruned(const float* a,const float* b,size_t n,double threshold,bool safe,L2PruneCounters* counters){
  if(!safe||n<32||n>4096||!std::isfinite(threshold)||threshold<0||!avx2_available())
    return l2_squared_fast(a,b,n,safe);
  if(counters)++counters->trials;
  double sum=fma_available()?simd_pruned_fma(a,b,n,threshold,counters):simd_pruned(a,b,n,threshold,counters);
  if(!std::isfinite(sum)||sum<=1e-20)return l2_squared_exact(a,b,n);
  return sum;
}
void l2_sq_f32_batch4_pruned(const float* query,const float* const vectors[4],size_t n,double threshold,double output[4],bool safe,L2PruneCounters* counters){
  if(!safe||n<32||n>4096||!std::isfinite(threshold)||threshold<0||!avx2_available()){
    l2_sq_f32_batch4(query,vectors,n,output,safe);return;
  }
  if(counters)counters->trials+=4;
  if(fma_available())simd_batch4_pruned_fma(query,vectors,n,threshold,output,counters);
  else simd_batch4_pruned(query,vectors,n,threshold,output,counters);
  for(size_t j=0;j<4;++j)if(!std::isfinite(output[j])||output[j]<=1e-20)
    output[j]=l2_squared_exact(query,vectors[j],n);
}
void l2_sq_f32_batch4(const float* query,const float* v0,const float* v1,const float* v2,const float* v3,size_t dim,float out[4]){
  const float* vectors[4]={v0,v1,v2,v3};double wide[4];bool safe=!has_extreme(query,dim);
  for(auto p:vectors)safe&=!has_extreme(p,dim);
  l2_sq_f32_batch4(query,vectors,dim,wide,safe);
  for(size_t j=0;j<4;++j)out[j]=static_cast<float>(wide[j]);
}
double l2_fast(const float* a,const float* b,size_t n,bool safe){return std::sqrt(l2_squared_fast(a,b,n,safe));}
double l2(const float* a,const float* b,size_t n){return l2_fast(a,b,n,!has_extreme(a,n)&&!has_extreme(b,n));}
}
