// Native elementwise / structural ops with hand-written vjps.
#include <stanli/extrema_grouping.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math/prim/fun/prod.hpp>
#include <stan/math/prim/fun/max.hpp>
#include <stan/math/prim/fun/min.hpp>
#include <stan/math/rev/core.hpp>

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace stanli {
namespace {

// OP_EXP: scalar out = exp(in). Partial is the output itself; no scratch.
void exp_fwd(KernelCtx& ctx) { ctx.out.data[0] = std::exp(ctx.in[0].data[0]); }
void exp_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data)
    ctx.in_adj[0].data[0] += ctx.out_adj * ctx.out.data[0];
}

// OP_ADD_N: scalar out = sum of scalar inputs.
void add_n_fwd(KernelCtx& ctx) {
  double acc = 0;
  for (int i = 0; i < ctx.n_in; ++i) {
    assert(ctx.in[i].len == 1);
    acc += ctx.in[i].data[0];
  }
  ctx.out.data[0] = acc;
}
void add_n_bwd(KernelCtx& ctx) {
  for (int i = 0; i < ctx.n_in; ++i)
    if (ctx.in_adj[i].data) ctx.in_adj[i].data[0] += ctx.out_adj;
}

// OP_BCAST_FMA: out[i] = a + b * x[i], a and b scalar.
void fma_fwd(KernelCtx& ctx) {
  const double a = ctx.in[0].data[0], b = ctx.in[1].data[0];
  const Desc& x = ctx.in[2];
  for (int64_t i = 0; i < x.len; ++i) ctx.out.data[i] = a + b * x.data[i];
}
void fma_bwd(KernelCtx& ctx) {
  const double b = ctx.in[1].data[0];
  const Desc& x = ctx.in[2];
  const Desc& dout = ctx.out_adj_vec;
  // Element order descending with direct accumulation, matching the var
  // tape's reverse replay of the per-element vari chain: local ascending
  // partial sums differ from it by 1 ULP.
  for (int64_t i = dout.len - 1; i >= 0; --i) {
    if (ctx.in_adj[0].data) ctx.in_adj[0].data[0] += dout.data[i];
    if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += dout.data[i] * x.data[i];
    if (ctx.in_adj[2].data) ctx.in_adj[2].data[i] += b * dout.data[i];
  }
}

// OP_MATVEC: out = X * beta, X data laid out COLUMN-major (Stan/Eigen
// convention), idata = {rows, cols}. X as a parameter is out of scope.
void matvec_fwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  Eigen::Map<const Eigen::MatrixXd> Xm(ctx.in[0].data, rows, cols);
  Eigen::Map<const Eigen::VectorXd> bm(ctx.in[1].data, cols);
  Eigen::Map<Eigen::VectorXd> outm(ctx.out.data, rows);
  outm.noalias() = Xm * bm;
}
void matvec_bwd(KernelCtx& ctx) {
  const int64_t rows = ctx.idata[0], cols = ctx.idata[1];
  if (ctx.in_adj[1].data != nullptr) {
    Eigen::Map<const Eigen::MatrixXd> Xm(ctx.in[0].data, rows, cols);
    Eigen::Map<const Eigen::VectorXd> doutm(ctx.out_adj_vec.data, rows);
    Eigen::Map<Eigen::VectorXd> adjm(ctx.in_adj[1].data, cols);
    // CmdStan's Matrix<var> exposes a scalar adjoint expression to Eigen.
    // A plain double Map selects a different, packetized reduction; severe
    // cancellation can turn that reassociation into thousands of ULP.
    adjm +=
        Xm.transpose() * doutm.unaryExpr([](double value) { return value; });
  }
}

