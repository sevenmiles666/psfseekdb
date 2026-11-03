/* Synchronous executor for GPU jobs that need a larger stack than an OB worker.
 * Callbacks own rollback of their side effects if they throw. */
#pragma once
#include <pthread.h>
#include <condition_variable>
#include <mutex>

namespace oceanbase { namespace common { namespace obvsag {
class CuvsJobExecutor {
public:
  using Function = void *(*)(void *);
  CuvsJobExecutor() : available_(false), stopping_(false), job_(nullptr), thread_() {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) { return; }
    int result = pthread_attr_setstacksize(&attributes, 32UL * 1024 * 1024);
    if (result == 0) { result = pthread_create(&thread_, &attributes, entry, this); }
    pthread_attr_destroy(&attributes);
    available_ = result == 0;
  }
  ~CuvsJobExecutor() {
    // Destruction, like any owner destruction, requires callers to be quiescent.
    if (available_) {
      { std::lock_guard<std::mutex> lock(queue_mutex_); stopping_ = true; }
      condition_.notify_all();
      pthread_join(thread_, nullptr);
    }
  }
  CuvsJobExecutor(const CuvsJobExecutor &) = delete;
  CuvsJobExecutor &operator=(const CuvsJobExecutor &) = delete;
  int execute(Function function, void *argument) {
    if (!available_ || function == nullptr) { return -1; }
    // Avoid deadlock if a GPU job synchronously invokes another large-stack job.
    if (pthread_equal(pthread_self(), thread_)) {
      try { function(argument); return 0; } catch (...) { return -1; }
    }
    std::lock_guard<std::mutex> submission(submit_mutex_);
    Job pending{function, argument, false, -1};
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (stopping_) { return -1; }
    job_ = &pending;
    condition_.notify_all();
    condition_.wait(lock, [&pending] { return pending.completed; });
    return pending.status;
  }
private:
  struct Job { Function function; void *argument; bool completed; int status; };
  static void *entry(void *argument) {
    static_cast<CuvsJobExecutor *>(argument)->run();
    return nullptr;
  }
  void run() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    for (;;) {
      condition_.wait(lock, [this] { return stopping_ || job_ != nullptr; });
      if (stopping_ && job_ == nullptr) { return; }
      Job *current = job_;
      lock.unlock();
      int status = 0;
      try { current->function(current->argument); } catch (...) { status = -1; }
      lock.lock();
      job_ = nullptr;
      current->status = status;
      current->completed = true;
      condition_.notify_all();
    }
  }
  bool available_;
  bool stopping_;
  Job *job_;
  pthread_t thread_;
  std::mutex submit_mutex_;
  std::mutex queue_mutex_;
  std::condition_variable condition_;
};
inline int execute_cuvs_job(CuvsJobExecutor::Function function, void *argument) {
  static CuvsJobExecutor executor;
  return executor.execute(function, argument);
}
}}}
