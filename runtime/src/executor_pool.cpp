#include <stanli/executor_pool.hpp>

#include <stan/math/rev/core/chainablestack.hpp>

namespace stanli {

// Only the non-owning pointer is TLS. The last lease destroys the tape while
// its calling thread is still running, including when leases overlap or move.
struct ExecutorPool::Tape {
  stan::math::ChainableStack stack;
  size_t leases = 0;

  static Tape*& current() {
    static thread_local Tape* tape = nullptr;
    return tape;
  }
};

ExecutorPool::Lease::Lease(ExecutorPool& pool, std::unique_ptr<Executor> ex)
    : pool_(&pool), ex_(std::move(ex)), tape_(Tape::current()) {
  if (!tape_) Tape::current() = tape_ = new Tape;
  ++tape_->leases;
}

ExecutorPool::Lease::Lease(Lease&& other) noexcept
    : pool_(other.pool_), ex_(std::move(other.ex_)), tape_(other.tape_) {
  other.tape_ = nullptr;
}

ExecutorPool::Lease::~Lease() {
  if (ex_) pool_->give_back(std::move(ex_));
  if (tape_ && --tape_->leases == 0) {
    Tape::current() = nullptr;
    delete tape_;
  }
}

ExecutorPool::Lease ExecutorPool::acquire() {
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