// OP_SUM_VEC: scalar out = sum(x). Matrix<var> and std::vector use scalar
// traversal. Data Eigen expressions retain their packet/offset grouping.
void sum_vec_fwd(KernelCtx& ctx) {
  if (ctx.in[0].len && ctx.variant) {
    using Vec = Eigen::Matrix<double, Eigen::Dynamic, 1>;
    const Eigen::Map<const Vec> input(ctx.in[0].data, ctx.in[0].len);
    ctx.out.data[0] =
        ctx.variant == 2
            ? reduce_phased(ctx.in[0].data, ctx.in[0].len, ctx.idata[0],
                            Eigen::internal::scalar_sum_op<double>())
            : input.unaryExpr(Eigen::internal::core_cast_op<double, double>())
                  .sum();
    return;
  }
  double acc = 0;
  for (int64_t i = 0; i < ctx.in[0].len; ++i) acc += ctx.in[0].data[i];
  ctx.out.data[0] = acc;
}
void sum_vec_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data)
    for (int64_t i = 0; i < ctx.in[0].len; ++i)
      ctx.in_adj[0].data[i] += ctx.out_adj;
}

// OP_PROD_VEC: product of a vector/row-vector.
// Bit 0 preserves the ascending scalar reduction selected by Eigen when the
// source expression contains a strided matrix row. Bit 1 marks an active
// expression: Matrix<var> has no double packet reducer, so its value follows
// the same ascending scalar order even for a contiguous named vector.
// Bit 2 carries a known nonzero phase for a contiguous direct view.
// Eigen's redux normally chooses its packet boundary from the input address.
// Graph slots share one arena, so that would make the arithmetic grouping
// depend on every slot laid out before this one.  The explicit same-type
// CwiseUnary expression has no DirectAccessBit but retains packet access:
// first_default_aligned is consequently lane zero, matching the Eigen value
// CmdStan passes to stan::math::prod without allocating a copy here.
void prod_vec_fwd(KernelCtx& ctx) {
  assert(ctx.in[0].len > 0);
  assert(ctx.variant <= 7);
  assert((ctx.variant & 6u) != 6u);
  if (ctx.variant & 4u) {
    assert(ctx.idata != nullptr && ctx.n_idata == 1);
    ctx.out.data[0] = prod_phased(ctx.in[0].data, ctx.in[0].len, ctx.idata[0]);
  } else if (ctx.variant & 3u) {
    double product = ctx.in[0].data[0];
    for (int64_t i = 1; i < ctx.in[0].len; ++i) product *= ctx.in[0].data[i];
    ctx.out.data[0] = product;
  } else {
    using Vec = Eigen::Matrix<double, Eigen::Dynamic, 1>;
    const Eigen::Map<const Vec> input(ctx.in[0].data, ctx.in[0].len);
    ctx.out.data[0] = stan::math::prod(
        input.unaryExpr(Eigen::internal::core_cast_op<double, double>()));
  }
}
void prod_vec_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<stan::math::var, Eigen::Dynamic, 1> input(ctx.in[0].len);
  for (int64_t i = 0; i < ctx.in[0].len; ++i) input(i) = ctx.in[0].data[i];
  stan::math::var product;
  if (ctx.variant & 1u) {
    // A strided row expression takes Eigen's ascending scalar reducer.
    product = input(0);
    for (int64_t i = 1; i < ctx.in[0].len; ++i) product *= input(i);
  } else {
    product = stan::math::prod(input);
  }
  stan::math::var seeded = product * ctx.out_adj;
  stan::math::grad(seeded.vi_);
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    ctx.in_adj[0].data[i] += input(i).adj();
}

