#include "seekdb_cuvs_bridge.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  assert(argc==2);
  const long n=4096, dim=128;
  std::mt19937 rng(20261103); std::uniform_real_distribution<float> noise(-1,1);
  std::vector<float> base(n*dim);
  for(auto& value:base)value=noise(rng);
  void* h=seekdb_cuvs_build(base.data(),n,dim); assert(h);
  std::atomic<int> completed{0};
  auto check=[&](void* index,int worker) {
    for(int iteration=0;iteration<12;++iteration) {
      const long nq=(iteration%3==0 ? 1 : (iteration%3==1 ? 17 : 128));
      const long k=(iteration%2==0 ? 100 : 10);
      std::vector<float> queries(nq*dim);std::vector<long> source(nq);
      for(long row=0;row<nq;++row) {
        source[row]=(worker*101+iteration*53+row*7)%n;
        std::copy_n(base.data()+source[row]*dim,dim,queries.data()+row*dim);
      }
      std::vector<unsigned> ids(nq*k);std::vector<float> distances(nq*k);
      assert(seekdb_cuvs_search(index,queries.data(),nq,k,ids.data(),distances.data())==0);
      for(long row=0;row<nq;++row) {
        assert(ids[row*k]==source[row]);
        std::vector<unsigned> unique(ids.begin()+row*k,ids.begin()+(row+1)*k);
        std::sort(unique.begin(),unique.end());
        assert(std::adjacent_find(unique.begin(),unique.end())==unique.end());
        for(long col=0;col<k;++col) {
          const auto id=ids[row*k+col];assert(id<static_cast<unsigned>(n));
          double exact=0;
          for(long d=0;d<dim;++d){double delta=base[id*dim+d]-queries[row*dim+d];exact+=delta*delta;}
          assert(std::isfinite(distances[row*k+col]));
          assert(std::abs(exact-distances[row*k+col])<=1e-4*std::max(1.0,exact));
        }
      }
      ++completed;
    }
  };
  std::vector<std::thread> workers;
  for(int i=0;i<4;++i)workers.emplace_back(check,h,i);
  std::thread snapshot([&](){for(int i=0;i<3;++i)assert(seekdb_cuvs_serialize(h,argv[1])==0);});
  for(auto& t:workers)t.join();snapshot.join();
  unsigned id=0;float distance=0;
  assert(seekdb_cuvs_search(h,base.data(),0,10,&id,&distance)!=0);
  assert(seekdb_cuvs_search(h,base.data(),1,n+1,&id,&distance)!=0);
  assert(seekdb_cuvs_search(h,base.data(),LONG_MAX,100,&id,&distance)!=0);
  seekdb_cuvs_free(h);
  h=seekdb_cuvs_deserialize(argv[1],n,dim);assert(h);check(h,7);seekdb_cuvs_free(h);
  assert(!seekdb_cuvs_deserialize(argv[1],n+1,dim));
  assert(!seekdb_cuvs_deserialize(argv[1],n,dim+1));
  for(long small : {256L,384L,512L,1024L}) {
    h=seekdb_cuvs_build(base.data(),small,dim);assert(h);
    std::vector<unsigned> small_ids(100);std::vector<float> small_distances(100);
    assert(seekdb_cuvs_search(h,base.data()+(small/2)*dim,1,100,small_ids.data(),small_distances.data())==0);
    assert(small_ids[0]==static_cast<unsigned>(small/2));
    for(auto value:small_ids)assert(value<static_cast<unsigned>(small));
    std::sort(small_ids.begin(),small_ids.end());
    assert(std::adjacent_find(small_ids.begin(),small_ids.end())==small_ids.end());
    seekdb_cuvs_free(h);
  }
  std::puts("warm-up index sizes 256/384/512/1024 returned complete GPU results");
  std::printf("workspace concurrency, variable nq/k, FP32 distances, overflow and snapshot checks passed: %d searches\n",completed.load());
}
