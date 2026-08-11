// Interchangeable executors over one model, leased one evaluation at a
// time.
//
// An Executor cannot be shared between threads: it owns the value and
// adjoint arenas a sweep writes through, so two concurrent sweeps on one
// executor produce wrong numbers rather than a crash. A sampler running
// chains concurrently over ONE model handle still has to evaluate the
// same graph from several threads, so each caller borrows its own clone
// for the length of one evaluation and gives it back.
//
// The lease is scoped, not thread-owned. A thread_local executor would
// pin a clone for the life of every thread that ever touched the model
// and raise the question of what happens when the model outlives, or is
// outlived by, the threads holding its clones. Scoping it to the call
// answers both: the pool holds every clone, and a clone is either in the
// free list or in exactly one lease.
//
// The mutex is held only to move a pointer on and off the free list,
// never across an evaluation. That is uncontended next to a gradient on
// any model worth sampling; if a fast enough model ever proves otherwise
// the free list can go lock-free without changing this interface.
#ifndef STANLI_EXECUTOR_POOL_HPP
#define STANLI_EXECUTOR_POOL_HPP

#include <stanli/graph.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace stanli {

class ExecutorPool {
 public:
  // The prototype is cloned on demand and must outlive the pool. It is
  // never handed out itself, so the caller keeps using it if it wants.
  explicit ExecutorPool(const Executor& proto) : proto_(&proto) {}

  ExecutorPool(const ExecutorPool&) = delete;
  ExecutorPool& operator=(const ExecutorPool&) = delete;

  class Lease {
   public:
    Lease(ExecutorPool& pool, std::unique_ptr<Executor> ex)
        : pool_(&pool), ex_(std::move(ex)) {}
    ~Lease() {
      if (ex_) pool_->give_back(std::move(ex_));
    }
    Lease(Lease&& o) noexcept : pool_(o.pool_), ex_(std::move(o.ex_)) {}
    Lease& operator=(Lease&&) = delete;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    Executor& operator*() const { return *ex_; }
    Executor* operator->() const { return ex_.get(); }

   private:
    ExecutorPool* pool_;
    std::unique_ptr<Executor> ex_;
  };

  // An executor for the duration of one evaluation. Also makes sure the
  // calling thread has an autodiff stack, which stan-math requires of
  // every thread that builds a nested tape and does not create by itself.
  Lease acquire();

  // Clones currently in the free list, for tests and diagnostics.
  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return free_.size();
  }

 private:
  void give_back(std::unique_ptr<Executor> ex) {
    std::lock_guard<std::mutex> lock(mu_);
    free_.push_back(std::move(ex));
  }

  mutable std::mutex mu_;
  std::vector<std::unique_ptr<Executor>> free_;
  const Executor* proto_;
};

}  // namespace stanli

#endif