// OP_EXTREMA_VEC: min/max of a direct named vector/row-vector. Bit 0 selects
// max and bit 1 marks an active expression.
// Stan Math defines extrema of an empty real
// Eigen container as +/- infinity.  For nonempty inputs, the same-type unary
// expression deliberately clears DirectAccessBit while retaining packet
// access, so Eigen begins its packet reduction at lane zero just as it does
// for CmdStan's aligned owning VectorXd.  A direct Map would instead make
// signed-zero/NaN tie grouping depend on this slot's arena address.
void extrema_vec_fwd(KernelCtx& ctx) {
  assert(ctx.variant <= 7);
  assert((ctx.variant & 6u) != 6u);
  const bool maximum = ctx.variant & 1u;
  if (ctx.in[0].len == 0) {
    ctx.out.data[0] = maximum ? -std::numeric_limits<double>::infinity()
                              : std::numeric_limits<double>::infinity();
    return;
  }
  if (ctx.variant & 4u) {
    assert(ctx.idata != nullptr && ctx.n_idata == 1);
    ctx.out.data[0] =
        extrema_phased(ctx.in[0].data, ctx.in[0].len, ctx.idata[0], maximum);
    return;
  }
  if (ctx.variant & 2u) {
    // Matrix<var> has no packet extrema reducer. Eigen retains coefficient
    // zero, then replaces it only on a strict comparison while scanning in
    // order. This distinction controls signed-zero bits, interior NaNs, and
    // which tied coefficient receives the adjoint.
    int64_t selected = 0;
    double value = ctx.in[0].data[0];
    for (int64_t i = 1; i < ctx.in[0].len; ++i) {
      const double candidate = ctx.in[0].data[i];
      if (maximum ? value < candidate : candidate < value) {
        value = candidate;
        selected = i;
      }
    }
    ctx.out.data[0] = value;
    if (ctx.scratch != nullptr) ctx.scratch[0] = static_cast<double>(selected);
    return;
  }
  using Vec = Eigen::Matrix<double, Eigen::Dynamic, 1>;
  const Eigen::Map<const Vec> input(ctx.in[0].data, ctx.in[0].len);
  const auto owning_grouping =
      input.unaryExpr(Eigen::internal::core_cast_op<double, double>());
  Eigen::Index selected = 0;
  // Keep Stan Math's exact value path. Eigen's index-returning overload can
  // select a different packet evaluator for NaNs and signed-zero ties, so it
  // is used only to remember the reverse destination.
  ctx.out.data[0] = maximum ? stan::math::max(owning_grouping)
                            : stan::math::min(owning_grouping);
  if (!maximum)
    (void)owning_grouping.minCoeff(&selected);
  else
    (void)owning_grouping.maxCoeff(&selected);
  if (ctx.scratch != nullptr) ctx.scratch[0] = static_cast<double>(selected);
}
void extrema_vec_bwd(KernelCtx& ctx) {
  // Phased variants exist only for forward-mode generated quantities and
  // share transient scratch whose contents need not survive the sweep.
  if (ctx.variant & 4u) return;
  if (!ctx.in_adj[0].data || ctx.in[0].len == 0) return;
  ctx.in_adj[0].data[static_cast<int64_t>(ctx.scratch[0])] += ctx.out_adj;
}

int64_t scalar_scratch(const Op&, const Slot*) { return 1; }

// One selected coefficient for reverse mode. Phased reductions are emitted
// only for the forward double path.
int64_t extrema_scratch(const Op& op, const Slot* slots) {
  (void)slots;
  if ((op.variant & 4u) != 0) return 0;
  return 1;
}

// OP_INDEX: scalar out = in[flat], idata = {flat}. Backward scatters.
void index_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = ctx.in[0].data[ctx.idata[0]];
}
void index_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data) ctx.in_adj[0].data[ctx.idata[0]] += ctx.out_adj;
}

// OP_SET_INDEX: out = copy(in[0]) with out[flat] = in[1] (scalar).
void set_index_fwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.out.len; ++i) ctx.out.data[i] = ctx.in[0].data[i];
  ctx.out.data[ctx.idata[0]] = ctx.in[1].data[0];
}
void set_index_bwd(KernelCtx& ctx) {
  const int64_t f = ctx.idata[0];
  if (ctx.in_adj[0].data)
    for (int64_t i = 0; i < ctx.out.len; ++i)
      if (i != f) ctx.in_adj[0].data[i] += ctx.out_adj_vec.data[i];
  if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += ctx.out_adj_vec.data[f];
}

// OP_SET_INDEX_INPLACE: same update, but out IS in[0] (one slot, so one
// arena buffer and one adjoint buffer). Sound only where the graph pass
// proved this write is the last use of that vector, so destroying the old
// element is unobservable.
void set_index_inplace_fwd(KernelCtx& ctx) {
  ctx.out.data[ctx.idata[0]] = ctx.in[1].data[0];
}
// in[0].adj and out.adj alias, so the elementwise `in.adj += out.adj` of
// the copying form is already done. Element f is the exception: it belongs
// to the written value, not to the overwritten vector, so hand it over and
// clear it.
void set_index_inplace_bwd(KernelCtx& ctx) {
  const int64_t f = ctx.idata[0];
  if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += ctx.out_adj_vec.data[f];
  ctx.out_adj_vec.data[f] = 0.0;
}

