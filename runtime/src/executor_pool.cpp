#include <stanli/executor_pool.hpp>

#include <stan/math/rev/core/chainablestack.hpp>

namespace stanli {

// A tape a thread borrows for the span of its leases. The pool owns every
// tape. A thread that arrives without an autodiff stack -- a caller's own
// worker thread under STAN_THREADS, where stan-math's pointer starts null
// -- installs one from the free list on its first lease and returns it on
// its last, so overlapping and moved leases share it and after the first
// evaluation on a thread no lease allocates. A thread that already has a
// stack (the main thread, which stan-math's static scheduler observer
// registers; a chain thread nuts.cpp equipped) installs nothing.
//
// Only the non-owning pointer is thread_local. Nothing with a destructor
// lives in TLS: under MinGW's emulated TLS such destructors run after the
// DLL that owns them may be gone, which is what crashed R worker processes
// at exit on Windows.
struct ExecutorPool::Tape {
  stan::math::ChainableStack::AutodiffStackStorage storage;
  size_t leases = 0;

  static Tape*& current() {
    static thread_local Tape* tape = nullptr;
    return tape;
  }

  // What stan::math::recover_memory() does, on this storage rather than on
  // the thread's current one, so a tape goes back to the free list empty.
  void recover() {
    storage.var_stack_.clear();
    storage.var_nochain_stack_.clear();
    for (auto* x : storage.var_alloc_stack_) delete x;
    storage.var_alloc_stack_.clear();
    storage.memalloc_.recover_all();
  }
};

ExecutorPool::ExecutorPool(const Executor& proto) : proto_(&proto) {}
ExecutorPool::~ExecutorPool() = default;

ExecutorPool::Tape* ExecutorPool::take_tape() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!free_tapes_.empty()) {
      Tape* t = free_tapes_.back().release();
      free_tapes_.pop_back();
      return t;
    }
  }
  return new Tape;
}

void ExecutorPool::give_back_tape(Tape* tape) {
  std::lock_guard<std::mutex> lock(mu_);
  free_tapes_.emplace_back(tape);
}

ExecutorPool::Lease::Lease(ExecutorPool& pool, std::unique_ptr<Executor> ex)
    : pool_(&pool), ex_(std::move(ex)), tape_(Tape::current()) {
  if (!tape_ && stan::math::ChainableStack::instance_ == nullptr) {
    tape_ = pool.take_tape();
    stan::math::ChainableStack::instance_ = &tape_->storage;
    Tape::current() = tape_;
  }
  if (tape_) ++tape_->leases;
}

ExecutorPool::Lease::Lease(Lease&& other) noexcept
    : pool_(other.pool_), ex_(std::move(other.ex_)), tape_(other.tape_) {
  other.tape_ = nullptr;
}

// Must run on the thread that acquired the lease: the tape is that thread's
// stan-math stack, and both TLS pointers are reset here.
ExecutorPool::Lease::~Lease() {
  if (ex_) pool_->give_back(std::move(ex_));
  if (tape_ && --tape_->leases == 0) {
    Tape::current() = nullptr;
    stan::math::ChainableStack::instance_ = nullptr;
    tape_->recover();
    // Whichever pool the last lease came from keeps the tape; tapes carry
    // no model state, so one migrating between the pools of one model is
    // harmless.
    pool_->give_back_tape(tape_);
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
