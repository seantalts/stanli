#pragma once

#include <stan/math/rev/core/chainable_alloc.hpp>
#include <stan/math/rev/core/chainablestack.hpp>

namespace stanli {

// An autodiff stack that an owner lends to a thread with none. Under
// STAN_THREADS a thread that a binding starts has no stack, and kernels such
// as lkj_corr_cholesky use one even in a double evaluation. The owner keeps
// the storage between loans, so a loan allocates nothing.
struct LentStack {
  stan::math::ChainableStack::AutodiffStackStorage storage;

  ~LentStack() { recover(); }

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

}  // namespace stanli