// OP_SLICE: out = in[start .. start+out.len), idata = {start}.
void slice_fwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0];
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = ctx.in[0].data[start + i];
}
void slice_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t start = ctx.idata[0];
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.in_adj[0].data[start + i] += ctx.out_adj_vec.data[i];
}

// OP_DYNAMIC_SLICE: the 1-based integer in[1] selects one fixed-width block
// from in[0]. Generated quantities use this for an RNG-produced state index;
// idata[0] is the number of blocks and out.len is their common width.
int64_t dynamic_slice_start(const KernelCtx& ctx) {
  if (ctx.n_in != 2 || ctx.n_idata != 1 || ctx.in[1].len != 1 ||
      ctx.idata == nullptr || ctx.idata[0] <= 0 || ctx.out.len <= 0 ||
      ctx.in[0].len % ctx.idata[0] != 0 ||
      ctx.in[0].len / ctx.idata[0] != ctx.out.len)
    throw std::logic_error("malformed dynamic slice descriptor");
  const double raw = ctx.in[1].data[0];
  const int64_t count = ctx.idata[0];
  if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < 1.0 ||
      raw > static_cast<double>(count))
    throw std::out_of_range("dynamic slice index out of range");
  return (static_cast<int64_t>(raw) - 1) * ctx.out.len;
}
void dynamic_slice_fwd(KernelCtx& ctx) {
  const int64_t start = dynamic_slice_start(ctx);
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = ctx.in[0].data[start + i];
}
void dynamic_slice_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t start = dynamic_slice_start(ctx);
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.in_adj[0].data[start + i] += ctx.out_adj_vec.data[i];
}

// OP_SLICE_STRIDED: out[i] = in[start + i*stride], idata = {start, stride}.
// Row extraction from column-major data matrices.
void slice_strided_fwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0], stride = ctx.idata[1];
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.out.data[i] = ctx.in[0].data[start + i * stride];
}
void slice_strided_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t start = ctx.idata[0], stride = ctx.idata[1];
  for (int64_t i = 0; i < ctx.out.len; ++i)
    ctx.in_adj[0].data[start + i * stride] += ctx.out_adj_vec.data[i];
}

// OP_GATHER: out[k] = in[idata[k]] (0-based indices).
void gather_fwd(KernelCtx& ctx) {
  for (int64_t k = 0; k < ctx.out.len; ++k)
    ctx.out.data[k] = ctx.in[0].data[ctx.idata[k]];
}
void gather_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  double* in_adj = ctx.in_adj[0].data;
  const double* out_adj = ctx.out_adj_vec.data;
  const int* idata = ctx.idata;
  const int64_t n = ctx.out.len;
  int64_t k = 0;
  while (k < n) {
    const int idx = idata[k];
    double sum = out_adj[k];
    int64_t j = k + 1;
    while (j < n && idata[j] == idx) sum += out_adj[j++];
    in_adj[idx] += sum;
    k = j;
  }
}

// OP_CONCAT2: out = [in[0]; in[1]] (contiguous; serves append_row of
// vectors and append_col of col-major matrices). Backward splits.
void concat2_fwd(KernelCtx& ctx) {
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    ctx.out.data[i] = ctx.in[0].data[i];
  for (int64_t i = 0; i < ctx.in[1].len; ++i)
    ctx.out.data[ctx.in[0].len + i] = ctx.in[1].data[i];
}
void concat2_bwd(KernelCtx& ctx) {
  if (ctx.in_adj[0].data)
    for (int64_t i = 0; i < ctx.in[0].len; ++i)
      ctx.in_adj[0].data[i] += ctx.out_adj_vec.data[i];
  if (ctx.in_adj[1].data)
    for (int64_t i = 0; i < ctx.in[1].len; ++i)
      ctx.in_adj[1].data[i] += ctx.out_adj_vec.data[ctx.in[0].len + i];
}

