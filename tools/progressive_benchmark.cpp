// Standalone diagnostic: modeled candidate traversal, never an Engine benchmark.
#include "sql.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
using namespace mvdb;
namespace {
constexpr size_t D=128,K=10,L=64,P=5;
struct Fixture {uint32_t n,nq,dim,seed; std::vector<float>x,q; std::vector<uint32_t>cat,qcat;};
template<class T>void read(std::ifstream& f,std::vector<T>& v,size_t n){v.resize(n);f.read(reinterpret_cast<char*>(v.data()),n*sizeof(T));if(!f)throw std::runtime_error("short fixture");}
Fixture load(const char* path){std::ifstream f(path,std::ios::binary);Fixture a;uint32_t header[4]{};f.read(reinterpret_cast<char*>(header),16);a.n=header[0];a.nq=header[1];a.dim=header[2];a.seed=header[3];if(a.n!=10000||a.nq!=1000||a.dim!=128)throw std::runtime_error("expected 10000x128/1000 fixture");read(f,a.x,a.n*D);read(f,a.cat,a.n);read(f,a.q,a.nq*D);read(f,a.qcat,a.nq);return a;}
struct Neighbor {double squared;uint32_t id;};
bool less(const Neighbor&a,const Neighbor&b){if(a.squared!=b.squared){if(std::abs(a.squared-b.squared)<=4*std::numeric_limits<double>::epsilon()*std::max(a.squared,b.squared)){auto x=std::sqrt(a.squared),y=std::sqrt(b.squared);if(x!=y)return x<y;}else return a.squared<b.squared;}return a.id<b.id;}
void push(std::vector<Neighbor>& h,Neighbor v){if(h.size()<K){h.push_back(v);std::push_heap(h.begin(),h.end(),less);}else if(less(v,h.front())){std::pop_heap(h.begin(),h.end(),less);h.back()=v;std::push_heap(h.begin(),h.end(),less);}}
using Candidates=std::vector<std::vector<uint32_t>>;
Candidates candidates(const Fixture&a,bool ivf){
  std::vector<std::vector<uint32_t>>lists(L);std::vector<float> centers(L*D);
  if(ivf){
    // Deterministic data-spaced initialization, six Lloyd iterations; model only.
    for(size_t j=0;j<L;++j)std::copy_n(a.x.data()+(j*a.n/L)*D,D,centers.data()+j*D);
    for(int iteration=0;iteration<6;++iteration){std::vector<double>sums(L*D);std::array<size_t,L>counts{};for(auto&v:lists)v.clear();
      for(uint32_t i=0;i<a.n;++i){double best=INFINITY;size_t winner=0;for(size_t j=0;j<L;++j){double d=l2_squared_fast(a.x.data()+i*D,centers.data()+j*D,D,true);if(d<best){best=d;winner=j;}}lists[winner].push_back(i);++counts[winner];for(size_t k=0;k<D;++k)sums[winner*D+k]+=a.x[i*D+k];}
      if(iteration!=5)for(size_t j=0;j<L;++j)if(counts[j])for(size_t k=0;k<D;++k)centers[j*D+k]=static_cast<float>(sums[j*D+k]/counts[j]);
    }
  }
  Candidates result(a.nq);
  for(size_t qi=0;qi<a.nq;++qi){auto append=[&](uint32_t i){if(qi<a.nq/2||a.cat[i]==a.qcat[qi])result[qi].push_back(i);};
    if(!ivf){for(uint32_t i=0;i<a.n;++i)append(i);}else{std::array<std::pair<double,size_t>,L> rank;for(size_t j=0;j<L;++j)rank[j]={l2_squared_fast(a.q.data()+qi*D,centers.data()+j*D,D,true),j};std::sort(rank.begin(),rank.end());for(size_t j=0;j<P;++j)for(auto i:lists[rank[j].second])append(i);}
  }return result;
}
struct Stats {uint64_t candidates=0,eligible=0,early=0,full_reject=0,exact=0,checksum=0;std::array<uint64_t,8> group{};};
using Answers=std::vector<std::vector<Neighbor>>;
Answers run(const Fixture&a,const Candidates& c,size_t prefix,bool oracle,Stats* stats=nullptr,bool groups=false){Answers result;result.reserve(a.nq);
  for(size_t qi=0;qi<a.nq;++qi){std::vector<Neighbor>h;h.reserve(K);const float*q=a.q.data()+qi*D;
    for(uint32_t i:c[qi]){const float*x=a.x.data()+i*D;if(stats)++stats->candidates;
      if(!oracle&&h.size()==K){double cutoff=l2_bounded_cutoff(h.front().squared);if(stats)++stats->eligible;
        if(groups&&stats){double partial=0;for(size_t g=0;g<8;++g){for(size_t k=g*16;k<(g+1)*16;++k){double d=double(x[k])-q[k];partial+=d*d;}if(partial>cutoff)++stats->group[g];}}
        double fast;
        if(prefix){auto bounded=l2_squared_bounded_128(x,q,cutoff,true,prefix);if(bounded.pruned){if(stats)++stats->early;continue;}fast=bounded.squared;}
        else fast=l2_squared_fast(x,q,D,true);
        if(fast>cutoff){if(stats)++stats->full_reject;continue;}
      }
      if(stats)++stats->exact;
      push(h,{l2_squared_exact(x,q,D),i+1});
    }std::sort(h.begin(),h.end(),less);result.push_back(std::move(h));
  }return result;
}
uint64_t verify(const Answers&a,const Answers&b){if(a.size()!=b.size())throw std::runtime_error("query mismatch");uint64_t checksum=1469598103934665603ULL;for(size_t i=0;i<a.size();++i){if(a[i].size()!=b[i].size())throw std::runtime_error("topk size mismatch");for(size_t j=0;j<a[i].size();++j){if(a[i][j].id!=b[i][j].id||a[i][j].squared!=b[i][j].squared)throw std::runtime_error("double/topk mismatch");checksum=(checksum^b[i][j].id)*1099511628211ULL;}}return checksum;}
void rotation(const Fixture&a){
  std::vector<float> matrix(D*D),output(a.n*D);const float scale=1/std::sqrt(float(D));
  for(unsigned i=0;i<D;++i)for(unsigned j=0;j<D;++j)matrix[i*D+j]=(std::popcount(i&j)&1)?-scale:scale;
  double orthogonal_error=0;for(size_t i=0;i<D;++i)for(size_t j=0;j<D;++j){double dot=0;for(size_t k=0;k<D;++k)dot+=double(matrix[i*D+k])*matrix[j*D+k];orthogonal_error=std::max(orthogonal_error,std::abs(dot-(i==j?1.:0.)));}
  for(int repeat=0;repeat<3;++repeat){auto start=std::clock();for(size_t i=0;i<a.n;++i)for(size_t row=0;row<D;++row){float sum=0;for(size_t k=0;k<D;++k)sum+=matrix[row*D+k]*a.x[i*D+k];output[i*D+row]=sum;}double ms=1000.*(std::clock()-start)/CLOCKS_PER_SEC;double checksum=0,energy=0,inputenergy=0;for(size_t j=0;j<output.size();++j){checksum+=output[j]*(1+(j%31));energy+=double(output[j])*output[j];inputenergy+=double(a.x[j])*a.x[j];}if(std::abs(energy/inputenergy-1)>1e-5)throw std::runtime_error("rotation energy mismatch");std::cout<<"{\"type\":\"dense_rotation\",\"seed\":"<<a.seed<<",\"repeat\":"<<repeat<<",\"cpu_ms\":"<<ms<<",\"terms\":163840000,\"arithmetic_flops\":327680000,\"matrix_bytes\":65536,\"checksum\":"<<checksum<<",\"orthogonal_max_error\":"<<orthogonal_error<<",\"energy_relative_error\":"<<std::abs(energy/inputenergy-1)<<"}\n";}
}
void ties(){Fixture a{};a.n=20;a.nq=1;a.dim=128;a.x.assign(20*D,0);a.q.assign(D,0);a.cat.assign(20,0);a.qcat.assign(1,0);for(size_t i=0;i<20;++i)a.x[i*D]=(i%2)?1:-1;Candidates c(1);for(int i=19;i>=0;--i)c[0].push_back(i);auto ref=run(a,c,0,true);for(auto p:{0,32,64})verify(ref,run(a,c,p,false));for(size_t i=0;i<K;++i)if(ref[0][i].id!=i+1)throw std::runtime_error("primary key tie mismatch");}
}
int main(int argc,char**argv){try{if(argc!=3)throw std::runtime_error("usage: progressive_benchmark FIXTURE.bin DISTRIBUTION");auto a=load(argv[1]);ties();std::cout<<std::setprecision(12);
  for(bool ivf:{false,true}){auto c=candidates(a,ivf);auto ref=run(a,c,0,true);for(size_t prefix:{size_t(0),size_t(32),size_t(64)}){Stats s;auto checksum=verify(ref,run(a,c,prefix,false,&s,prefix==0));if(s.early+s.full_reject+s.exact!=s.candidates)throw std::runtime_error("counter partition mismatch");std::cout<<"{\"type\":\"counts\",\"distribution\":\""<<argv[2]<<"\",\"seed\":"<<a.seed<<",\"route\":\""<<(ivf?"modeled_ivf64_nprobe5":"exact_candidates")<<"\",\"prefix\":"<<prefix<<",\"candidates\":"<<s.candidates<<",\"eligible\":"<<s.eligible<<",\"early_pruned\":"<<s.early<<",\"full_rejected\":"<<s.full_reject<<",\"double_calls\":"<<s.exact<<",\"checksum\":"<<checksum<<",\"cumulative_double_group_prunable\":[";for(size_t g=0;g<8;++g)std::cout<<(g?",":"")<<s.group[g];std::cout<<"]}\n";}
    // Alternate method order; counts and equality verification excluded from CPU timers.
    for(size_t repeat=0;repeat<3;++repeat)for(size_t turn=0;turn<3;++turn){size_t prefix=std::array<size_t,3>{0,32,64}[(turn+repeat)%3];auto start=std::clock();auto actual=run(a,c,prefix,false);double ms=1000.*(std::clock()-start)/CLOCKS_PER_SEC;auto checksum=verify(ref,actual);std::cout<<"{\"type\":\"timing\",\"distribution\":\""<<argv[2]<<"\",\"seed\":"<<a.seed<<",\"route\":\""<<(ivf?"modeled_ivf64_nprobe5":"exact_candidates")<<"\",\"prefix\":"<<prefix<<",\"repeat\":"<<repeat<<",\"cpu_ms\":"<<ms<<",\"checksum\":"<<checksum<<"}\n";}
  }rotation(a);return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
