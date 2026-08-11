#include <stanli/executor_pool.hpp>

#include <stan/math/rev/core/chainablestack.hpp>

namespace stanli {

ExecutorPool::Lease ExecutorPool::acquire() {
  // stan-math REQUIRES an autodiff stack on every thread that builds a
  // nested tape, and under STAN_THREADS the pointer to it is thread_local
  // and starts null. CmdStan never writes this because TBB's
  // scheduler-entry hook does it for every worker, and this build stubs
  // TBB out; nuts.cpp does it explicitly for the chain threads it starts
  // itself. Here the threads belong to the CALLER -- a sampler embedding
  // the runtime -- so the pool is the only place that can.
  //
  // Constructing one on a thread that already has a stack is a no-op:
  // AutodiffStackSingleton::init() hands ownership to the first
  // constructor on the thread and returns false to every later one, whose
  // destructor then leaves the stack alone. So this is safe on the main
  // thread too. Destroyed when the thread exits, which is the only point
  // at which freeing the tape is correct.
  static thread_local stan::math::ChainableStack ad_tape_for_this_thread;

  std::unique_ptr<Executor> ex;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!free_.empty()) {
      ex = std::move(free_.back());
      free_.pop_back();
    }
  }
  // Cloning outside the lock: it copies the arenas, which is far more
  // work than the free list is worth blocking for. Two threads arriving
  // at an empty pool both clone, and both clones are kept.
  if (!ex) ex = std::make_unique<Executor>(*proto_);
  return Lease(*this, std::move(ex));
}

}  // namespace stanli
