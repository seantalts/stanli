// Interchangeable executors over one model, leased one evaluation at a
// time.
//
// An Executor cannot be shared between threads: it owns the value and
// adjoint arenas a sweep writes through, so two concurrent sweeps on one
// executor produce wrong numbers rather than a crash. Each caller borrows
// its own clone for the length of one evaluation.
//
// The pool owns every clone as an entry with a busy flag. A thread
// remembers the entry it used last in a few thread_local slots and takes it
// back with one atomic exchange, so a steady-state lease touches no memory
// another thread writes. Only a miss, or a nested lease, takes the mutex.
//
// Constraints on the thread_local state:
//  - It holds raw values only. Nothing with a destructor lives in TLS: under
//    MinGW's emulated TLS such destructors run after the DLL that owns them
//    may be gone, which crashed R worker processes at exit on Windows.
//  - Slots are matched by a per-pool generation from a process-wide counter,
//    since a new pool is often built at the address of a dead one.
//  - When a thread exits its entry stays in the pool, idle. A thread with no
//    cached entry scans for an idle one before cloning.
#ifndef STANLI_EXECUTOR_POOL_HPP
#define STANLI_EXECUTOR_POOL_HPP

#include <stanli/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace stanli {

class ExecutorPool {
  struct Entry;
  struct Slot;
  struct Tls;

 public:
  // The prototype is cloned on demand and must outlive the pool. It is
  // never handed out itself, so the caller keeps using it if it wants.
  // Both out of line: entries are an incomplete type here.
  explicit ExecutorPool(const Executor& proto);
  ~ExecutorPool();

  ExecutorPool(const ExecutorPool&) = delete;
  ExecutorPool& operator=(const ExecutorPool&) = delete;

  class Lease {
   public:
    ~Lease();
    Lease(Lease&& o) noexcept;
    Lease& operator=(Lease&&) = delete;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    Executor& operator*() const { return *ex_; }
    Executor* operator->() const { return ex_; }

   private:
    friend class ExecutorPool;
    Lease(Executor* ex, Entry* entry, Slot* slot, bool counted)
        : ex_(ex), entry_(entry), slot_(slot), counted_(counted) {}

    Executor* ex_;
    Entry* entry_;
    Slot* slot_;
    bool counted_;
  };

  // An executor for the duration of one evaluation. Also makes sure the
  // calling thread has an autodiff stack, which stan-math requires of
  // every thread that builds a nested tape and does not create by itself:
  // a thread without one gets a pool-owned stack for as long as it holds
  // any lease, and must release every lease on the thread that acquired it.
  // That stack belongs to the entry that installed it, so a pool must not be
  // destroyed while a lease of another pool still shares its stack.
  Lease acquire();

  uint64_t generation() const { return gen_; }

  // Executors the pool owns, leased or idle.
  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
  }

 private:
  static Tls& tls();
  Entry* acquire_slow();

  const uint64_t gen_;
  mutable std::mutex mu_;
  std::vector<std::unique_ptr<Entry>> entries_;
  const Executor* proto_;
};

}  // namespace stanli

#endif