// OP_REP_MAT: idata = {R, C, mode}. mode 0: scalar fill; mode 1: vector
// replicated as C columns (out[j*R+i] = v[i]); mode 2: row_vector across R
// rows (out[j*R+i] = v[j]). AoS rep_matrix copies var handles, so each
// source vari accumulates its consumers' adjoints in flat col-major order;
// the backward loops preserve exactly that per-vari sequence.
void rep_mat_fwd(KernelCtx& ctx) {
  const int64_t R = ctx.idata[0], C = ctx.idata[1], mode = ctx.idata[2];
  const double* v = ctx.in[0].data;
  for (int64_t j = 0; j < C; ++j)
    for (int64_t i = 0; i < R; ++i)
      ctx.out.data[j * R + i] = mode == 0 ? v[0] : mode == 1 ? v[i] : v[j];
}
void rep_mat_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const int64_t R = ctx.idata[0], C = ctx.idata[1], mode = ctx.idata[2];
  const double* dout = ctx.out_adj_vec.data;
  double* dv = ctx.in_adj[0].data;
  for (int64_t j = 0; j < C; ++j)
    for (int64_t i = 0; i < R; ++i)
      dv[mode == 0 ? 0 : mode == 1 ? i : j] += dout[j * R + i];
}

// OP_SET_SLICE: out = copy(in[0]) with out[start..start+in[1].len) = in[1].
void set_slice_fwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0];
  for (int64_t i = 0; i < ctx.out.len; ++i) ctx.out.data[i] = ctx.in[0].data[i];
  for (int64_t i = 0; i < ctx.in[1].len; ++i)
    ctx.out.data[start + i] = ctx.in[1].data[i];
}
void set_slice_bwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0], len = ctx.in[1].len;
  if (ctx.in_adj[0].data)
    for (int64_t i = 0; i < ctx.out.len; ++i)
      if (i < start || i >= start + len)
        ctx.in_adj[0].data[i] += ctx.out_adj_vec.data[i];
  if (ctx.in_adj[1].data)
    for (int64_t i = 0; i < len; ++i)
      ctx.in_adj[1].data[i] += ctx.out_adj_vec.data[start + i];
}

// OP_SET_SLICE_INPLACE: out IS in[0]. Untouched adjoints already pass
// through because those two adjoint buffers alias. Overwritten cells belong
// to the RHS instead, so route them there and clear them from the base.
void set_slice_inplace_fwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0];
  for (int64_t i = 0; i < ctx.in[1].len; ++i)
    ctx.out.data[start + i] = ctx.in[1].data[i];
}
void set_slice_inplace_bwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0], len = ctx.in[1].len;
  if (ctx.in_adj[1].data)
    for (int64_t i = 0; i < len; ++i)
      ctx.in_adj[1].data[i] += ctx.out_adj_vec.data[start + i];
  for (int64_t i = 0; i < len; ++i) ctx.out_adj_vec.data[start + i] = 0.0;
}

// OP_SET_SLICE_STRIDED: out = copy(in[0]) with
// out[start + i*stride] = in[1][i], idata = {start, stride}. The write-side
// mirror of OP_SLICE_STRIDED: a loop filling a column-major matrix row by
// row advances its flat index by the row count.
void set_slice_strided_fwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0], stride = ctx.idata[1];
  for (int64_t i = 0; i < ctx.out.len; ++i) ctx.out.data[i] = ctx.in[0].data[i];
  for (int64_t i = 0; i < ctx.in[1].len; ++i)
    ctx.out.data[start + i * stride] = ctx.in[1].data[i];
}
void set_slice_strided_bwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0], stride = ctx.idata[1],
                len = ctx.in[1].len;
  if (ctx.in_adj[1].data)
    for (int64_t i = 0; i < len; ++i)
      ctx.in_adj[1].data[i] += ctx.out_adj_vec.data[start + i * stride];
  if (ctx.in_adj[0].data) {
    // Everything except the overwritten comb of positions passes through.
    // Walk the comb alongside the vector rather than add-then-subtract:
    // (x + a) - a is not x in floating point once x + a rounds.
    int64_t next = start, k = 0;
    for (int64_t i = 0; i < ctx.out.len; ++i) {
      if (i == next && k < len) {
        next += stride;
        ++k;
        continue;
      }
      ctx.in_adj[0].data[i] += ctx.out_adj_vec.data[i];
    }
  }
}

