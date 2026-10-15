// Shared original ADD_N fold; only the arity may become constant.
#ifndef STANLI_ADD_N_FOLD_HPP
#define STANLI_ADD_N_FOLD_HPP
#include <stanli/optable.hpp>
#include <cassert>
namespace stanli {
template <int Arity = -1>
inline void add_n_fold(KernelCtx& ctx) {
  double acc = 0;
  const int count = Arity < 0 ? ctx.n_in : Arity;
  for (int i = 0; i < count; ++i) {
    assert(ctx.in[i].len == 1);
    acc += ctx.in[i].data[0];
  }
  ctx.out.data[0] = acc;
}
}  // namespace stanli
#endif
