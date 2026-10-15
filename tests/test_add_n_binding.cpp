// Generic ADD_N fold: all supported arities, alignments, aliasing and IEEE
// cases.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
namespace stanli {
using ForwardFn = void (*)(KernelCtx&);
ForwardFn resolve_forward_fn(const Op&);
}  // namespace stanli
bool same(double x, double y) {
  return (std::isnan(x) && std::isnan(y)) ||
         std::memcmp(&x, &y, sizeof(x)) == 0;
}
int main() {
  const auto* kernel = stanli::find_kernel(stanli::OP_ADD_N);
  int cases = 0, errors = 0;
  for (int n = 0; n <= 6; ++n)
    for (int offset = 0; offset < 8; ++offset)
      for (int pattern = 0; pattern < 64; ++pattern)
        for (int alias = -1; alias < n; ++alias) {
          std::array<std::array<double, 16>, 6> data;
          std::array<int, 6> indices{};
          stanli::KernelCtx ctx{};
          ctx.n_in = n;
          ctx.variant = 0;
          ctx.idata = indices.data();
          ctx.n_idata = n;
          for (int k = 0; k < 6; ++k) {
            data[k].fill(713.25);
            indices[k] = 0;
            double x = std::sin((k + 1) * .31 * (pattern + 1));
            if (pattern == 60) x = k % 2 ? 1e20 : -1e20;
            if (pattern == 61) x = k % 2 ? -0. : 0.;
            if (pattern == 62)
              x = k % 2 ? -std::numeric_limits<double>::infinity()
                        : std::numeric_limits<double>::infinity();
            if (pattern == 63) x = std::numeric_limits<double>::quiet_NaN();
            data[k][offset + indices[k]] = x;
            ctx.in[k] = {data[k].data() + offset, 1};
          }
          const auto original = data;
          double output = 713.25, want = 0;
          for (int k = 0; k < n; ++k) want += ctx.in[k].data[indices[k]];
          ctx.out = {alias < 0 ? &output : ctx.in[alias].data + indices[alias],
                     1};
          stanli::Op op;
          op.opcode = stanli::OP_ADD_N;
          op.n_in = n;
          op.variant = ctx.variant;
          auto forward = stanli::resolve_forward_fn(op);
          if (forward == kernel->forward) ++errors;
          forward(ctx);
          ++cases;
          bool ok = same(ctx.out.data[0], want);
          for (int k = 0; k < 6; ++k)
            for (int j = 0; j < 16; ++j)
              if (!(k == alias && j == offset + indices[k]))
                ok = ok && same(data[k][j], original[k][j]);
          if (!ok) ++errors;
        }
  stanli::Op op;
  op.opcode = stanli::OP_ADD_N;
  op.n_in = 6;
  op.n_in = 7;
  if (stanli::resolve_forward_fn(op) != kernel->forward) ++errors;
  op.n_in = 6;
  op.dyn_lengths = 1;
  if (stanli::resolve_forward_fn(op) != kernel->forward) ++errors;
  op.dyn_lengths = 0;
  op.variant = 255;
  if (stanli::resolve_forward_fn(op) != kernel->forward) ++errors;
  const auto saved = *kernel;
  auto custom = saved;
  custom.forward = [](stanli::KernelCtx& c) { c.out.data[0] = 42.; };
  stanli::register_kernel(stanli::OP_ADD_N, custom);
  op.variant = 0;
  if (stanli::resolve_forward_fn(op) != custom.forward) ++errors;
  stanli::register_kernel(stanli::OP_ADD_N, saved);
  std::printf("cases=%d errors=%d\n", cases, errors);
  return errors ? 1 : 0;
}