// OP_SET_SLICE_STRIDED_INPLACE: the comb-shaped counterpart above, with
// out and in[0] sharing both value and adjoint storage.
void set_slice_strided_inplace_fwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0], stride = ctx.idata[1];
  for (int64_t i = 0; i < ctx.in[1].len; ++i)
    ctx.out.data[start + i * stride] = ctx.in[1].data[i];
}
void set_slice_strided_inplace_bwd(KernelCtx& ctx) {
  const int64_t start = ctx.idata[0], stride = ctx.idata[1],
                len = ctx.in[1].len;
  if (ctx.in_adj[1].data)
    for (int64_t i = 0; i < len; ++i)
      ctx.in_adj[1].data[i] += ctx.out_adj_vec.data[start + i * stride];
  for (int64_t i = 0; i < len; ++i)
    ctx.out_adj_vec.data[start + i * stride] = 0.0;
}

}  // namespace

// Called from Executor's constructor path; a static registrar object in a
// static library dropped by the linker.
void register_elementwise_kernels() {
  register_kernel(OP_EXP, Kernel{exp_fwd, exp_bwd, nullptr});
  register_kernel(OP_ADD_N, Kernel{add_n_fwd, add_n_bwd, nullptr});
  register_kernel(OP_BCAST_FMA, Kernel{fma_fwd, fma_bwd, nullptr});
  register_kernel(OP_MATVEC, Kernel{matvec_fwd, matvec_bwd, nullptr});
  register_kernel(OP_SUM_VEC, Kernel{sum_vec_fwd, sum_vec_bwd, nullptr});
  register_kernel(OP_PROD_VEC, Kernel{prod_vec_fwd, prod_vec_bwd, nullptr});
  register_kernel(OP_EXTREMA_VEC,
                  Kernel{extrema_vec_fwd, extrema_vec_bwd, extrema_scratch});
  register_kernel(OP_INDEX, Kernel{index_fwd, index_bwd, nullptr});
  register_kernel(OP_SET_INDEX, Kernel{set_index_fwd, set_index_bwd, nullptr});
  register_kernel(OP_SET_INDEX_INPLACE, Kernel{set_index_inplace_fwd,
                                               set_index_inplace_bwd, nullptr});
  register_kernel(OP_SLICE, Kernel{slice_fwd, slice_bwd, nullptr});
  register_kernel(OP_DYNAMIC_SLICE,
                  Kernel{dynamic_slice_fwd, dynamic_slice_bwd, nullptr});
  register_kernel(OP_SET_SLICE, Kernel{set_slice_fwd, set_slice_bwd, nullptr});
  register_kernel(OP_SET_SLICE_INPLACE, Kernel{set_slice_inplace_fwd,
                                               set_slice_inplace_bwd, nullptr});
  register_kernel(OP_SET_SLICE_STRIDED, Kernel{set_slice_strided_fwd,
                                               set_slice_strided_bwd, nullptr});
  register_kernel(OP_SET_SLICE_STRIDED_INPLACE,
                  Kernel{set_slice_strided_inplace_fwd,
                         set_slice_strided_inplace_bwd, nullptr});
  register_kernel(OP_SLICE_STRIDED,
                  Kernel{slice_strided_fwd, slice_strided_bwd, nullptr});
  register_kernel(OP_GATHER, Kernel{gather_fwd, gather_bwd, nullptr});
  register_kernel(OP_CONCAT2, Kernel{concat2_fwd, concat2_bwd, nullptr});
  register_kernel(OP_REP_MAT, Kernel{rep_mat_fwd, rep_mat_bwd, nullptr});
}

}  // namespace stanli
