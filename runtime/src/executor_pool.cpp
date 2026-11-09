#include <stanli/executor_pool.hpp>

#include <stan/math/rev/core/chainable_alloc.hpp>

#include <atomic>

namespace stanli {

namespace {

constexpr size_t kSlots = 4;

std::atomic<uint64_t> next_generation{1};

using stan::math::ChainableStack;

}  // namespace

struct ExecutorPool::Entry {
  // The stack a thread without one installs for the span of its leases.
  struct Tape {
    ChainableStack::AutodiffStackStorage storage;

    // What stan::math::recover_memory() does, on this storage rather than on
    // the thread's current one.
    void recover() {
      storage.var_stack_.clear();
      storage.var_nochain_stack_.clear();
      for (auto* x : storage.var_alloc_stack_) delete x;
      storage.var_alloc_stack_.clear();
      storage.memalloc_.recover_all();
    }
  };

  alignas(128) std::atomic<bool> busy{true};
  std::unique_ptr<Executor> ex;
  std::unique_ptr<Tape> tape;
};

struct ExecutorPool::Slot {
  uint64_t gen;
  Entry* entry;
  bool held;
};

struct ExecutorPool::Tls {
  Slot slots[kSlots];
  size_t next;
  // The entry whose tape is installed on this thread. It stays busy until
  // the last lease sharing the tape ends, so no other thread can touch it.
  Entry* tape_owner;
  size_t tape_leases;
};

ExecutorPool::Tls& ExecutorPool::tls() {
  static thread_local Tls t;
  return t;
}

ExecutorPool::ExecutorPool(const Executor& proto)
    : gen_(next_generation.fetch_add(1, std::memory_order_relaxed)),
      proto_(&proto) {}
ExecutorPool::~ExecutorPool() = default;

ExecutorPool::Entry* ExecutorPool::acquire_slow() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& e : entries_) {
      if (!e->busy.load(std::memory_order_relaxed) &&
          !e->busy.exchange(true, std::memory_order_acquire))
        return e.get();
    }
  }
  auto e = std::make_unique<Entry>();
  e->ex = std::make_unique<Executor>(*proto_);
  Entry* raw = e.get();
  std::lock_guard<std::mutex> lock(mu_);
  entries_.push_back(std::move(e));
  return raw;
}

ExecutorPool::Lease ExecutorPool::acquire() {
  Tls& t = tls();
  Slot* hit = nullptr;
  for (Slot& s : t.slots) {
    if (s.gen == gen_) {
      hit = &s;
      break;
    }
  }

  Entry* e = nullptr;
  Slot* slot = nullptr;
  if (hit && !hit->held) {
    Entry* c = hit->entry;
    if (!c->busy.load(std::memory_order_relaxed) &&
        !c->busy.exchange(true, std::memory_order_acquire))
      e = c;
  }
  if (e) {
    slot = hit;
  } else {
    e = acquire_slow();
    if (hit) {
      // Held means a lease of this thread's own: leave it that entry.
      // Otherwise another thread took the entry, so move to this one.
      if (!hit->held) {
        hit->entry = e;
        slot = hit;
      }
    } else {
      Slot* victim = nullptr;
      for (Slot& s : t.slots) {
        if (s.gen == 0) {
          victim = &s;
          break;
        }
      }
      for (size_t i = 0; !victim && i < kSlots; ++i) {
        Slot& s = t.slots[(t.next + i) % kSlots];
        if (!s.held) {
          victim = &s;
          t.next = (t.next + i + 1) % kSlots;
        }
      }
      if (victim) {
        *victim = Slot{gen_, e, false};
        slot = victim;
      }
    }
  }
  if (slot) slot->held = true;

  bool counted = false;
  if (t.tape_owner) {
    ++t.tape_leases;
    counted = true;
  } else if (ChainableStack::instance_ == nullptr) {
    if (!e->tape) e->tape = std::make_unique<Entry::Tape>();
    ChainableStack::instance_ = &e->tape->storage;
    t.tape_owner = e;
    t.tape_leases = 1;
    counted = true;
  }
  return Lease(e->ex.get(), e, slot, counted);
}

ExecutorPool::Lease::Lease(Lease&& o) noexcept
    : ex_(o.ex_), entry_(o.entry_), slot_(o.slot_), counted_(o.counted_) {
  o.entry_ = nullptr;
}

// Must run on the thread that acquired the lease.
ExecutorPool::Lease::~Lease() {
  if (!entry_) return;
  Tls& t = tls();
  if (slot_) slot_->held = false;
  Entry* owner = nullptr;
  if (counted_ && --t.tape_leases == 0) {
    owner = t.tape_owner;
    t.tape_owner = nullptr;
    ChainableStack::instance_ = nullptr;
    owner->tape->recover();
    owner->busy.store(false, std::memory_order_release);
  }
  if (entry_ != owner && !(counted_ && entry_ == t.tape_owner))
    entry_->busy.store(false, std::memory_order_release);
}

}  // namespace stanli
