#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include "../kernels/add_n_fold.hpp"

#include <cassert>

namespace stanli {

extern void (*const canonical_add_n_forward)(KernelCtx&);

namespace {
template <int Arity>
void add_n_fixed_fwd(KernelCtx& ctx) {
  assert(ctx.n_in == Arity);
  add_n_fold<Arity>(ctx);
}
void (*pick_add_n_arity(unsigned arity))(KernelCtx&) {
  switch (arity) {
    case 0:
      return &add_n_fixed_fwd<0>;
    case 1:
      return &add_n_fixed_fwd<1>;
    case 2:
      return &add_n_fixed_fwd<2>;
    case 3:
      return &add_n_fixed_fwd<3>;
    case 4:
      return &add_n_fixed_fwd<4>;
    case 5:
      return &add_n_fixed_fwd<5>;
    case 6:
      return &add_n_fixed_fwd<6>;
  }
  return canonical_add_n_forward;
}

}  // namespace

using ForwardFn = void (*)(KernelCtx&);

ForwardFn resolve_forward_fn(const Op& op) {
  constexpr int capacity = sizeof(KernelCtx{}.in) / sizeof(Desc);
  if (op.opcode == OP_ADD_N && op.n_in <= capacity && op.variant == 0 &&
      !op.dyn_lengths && kernel(op.opcode).forward == canonical_add_n_forward) {
    return pick_add_n_arity(op.n_in);
  }
  if (op.opcode == OP_ISLAND && op.variant == kIslandCallVariant)
    return island_calls_fwd;
  return kernel(op.opcode).forward;
}

}  // namespace stanli
