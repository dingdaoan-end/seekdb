#include "../src/sql.hpp"
#include "../src/simd.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>
#include <random>
static double reference(const float* a,const float* b,size_t n){double sum=0;for(size_t i=0;i<n;++i){double d=double(a[i])-b[i];sum+=d*d;}return std::sqrt(sum);}
struct comma_decimal : std::numpunct<char>{char do_decimal_point()const override{return ',';}};
int main(){
  auto previous=std::locale::global(std::locale(std::locale::classic(),new comma_decimal));
  assert(mvdb::json(1.25)=="1.25");
  assert(mvdb::json(std::vector<float>{1.25f,-0.5f})=="[1.25,-0.5]");
  std::locale::global(previous);
  std::mt19937 random(123);std::uniform_real_distribution<float> value(-100,100);
  for(size_t n:{size_t(0),size_t(1),size_t(7),size_t(8),size_t(31),size_t(32),size_t(33),size_t(63),size_t(127),size_t(128),size_t(129),size_t(4096),size_t(4097)}){
    std::vector<float> a(n),b(n);
    for(int trial=0;trial<100;++trial){for(size_t i=0;i<n;++i){a[i]=value(random);b[i]=value(random);}auto expected=reference(a.data(),b.data(),n);
      assert(mvdb::l2_squared_exact(a.data(),b.data(),n)==expected*expected || std::abs(mvdb::l2_squared_exact(a.data(),b.data(),n)-expected*expected)<=expected*expected*1e-14);
      auto squared=mvdb::l2_squared_fast(a.data(),b.data(),n,true);assert(std::abs(std::sqrt(squared)-expected)<=1e-5+expected*1e-5);
      for(double actual:{mvdb::l2(a.data(),b.data(),n),mvdb::l2_fast(a.data(),b.data(),n,true),mvdb::l2_fast(a.data(),b.data(),n,false)})assert(std::abs(actual-expected)<=1e-5+expected*1e-5);
    }
  }
  // Page payloads and query vectors need not share alignment. Exercise every
  // float alignment and both sides of the 128D hot path and four-lane tail.
  for(size_t offset=0;offset<8;++offset)for(size_t n:{size_t(127),size_t(128),size_t(129),size_t(4096)}){
    std::vector<float> a(n+8),b(n+8);
    for(size_t i=0;i<a.size();++i){a[i]=value(random);b[i]=value(random);}
    const auto expected=reference(a.data()+offset,b.data()+7-offset,n);
    auto actual=mvdb::l2(a.data()+offset,b.data()+7-offset,n);
    assert(std::abs(actual-expected)<=1e-5+expected*1e-5);
    assert(mvdb::l2_fast(a.data()+offset,b.data()+7-offset,n,false)==expected);
  }
  for(float x:{1e30f,std::numeric_limits<float>::max(),1e-30f}){std::vector<float>a(128,x),b(128,-x);auto expected=reference(a.data(),b.data(),128),actual=mvdb::l2(a.data(),b.data(),128);assert(std::isfinite(actual));assert(mvdb::l2_squared_fast(a.data(),b.data(),128,false)==mvdb::l2_squared_exact(a.data(),b.data(),128));assert(std::abs(actual-expected)<=expected*1e-12);}
  for(int exponent=-145;exponent<=53;exponent+=3){std::vector<float>a(128),b(128);for(size_t i=0;i<a.size();++i){a[i]=std::ldexp(value(random)/100,exponent);b[i]=i%2?std::nextafter(a[i],0.f):-a[i];}auto expected=reference(a.data(),b.data(),a.size()),actual=mvdb::l2(a.data(),b.data(),a.size());assert(std::abs(actual-expected)<=1e-5+expected*1e-5);}
  // Real four-vector dispatch: distinct candidates, misalignment, tails and
  // widened fallback. Tiny distances must survive float square underflow.
  for(size_t n:{size_t(0),size_t(1),size_t(7),size_t(8),size_t(31),size_t(127),size_t(128),size_t(129),size_t(4096),size_t(4097)}){
    std::vector<float> q(n+8),v[4];const float* pointers[4];
    for(auto& x:q)x=value(random);
    for(size_t j=0;j<4;++j){v[j].resize(n+8);for(auto& x:v[j])x=value(random);pointers[j]=v[j].data()+j;}
    float public_out[4];mvdb::l2_sq_f32_batch4(q.data()+7,pointers[0],pointers[1],pointers[2],pointers[3],n,public_out);
    for(size_t j=0;j<4;++j){auto expected=mvdb::l2_squared_exact(q.data()+7,pointers[j],n);assert(std::abs(public_out[j]-expected)<=1e-5+expected*1e-4);}
    for(bool safe:{false,true}){double actual[4];mvdb::l2_sq_f32_batch4(q.data()+7,pointers,n,actual,safe);
      for(size_t j=0;j<4;++j){auto expected=mvdb::l2_squared_exact(q.data()+7,pointers[j],n);
        if(!safe)assert(actual[j]==expected);else assert(std::abs(actual[j]-expected)<=1e-5+expected*1e-4);}}
  }
  for(float scale:{1e-30f,1e-20f,1.0f,1e16f,1e30f,std::numeric_limits<float>::max()}){
    std::vector<float> q(129,scale),v[4];const float* pointers[4];bool safe=!mvdb::has_extreme(q.data(),q.size());
    for(size_t j=0;j<4;++j){v[j].assign(129,j%2?-scale:std::nextafter(scale,0.f));pointers[j]=v[j].data();safe&=!mvdb::has_extreme(pointers[j],129);}
    double actual[4];mvdb::l2_sq_f32_batch4(q.data(),pointers,129,actual,safe);
    for(size_t j=0;j<4;++j){auto expected=mvdb::l2_squared_exact(q.data(),pointers[j],129);assert(std::isfinite(actual[j]));assert(expected==0?actual[j]==0:std::abs(actual[j]-expected)<=expected*1e-4);}
  }
  // Prefix rejection must be certified against widened float64 distances;
  // survivors retain the exact bits of the original float accumulation kernel.
  for(size_t n:{size_t(0),size_t(7),size_t(31),size_t(32),size_t(33),size_t(63),size_t(64),size_t(65),size_t(127),size_t(128),size_t(129),size_t(4096),size_t(4097)}){
    std::vector<float> q(n+8),v[4];const float* pointers[4];
    for(auto& x:q)x=value(random);
    for(size_t j=0;j<4;++j){v[j].resize(n+8);for(auto& x:v[j])x=value(random);pointers[j]=v[j].data()+j;}
    for(bool safe:{false,true}){
      double full[4];mvdb::l2_sq_f32_batch4(q.data()+7,pointers,n,full,safe);
      for(double threshold:{0.0,1.0,10000.0,1000000.0,std::numeric_limits<double>::infinity(),-1.0}){
        mvdb::L2PruneCounters counters;double actual[4];
        mvdb::l2_sq_f32_batch4_pruned(q.data()+7,pointers,n,threshold,actual,safe,&counters);
        const bool eligible=safe&&mvdb::avx2_available()&&n>=32&&n<=4096&&std::isfinite(threshold)&&threshold>=0;
        assert(counters.trials==(eligible?4:0));assert(counters.pruned32+counters.pruned64<=counters.trials);
        for(size_t j=0;j<4;++j){
          const double exact=mvdb::l2_squared_exact(q.data()+7,pointers[j],n);
          assert(actual[j]==full[j]||(actual[j]>threshold&&actual[j]<=exact));
          mvdb::L2PruneCounters one;
          auto scalar=mvdb::l2_sq_f32_pruned(q.data()+7,pointers[j],n,threshold,safe,&one);
          const auto fast=mvdb::l2_squared_fast(q.data()+7,pointers[j],n,safe);
          assert(one.trials==(eligible?1:0));
          assert(scalar==fast||(scalar>threshold&&scalar<=exact));
        }
      }
      // Exact ties and nextafter thresholds must not cause a rejection, even
      // when the rounded prefix slightly exceeds the float64 reference.
      for(size_t j=0;j<4;++j){
        auto exact=mvdb::l2_squared_exact(q.data()+7,pointers[j],n);
        for(auto threshold:{exact,std::nextafter(exact,0.0),std::nextafter(exact,std::numeric_limits<double>::infinity())}){
          mvdb::L2PruneCounters counters;
          auto actual=mvdb::l2_sq_f32_pruned(q.data()+7,pointers[j],n,threshold,safe,&counters);
          assert(counters.pruned32+counters.pruned64==0);
          assert(actual==mvdb::l2_squared_fast(q.data()+7,pointers[j],n,safe));
        }
      }
    }
  }
  // Mixed batch: first coordinate block rejects one candidate; the second
  // rejects another. A tie and a tail-only contribution survive unchanged.
  {
    std::vector<float> q(129,0),v[4];const float* pointers[4];
    for(size_t j=0;j<4;++j){v[j].assign(129,0);pointers[j]=v[j].data();}
    for(size_t i=0;i<32;++i){v[0][i]=2;v[1][32+i]=2;v[2][i]=1;}
    v[3][128]=2;
    double full[4],actual[4];mvdb::L2PruneCounters counters;
    mvdb::l2_sq_f32_batch4(q.data(),pointers,129,full,true);
    mvdb::l2_sq_f32_batch4_pruned(q.data(),pointers,129,32,actual,true,&counters);
    if(mvdb::avx2_available())assert(counters.trials==4&&counters.pruned32==1&&counters.pruned64==1);
    assert(actual[2]==full[2]&&actual[3]==full[3]);
    for(size_t j=0;j<4;++j){
      mvdb::L2PruneCounters one;
      const auto scalar=mvdb::l2_sq_f32_pruned(q.data(),pointers[j],129,32,true,&one);
      if(mvdb::avx2_available()){
        assert(one.trials==1);
        assert(one.pruned32==(j==0?1:0));assert(one.pruned64==(j==1?1:0));
      }
      if(j>=2)assert(scalar==mvdb::l2_squared_fast(q.data(),pointers[j],129,true));
    }
    for(size_t j=0;j<2;++j){
      assert(actual[j]>32&&actual[j]<=mvdb::l2_squared_exact(q.data(),pointers[j],129));
      if(mvdb::avx2_available())assert(actual[j]==std::nextafter(32.0,std::numeric_limits<double>::infinity()));
    }
    // Zero, subnormal and maximum thresholds exercise the sentinel's bit
    // increment and the upward-rounded limit's overflow handling.
    for(auto threshold:{0.0,-0.0,std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max()}){
      mvdb::L2PruneCounters one;
      const auto scalar=mvdb::l2_sq_f32_pruned(q.data(),pointers[0],129,threshold,true,&one);
      if(threshold==std::numeric_limits<double>::max())assert(scalar==full[0]&&one.pruned32+one.pruned64==0);
      else assert(scalar>threshold&&scalar<=full[0]);
    }
  }
  // Cancellation, exponent ranges, tiny/overflow fallback, and thresholds
  // adjacent to the exact prefix expose unsafe fixed float error margins.
  for(float scale:{std::numeric_limits<float>::denorm_min(),1e-30f,1e-20f,1.f,1e16f,1e30f,std::numeric_limits<float>::max()}){
    for(size_t n:{size_t(32),size_t(64),size_t(129),size_t(4096)}){
      std::vector<float> q(n,scale),v[4];const float* pointers[4];
      bool safe=!mvdb::has_extreme(q.data(),n);
      for(size_t j=0;j<4;++j){v[j].assign(n,j%2?-scale:std::nextafter(scale,0.f));pointers[j]=v[j].data();safe&=!mvdb::has_extreme(pointers[j],n);}
      for(size_t j=0;j<4;++j){
        const auto exact=mvdb::l2_squared_exact(q.data(),pointers[j],n);
        const auto fast=mvdb::l2_squared_fast(q.data(),pointers[j],n,safe);
        for(auto threshold:{0.0,exact,std::nextafter(exact,0.0)}){
          mvdb::L2PruneCounters counters;
          auto actual=mvdb::l2_sq_f32_pruned(q.data(),pointers[j],n,threshold,safe,&counters);
          assert(std::isfinite(actual));
          assert(actual==fast||(actual>threshold&&actual<=exact));
          if(threshold==exact)assert(counters.pruned32+counters.pruned64==0);
          double batch[4];mvdb::l2_sq_f32_batch4_pruned(q.data(),pointers,n,threshold,batch,safe);
          double full[4];mvdb::l2_sq_f32_batch4(q.data(),pointers,n,full,safe);
          for(size_t k=0;k<4;++k)assert(batch[k]==full[k]||(batch[k]>threshold&&batch[k]<=mvdb::l2_squared_exact(q.data(),pointers[k],n)));
        }
      }
    }
  }
  // Search many rounding-sensitive prefixes with a zero tail. The threshold
  // lies just below each exact result: a rounded-up SIMD sum must not prune.
  for(size_t n:{size_t(32),size_t(64)})for(int trial=0;trial<1000;++trial){
    std::vector<float> q(n),v[4];const float* pointers[4];
    for(auto& x:q)x=value(random);
    for(size_t j=0;j<4;++j){v[j].resize(n);for(size_t i=0;i<n;++i)v[j][i]=(trial%2)?std::nextafter(q[i],0.f):value(random);pointers[j]=v[j].data();}
    for(size_t j=0;j<4;++j){
      const auto exact=mvdb::l2_squared_exact(q.data(),pointers[j],n);
      const auto threshold=std::nextafter(exact,0.0);
      mvdb::L2PruneCounters counters;
      auto actual=mvdb::l2_sq_f32_pruned(q.data(),pointers[j],n,threshold,true,&counters);
      assert(counters.pruned32+counters.pruned64==0);
      assert(actual==mvdb::l2_squared_fast(q.data(),pointers[j],n,true));
      double full[4],batch[4];mvdb::l2_sq_f32_batch4(q.data(),pointers,n,full,true);
      mvdb::l2_sq_f32_batch4_pruned(q.data(),pointers,n,threshold,batch,true);
      assert(batch[j]==full[j]);
    }
  }
  std::cout<<"SIMD dispatch, certified prefix pruning, ties, tails, extremes and tiny values passed\n";
}
