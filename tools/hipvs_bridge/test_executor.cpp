#include "lib/vector/cuvs_job_executor.h"
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstdio>
using namespace oceanbase::common::obvsag;
struct State { std::atomic<int> calls{0}; std::thread::id worker; };
static void *increment(void *argument) {
  State *state=static_cast<State *>(argument);
  if(state->calls.load()==0)state->worker=std::this_thread::get_id();
  assert(state->worker==std::this_thread::get_id());
  ++state->calls;return nullptr;
}
static void *recursive(void *argument) { assert(execute_cuvs_job(increment,argument)==0);return nullptr; }
static void *failure(void *) { throw std::runtime_error("expected worker failure"); }
int main() {
  State state;
  std::vector<std::thread> submitters;
  for(int t=0;t<8;++t)submitters.emplace_back([&]{for(int n=0;n<250;++n)assert(execute_cuvs_job(increment,&state)==0);});
  for(auto& thread:submitters)thread.join();
  assert(state.calls==2000);
  assert(execute_cuvs_job(recursive,&state)==0);
  assert(state.calls==2001);
  assert(execute_cuvs_job(failure,nullptr)!=0);
  assert(execute_cuvs_job(increment,&state)==0);
  assert(state.calls==2002);
  assert(execute_cuvs_job(nullptr,nullptr)!=0);
  { CuvsJobExecutor temporary; State independent; assert(temporary.execute(increment,&independent)==0); }
  std::puts("executor concurrent submissions, thread reuse, recursion, failure recovery and shutdown passed");
}
