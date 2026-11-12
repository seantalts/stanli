// Elementwise expression ops vs the stan-math var operations that stanc3's
// C++ backend would emit for the same MIR node. lp = sum(op(args)).
#include "expect.hpp"
#include "graph_helpers.hpp"

#include <stanli/expression_layout.hpp>
#include <stanli/extrema_grouping.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stan/math.hpp>
#include <stan/model/indexing.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

using stan::math::var;
using VecV = Eigen::Matrix<var, -1, 1>;

static const std::vector<double> A{0.5, -1.2, 2.0, 0.3};
static const std::vector<double> B{1.5, 0.7, -0.4, 2.2};
static const double S = 0.8, T = -1.7;

static VecV mkv(const std::vector<double>& v) {
  VecV x(v.size());
  for (size_t i = 0; i < v.size(); ++i) x(i) = v[i];
  return x;
}

static void test_matrix_transpose_add() {
  using MatV = Eigen::Matrix<var, -1, -1>;
  for (int n : {1, 3, 7}) {
    for (bool left : {false, true}) {
      stan::math::nested_rev_autodiff scope;
      MatV a(n, n);
      std::vector<double> x(n * n), transposed(n * n), seeds(n * n),
          gradient(n * n), other(n * n, 0), output(n * n);
      for (int i = 0; i < n * n; ++i) {
        x[i] = .13 * (i + 1);
        a.data()[i] = x[i];
        seeds[i] = i % 3 == 0 ? 1e16 : (i % 3 == 1 ? -1e16 : 1.0);
        gradient[i] = .7;
      }
      for (int i = 0; i < n * n; ++i) transposed[i] = x[(i % n) * n + i / n];
      MatV result = left ? MatV(stan::math::add(stan::math::transpose(a), a))
                         : MatV(stan::math::add(a, stan::math::transpose(a)));
      var total = 0;
      for (int i = 0; i < n * n; ++i) total += result.data()[i] * seeds[i];
      for (int i = 0; i < n * n; ++i) a.data()[i].adj() = gradient[i];
      stan::math::grad(total.vi_);
      stanli::KernelCtx ctx;
      ctx.n_in = 2;
      ctx.in[0] = {left ? transposed.data() : x.data(), n * n};
      ctx.in[1] = {left ? x.data() : transposed.data(), n * n};
      ctx.in_adj[0] = {left ? other.data() : gradient.data(), n * n};
      ctx.in_adj[1] = {left ? gradient.data() : other.data(), n * n};
      ctx.out = {output.data(), n * n};
      ctx.out_adj_vec = {seeds.data(), n * n};
      ctx.out_adj = seeds[0];
      ctx.variant =
          left ? stanli::kAddTransposeLeft : stanli::kAddTransposeRight;
      ctx.idata = &n;
      ctx.n_idata = 1;
      const auto* kernel = stanli::find_kernel(stanli::OP_ADD);
      kernel->forward(ctx);
      kernel->backward(ctx);
      for (int i = 0; i < n * n; ++i) {
        expect_eq_or_both_nan("matrix transpose add value", output[i],
                              result.data()[i].val());
        expect_eq_or_both_nan("matrix transpose add gradient", gradient[i],
                              a.data()[i].adj());
        expect_eq_or_both_nan("matrix transpose add direct accumulation",
                              other[i], 0);
      }
    }
  }
}

// `params` marks which inputs the graph makes parameters; the rest reach the
// kernel with no adjoint, the way a data slot does. `ref_fn` gets every input
// as a var and is expected to read the data ones through `.val()`, so the
// reference picks stan-math's non-var overload for them.
template <typename F>
static void check_case_params(const std::string& tag, uint16_t opcode,
                              int64_t out_len,
                              const std::vector<std::vector<double>>& vals,
                              const std::vector<bool>& params, F&& ref_fn,
                              int lp_ulp = 0, uint8_t variant = 0) {
  auto r =
      stanli::testutil::run_op_sum(opcode, out_len, vals, params, {}, variant);
  std::vector<VecV> vs;
  for (const auto& v : vals) vs.push_back(mkv(v));
  var lp = ref_fn(vs);
  lp.grad();
  // OP_LOGV takes Eigen's packet log, a ulp off libm on some arguments. The
  // gradients are 1/x and stay bitwise; only lp carries the difference, and a
  // sum that cancels reports it as several ulps of the result.
  expect_ulp_of(tag + " lp", r.value, lp.val(), lp.val(), lp_ulp);
  size_t gi = 0;
  for (size_t k = 0; k < vs.size(); ++k) {
    if (!params[k]) continue;
    for (int i = 0; i < vs[k].size(); ++i)
      expect_eq_or_both_nan(tag + " g" + std::to_string(gi), r.grad[gi],
                            vs[k](i).adj()),
          ++gi;
  }
  stan::math::recover_memory();
}

template <typename F>
static void check_case(const std::string& tag, uint16_t opcode, int64_t out_len,
                       const std::vector<std::vector<double>>& vals, F&& ref_fn,
                       int lp_ulp = 0) {
  check_case_params(tag, opcode, out_len, vals,
                    std::vector<bool>(vals.size(), true),
                    std::forward<F>(ref_fn), lp_ulp);
}

static uint64_t layout_bits(double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static void test_expression_layout_policy() {
  using stanli::ExpressionLayout;
  using stanli::expression_layout::contiguous;
  using stanli::expression_layout::elementwise;

  if (contiguous(ExpressionLayout::unknown(), 3).known() ||
      contiguous(ExpressionLayout::direct(7), 5) !=
          ExpressionLayout::direct(12) ||
      contiguous(ExpressionLayout::packet(), 5) != ExpressionLayout::packet() ||
      contiguous(ExpressionLayout::direct(std::numeric_limits<int64_t>::max()),
                 1)
          .known()) {
    ++failures;
    std::printf("FAIL expression layout contiguous policy\n");
  }
  if (elementwise(true, true, true, true) != ExpressionLayout::scalar() ||
      elementwise(false, true, true, true) != ExpressionLayout::packet() ||
      elementwise(false, false, true, true) != ExpressionLayout::scalar() ||
      elementwise(false, true, false, true).known() ||
      elementwise(false, true, true, false) != ExpressionLayout::scalar()) {
    ++failures;
    std::printf("FAIL expression layout elementwise policy\n");
  }

  using stanli::mir::Expr;
  using stanli::mir::UnsizedLeaf;
  Expr vector;
  vector.kind = Expr::Var;
  vector.unsized.leaf = UnsizedLeaf::Vector;
  vector.type_ = "UVector";
  Expr multi;
  multi.kind = Expr::FunApp;
  multi.name = "IndexMulti";
  Expr gather;
  gather.kind = Expr::Indexed;
  gather.unsized.leaf = UnsizedLeaf::Vector;
  gather.type_ = "UVector";
  gather.args = {vector, multi};
  Expr exp_gather;
  exp_gather.kind = Expr::FunApp;
  exp_gather.fn_lib = Expr::Lib::StanLib;
  exp_gather.name = "exp";
  exp_gather.unsized.leaf = UnsizedLeaf::Vector;
  exp_gather.type_ = "UVector";
  exp_gather.args = {gather};
  Expr start;
  start.kind = Expr::LitInt;
  start.lit_i = 2;
  start.unsized.leaf = UnsizedLeaf::Int;
  Expr count = start;
  Expr segment_gather;
  segment_gather.kind = Expr::FunApp;
  segment_gather.fn_lib = Expr::Lib::StanLib;
  segment_gather.name = "segment";
  segment_gather.unsized.leaf = UnsizedLeaf::Vector;
  segment_gather.type_ = "UVector";
  segment_gather.args = {gather, start, count};
  Expr matrix = vector;
  matrix.unsized.leaf = UnsizedLeaf::Matrix;
  matrix.type_ = "UMatrix";
  Expr transpose;
  transpose.kind = Expr::FunApp;
  transpose.fn_lib = Expr::Lib::StanLib;
  transpose.name = "Transpose__";
  transpose.unsized.leaf = UnsizedLeaf::Matrix;
  transpose.type_ = "UMatrix";
  transpose.args = {matrix};
  Expr udf_return = vector;
  udf_return.kind = Expr::FunApp;
  udf_return.fn_lib = Expr::Lib::UserDefined;
  Expr array_vector = vector;
  array_vector.unsized.depth = 1;
  Expr single = multi;
  single.name = "IndexSingle";
  Expr inner = vector;
  inner.kind = Expr::Indexed;
  inner.args = {array_vector, single};
  Expr between = multi;
  between.name = "IndexBetween";
  between.args = {start, count};
  Expr inner_range = inner;
  inner_range.args = {array_vector, single, between};
  if (stanli::mir::source_expression_layout(exp_gather) !=
          ExpressionLayout::scalar() ||
      stanli::mir::source_expression_layout(segment_gather) !=
          ExpressionLayout::scalar() ||
      stanli::mir::source_expression_layout(transpose).known() ||
      stanli::mir::source_expression_layout(udf_return) !=
          ExpressionLayout::direct() ||
      stanli::mir::source_expression_layout(inner) !=
          ExpressionLayout::direct() ||
      stanli::mir::source_expression_layout(inner_range) !=
          ExpressionLayout::direct(1)) {
    ++failures;
    std::printf("FAIL MIR source expression layout policy\n");
  }

  // Pin the policy to two actual Stan Math reduction expressions. An owning
  // vector exposes packet access; a row of a matrix is strided and reduces in
  // scalar coefficient order. The generated values deliberately search for a
  // bit-level distinction between those two expressions on the host Eigen
  // packet width.
  const int packet_width = std::max(
      1, static_cast<int>(Eigen::internal::packet_traits<double>::size));
  const int n = 2 * packet_width + 1;
  Eigen::VectorXd owning(n);
  VecV active(n);
  bool distinguished = packet_width == 1;
  uint64_t state = 0x4d4154524958524fULL;
  double packet_ref = 0.0;
  double scalar_ref = 0.0;
  for (int trial = 0; trial < 4096 && !distinguished; ++trial) {
    for (int i = 0; i < n; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const double p = 0.01 + 0.98 * static_cast<double>(state >> 11) /
                                  static_cast<double>(uint64_t{1} << 53);
      owning[i] = 1.0 - p;
      active[i] = owning[i];
    }
    packet_ref = stan::math::prod(
        owning.unaryExpr(Eigen::internal::core_cast_op<double, double>()));
    scalar_ref = stan::math::prod(active).val();
    distinguished =
        packet_width == 1 || layout_bits(packet_ref) != layout_bits(scalar_ref);
  }
  if (!distinguished && packet_width != 1) {
    ++failures;
    std::printf(
        "FAIL expression layout grouping oracle did not distinguish "
        "(width %d, packet %016llx, scalar %016llx)\n",
        packet_width, static_cast<unsigned long long>(layout_bits(packet_ref)),
        static_cast<unsigned long long>(layout_bits(scalar_ref)));
  } else {
    const ExpressionLayout packet_layout = elementwise(false, true, true, true);
    const ExpressionLayout scalar_layout = ExpressionLayout::scalar();
    const double packet_result =
        packet_layout.kind == ExpressionLayout::Kind::Packet ? packet_ref
                                                             : scalar_ref;
    const double scalar_result =
        scalar_layout.kind == ExpressionLayout::Kind::Scalar ? scalar_ref
                                                             : packet_ref;
    if (layout_bits(packet_result) != layout_bits(packet_ref) ||
        layout_bits(scalar_result) != layout_bits(scalar_ref)) {
      ++failures;
      std::printf("FAIL expression layout selected wrong Stan Math grouping\n");
    }
  }
  stan::math::recover_memory();
}

// dot(src[idata], weights) plus, for a parameter source, a second term that
// seeds a nonzero adjoint before the gather's backward runs.
static std::vector<double> run_gather_dot(const std::vector<int>& idx,
                                          const std::vector<double>& weights,
                                          int64_t j, bool param_source) {
  using namespace stanli;
  Graph g;
  const int src = g.add_slot(j, param_source);
  const int weight = g.add_slot((int64_t)weights.size(), false);
  const int gathered = g.add_slot((int64_t)idx.size(), false);
  g.idata_pool.push_back(idx);
  Op gop;
  gop.opcode = OP_GATHER;
  gop.out = gathered;
  gop.in[0] = src;
  gop.n_in = 1;
  gop.idata = g.idata_pool.back().data();
  gop.n_idata = (int64_t)g.idata_pool.back().size();
  g.ops.push_back(gop);
  const int dotv = g.add_slot(1, false);
  g.add_op(OP_DOT, {gathered, weight}, dotv);
  int lp = -1, offsets = -1;
  if (param_source) {
    offsets = g.add_slot(j, false);
    const int shifted = g.add_slot(j, false);
    const int seed_sum = g.add_slot(1, false);
    g.add_op(OP_ADD, {src, offsets}, shifted);
    g.add_op(OP_SUM_VEC, {shifted}, seed_sum);
    lp = g.add_slot(1, false);
    g.add_op(OP_ADD_N, {dotv, seed_sum}, lp);
  } else {
    const int extra = g.add_slot(1, true);
    lp = g.add_slot(1, false);
    g.add_op(OP_ADD, {dotv, extra}, lp);
  }
  g.result_slot = lp;

  Executor ex(std::move(g));
  double* wp = ex.value_ptr(weight);
  std::copy(weights.begin(), weights.end(), wp);
  if (param_source) {
    double* op = ex.value_ptr(offsets);
    for (int64_t i = 0; i < j; ++i) op[i] = 0.0;
  } else {
    double* sp = ex.value_ptr(src);
    for (int64_t i = 0; i < j; ++i) sp[i] = 0.5 + 0.1 * (double)i;
  }
  std::vector<double> grad((size_t)ex.n_params());
  for (int64_t i = 0; i < ex.n_params(); ++i)
    ex.params_data()[i] = 0.1 + 0.05 * (double)i;
  ex.gradient(grad.data());
  return grad;
}

static void test_gather_bwd_repeated_indices() {
  const int64_t J = 5;
  const std::vector<double> weights_sorted = {1.3, -0.7, 2.1,  0.4, -1.9, 0.05,
                                              3.3, -2.2, 0.11, 1.0, -0.65};
  const std::vector<int> idx_sorted = {0, 0, 0, 1, 1, 2, 3, 3, 3, 3, 4};
  const std::vector<double> weights_unsorted = {0.9,  -1.1, 2.3,   -0.4, 1.7,
                                                -2.9, 0.65, -0.15, 1.05, -0.8};
  const std::vector<int> idx_unsorted = {2, 0, 3, 0, 1, 3, 2, 0, 4, 1};

  auto check = [](const char* tag, const std::vector<int>& idx,
                  const std::vector<double>& weights, int64_t j, int budget) {
    const std::vector<double> got = run_gather_dot(idx, weights, j, true);
    std::vector<double> want(j, 1.0);
    for (size_t k = 0; k < idx.size(); ++k) want[idx[k]] += weights[k];
    for (int64_t i = 0; i < j; ++i)
      expect_ulp(std::string(tag) + " g" + std::to_string(i), got[i], want[i],
                 budget);
  };
  check("gather sorted runs", idx_sorted, weights_sorted, J, 10);
  check("gather unsorted dup", idx_unsorted, weights_unsorted, J, 0);
}

static void test_gather_bwd_null_in_adj() {
  const int64_t J = 3;
  const std::vector<int> idx = {0, 1, 1, 2};
  const std::vector<double> weights = {1.5, -0.5, 2.0, 0.25};
  const std::vector<double> got = run_gather_dot(idx, weights, J, false);
  if (got.size() != 1 || got[0] != 1.0) {
    ++failures;
    std::printf("FAIL gather null in_adj no-op grad0 got %.17g want 1\n",
                got.empty() ? 0.0 : got[0]);
  }
}

// Use arbitrary output seeds and preexisting input adjoints: a unit-seeded
// sum hides several distinct Stan overloads' multiplication/reduction orders.
static void test_matrix_scalar_division() {
  using namespace stanli;
  for (int n : {1, 7, 40, 129}) {
    for (bool active_matrix : {false, true}) {
      for (bool active_scalar : {false, true}) {
        stan::math::nested_rev_autodiff nested;
        Eigen::VectorXd values(n), seeds(n), out(n), adj(n);
        for (int i = 0; i < n; ++i) {
          values[i] = (i % 2 ? -1.0 : 1.0) * (0.17 + 0.031 * i);
          seeds[i] = 0.23 + 0.073 * ((i * 7) % 11);
          adj[i] = -0.17;
        }
        double divisor = 0.04820923213628131, scalar_adj = -0.17;
        VecV a = values.cast<var>();
        var b = divisor;
        VecV result;
        Eigen::VectorXd expected_out;
        if (active_matrix && active_scalar)
          result = stan::math::divide(a, b);
        else if (active_matrix)
          result = stan::math::divide(a, divisor);
        else if (active_scalar)
          result = stan::math::divide(values, b);
        else
          expected_out = stan::math::divide(values, divisor);
        if (active_matrix || active_scalar) {
          expected_out = result.val();
          a.adj().setConstant(-0.17);
          b.adj() = -0.17;
          result.adj() = seeds;
          stan::math::grad();
        }
        KernelCtx c{};
        c.n_in = 2;
        c.in[0] = {values.data(), n};
        c.in[1] = {&divisor, 1};
        c.out = {out.data(), n};
        c.out_adj = seeds[0];
        c.out_adj_vec = {seeds.data(), n};
        if (active_matrix) c.in_adj[0] = {adj.data(), n};
        if (active_scalar) c.in_adj[1] = {&scalar_adj, 1};
        c.variant = kDivMatrixScalar | (active_matrix ? kDivMatrixActive : 0) |
                    (active_scalar ? kDivScalarActive : 0);
        const Kernel* k = find_kernel(OP_DIV);
        k->forward(c);
        k->backward(c);
        for (int i = 0; i < n; ++i) {
          expect_eq_or_both_nan("matrix/scalar division value", out[i],
                                expected_out[i]);
          if (active_matrix)
            expect_eq_or_both_nan("matrix/scalar division da", adj[i],
                                  a[i].adj());
        }
        if (active_scalar)
          expect_eq_or_both_nan("matrix/scalar division db", scalar_adj,
                                b.adj());
      }
    }
  }
}

static void test_power_seed_grouping() {
  using namespace stanli;
  for (int n : {1, 7, 40}) {
    stan::math::nested_rev_autodiff scope;
    Eigen::VectorXd a(n), b(n), seed(n), output(n), da(n), db(n);
    VecV av(n), bv(n), result(n);
    for (int i = 0; i < n; ++i) {
      a[i] = .17 + .031 * i;
      b[i] = .71 + .043 * i;
      seed[i] = -.29 + .071 * i;
      av[i] = a[i];
      bv[i] = b[i];
      result[i] = stan::math::pow(av[i], bv[i]);
      av[i].adj() = da[i] = .13;
      bv[i].adj() = db[i] = -.11;
      result[i].adj() = seed[i];
    }
    stan::math::grad();
    KernelCtx ctx{};
    ctx.n_in = 2;
    ctx.in[0] = {a.data(), n};
    ctx.in[1] = {b.data(), n};
    ctx.in_adj[0] = {da.data(), n};
    ctx.in_adj[1] = {db.data(), n};
    ctx.out = {output.data(), n};
    ctx.out_adj = seed[0];
    ctx.out_adj_vec = {seed.data(), n};
    const Kernel* implementation = find_kernel(OP_POW);
    implementation->forward(ctx);
    implementation->backward(ctx);
    for (int i = 0; i < n; ++i) {
      expect_eq_or_both_nan("power weighted value", output[i], result[i].val());
      expect_eq_or_both_nan("power weighted base", da[i], av[i].adj());
      expect_eq_or_both_nan("power weighted exponent", db[i], bv[i].adj());
    }
  }
}

static void test_reduction_overloads() {
  using namespace stanli;
  for (int n : {0, 1, 7, 40, 129}) {
    stan::math::nested_rev_autodiff scope;
    Eigen::VectorXd values(n + 1), adj = Eigen::VectorXd::Constant(n, .13);
    VecV active(n);
    for (int i = 0; i <= n; ++i) {
      values[i] = (i % 3 == 0 ? 1e7 : -.3) + .17 * i;
      if (i < n) active[i] = values[i];
    }
    var result = stan::math::dot_self(active);
    active.adj().setConstant(.13);
    result.adj() = -.731;
    stan::math::grad();
    double output;
    KernelCtx ctx{};
    ctx.n_in = 2;
    ctx.in[0] = ctx.in[1] = {values.data(), n};
    ctx.in_adj[0] = ctx.in_adj[1] = {adj.data(), n};
    ctx.out = {&output, 1};
    ctx.out_adj = -.731;
    ctx.variant = 1;  // Matrix<var> dot_self
    const Kernel* dot = find_kernel(OP_DOT);
    dot->forward(ctx);
    dot->backward(ctx);
    expect_eq_or_both_nan("dot_self value grouping", output, result.val());
    for (int i = 0; i < n; ++i)
      expect_eq_or_both_nan("dot_self gradient grouping", adj[i],
                            active[i].adj());

    const Kernel* sum = find_kernel(OP_SUM_VEC);
    ctx.n_in = 1;
    ctx.variant = 1;  // owning Eigen double input
    sum->forward(ctx);
    Eigen::VectorXd owning = values.head(n);
    expect_eq_or_both_nan("sum owning Eigen", output, stan::math::sum(owning));
    int offset = 1;
    ctx.in[0] = {values.data() + 1, n};
    ctx.variant = 2;
    ctx.idata = &offset;
    ctx.n_idata = 1;
    sum->forward(ctx);
    expect_eq_or_both_nan("sum shifted Eigen", output,
                          stan::math::sum(values.segment(1, n)));
  }
}

static uint64_t reduction_bits(double x) {
  uint64_t out = 0;
  std::memcpy(&out, &x, sizeof(out));
  return out;
}

static bool same_reduction_value(double got, double want) {
  // Eigen deliberately leaves the sign and payload of a propagated NaN
  // unspecified.  Sanitizer instrumentation can therefore change those bits
  // without changing the reduction's value contract.  Everything else --
  // including signed zero and infinity -- remains a bitwise comparison.
  if (std::isnan(got) || std::isnan(want))
    return std::isnan(got) && std::isnan(want);
  return reduction_bits(got) == reduction_bits(want);
}

static void test_product_exact_grouping() {
  using namespace stanli;
  // Direct kernel calls need the one-time registration normally performed by
  // Executor construction.
  {
    Graph graph;
    const int in = graph.add_slot(1, true);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_EXPV, {in}, out);
    graph.result_slot = out;
    Executor warm(std::move(graph));
  }

  const Kernel& product = kernel(OP_PROD_VEC);
  if (product.backward == nullptr || product.scratch_size != nullptr) {
    ++failures;
    std::printf("FAIL product kernel implementation contract\n");
  }

  const double denorm = std::numeric_limits<double>::denorm_min();
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::vector<double>> cases = {
      {0.5},
      {-0.0},
      {denorm},
      {inf},
      {nan},
      {0.5, 0.5, 0.5},
      {0.0, inf, 2.0},
      {-0.0, inf, 2.0},
      {inf, 0.0, -2.0},
      {nan, 0.0, inf},
      {1.0 + 0x1p-52, 1.0 - 0x1p-52, 1.0 + 0x1p-51},
      {0.5, 0.5, 0.5, 0.5, 0.5},
      {1e200, 1e200, 1e-200, 1e-200, 3.0},
      {1e200, 1e-200, 1e200, 1e-200, 3.0},
      {denorm, 2.0, 2.0, 2.0, 2.0},
      {-0.0, -1.0, -1.0, 1.0, 1.0},
      {inf, -0.0, nan, -inf, 1.0},
  };

  // Exercise every packet boundary on the build host.  Fixed lengths 1/3/5
  // above pin the public contract; these sizes keep the same test meaningful
  // when Eigen is compiled for SSE, AVX2, or AVX-512.
  const int packet_width = std::max(
      1, static_cast<int>(Eigen::internal::packet_traits<double>::size));
  const int packet_lengths[] = {packet_width - 1, packet_width,
                                packet_width + 1, 2 * packet_width + 1};
  const int exponents[] = {500, -500, 400, -400, 300, -300, 0, 0};
  for (int n : packet_lengths) {
    if (n <= 0) continue;  // Scalar-only Eigen has packet width one.
    std::vector<double> values(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double mantissa =
          1.0 + static_cast<double>((i * 5 + 1) % 7) * 0x1p-52;
      values[static_cast<size_t>(i)] =
          std::ldexp((i % 3 == 0) ? -mantissa : mantissa, exponents[i % 8]);
    }
    cases.push_back(std::move(values));
  }

  std::map<std::string, const mir::FunDef*> funs;
  MirInterp<double> interp(funs, "product grouping test");
  mir::Expr variable;
  variable.kind = mir::Expr::Var;
  variable.name = "x";
  variable.type_ = "UVector";
  variable.unsized.leaf = mir::UnsizedLeaf::Vector;
  mir::Expr call;
  call.kind = mir::Expr::FunApp;
  call.name = "prod";
  call.fn_lib = mir::Expr::Lib::StanLib;
  call.type_ = "UReal";
  call.unsized.leaf = mir::UnsizedLeaf::Real;
  call.args = {variable};

  for (size_t c = 0; c < cases.size(); ++c) {
    const std::vector<double>& values = cases[c];
    Eigen::VectorXd pinned(static_cast<Eigen::Index>(values.size()));
    for (size_t i = 0; i < values.size(); ++i) pinned[i] = values[i];
    const double want = stan::math::prod(pinned);

    DataMap::Entry entry;
    entry.r = values;
    entry.dims = {static_cast<int64_t>(values.size())};
    interp.env()["x"] = std::move(entry);
    const double interpreted = interp.eval(call).r.at(0);
    if (!same_reduction_value(interpreted, want)) {
      ++failures;
      std::printf("FAIL product interpreter case %zu: got %llx want %llx\n", c,
                  static_cast<unsigned long long>(reduction_bits(interpreted)),
                  static_cast<unsigned long long>(reduction_bits(want)));
    }

    // Pin the address-independence seam at every packet-relevant shift.  A
    // direct Eigen::Map differs at odd shifts; OP_PROD_VEC must not.
    for (int offset = 0; offset < packet_width; ++offset) {
      Eigen::VectorXd storage(static_cast<Eigen::Index>(
          values.size() + static_cast<size_t>(packet_width)));
      storage.setZero();
      std::copy(values.begin(), values.end(), storage.data() + offset);
      double got = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] =
          Desc{storage.data() + offset, static_cast<int64_t>(values.size())};
      ctx.out = Desc{&got, 1};
      product.forward(ctx);
      if (!same_reduction_value(got, want)) {
        ++failures;
        std::printf(
            "FAIL product kernel case %zu offset %d: got %llx want "
            "%llx\n",
            c, offset, static_cast<unsigned long long>(reduction_bits(got)),
            static_cast<unsigned long long>(reduction_bits(want)));
      }
    }
  }

  // This tranche changes only Eigen vector/row-vector products.  Stan arrays
  // retain MirInterp's existing ascending scalar fold.
  mir::Expr array_variable = variable;
  array_variable.name = "a";
  array_variable.type_ = "(UArray UReal)";
  array_variable.unsized.leaf = mir::UnsizedLeaf::Real;
  array_variable.unsized.depth = 1;
  mir::Expr array_call = call;
  array_call.args = {array_variable};
  const std::vector<double> array_values = {1e200, 1e200, 1e-200, 1e-200, 3.0};
  DataMap::Entry array_entry;
  array_entry.r = array_values;
  array_entry.dims = {static_cast<int64_t>(array_values.size())};
  interp.env()["a"] = std::move(array_entry);
  double array_want = 1.0;
  for (double value : array_values) array_want *= value;
  const double array_got = interp.eval(array_call).r.at(0);
  if (reduction_bits(array_got) != reduction_bits(array_want)) {
    ++failures;
    std::printf("FAIL array product changed its scalar left fold\n");
  }

  // Pin the actual expression surfaces admitted by lower.cpp.  CmdStan
  // gives Eigen the unevaluated CwiseBinary expression; stanli materializes
  // subtraction into a graph slot before OP_PROD_VEC.  Basic subtraction is
  // lane-bit-transparent, so the two product groupings must still agree.
  const auto check_minus_surface = [&](int n, const char* label) {
    std::vector<Eigen::VectorXd> indexed(2, Eigen::VectorXd(n));
    const double xs[] = {0.5,  -0.25, 2.0, -1.0, std::nextafter(1.0, 0.0),
                         -0.5, 0.25,  1.5};
    for (int i = 0; i < n; ++i)
      indexed[1][i] = xs[i % static_cast<int>(sizeof(xs) / sizeof(xs[0]))];

    const uint64_t direct =
        reduction_bits(stan::math::prod(Eigen::VectorXd::Ones(n) - indexed[1]));
    const uint64_t indexed_transpose = reduction_bits(
        stan::math::prod(Eigen::RowVectorXd::Ones(n) - indexed[1].transpose()));
    if (direct != indexed_transpose) {
      ++failures;
      std::printf(
          "FAIL %s pinned outer-minus surfaces disagree: %llx vs "
          "%llx\n",
          label, static_cast<unsigned long long>(direct),
          static_cast<unsigned long long>(indexed_transpose));
    }

    std::vector<double> materialized(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      materialized[static_cast<size_t>(i)] = 1.0 - indexed[1][i];
    for (int offset = 0; offset < packet_width; ++offset) {
      Eigen::VectorXd storage(n + packet_width);
      storage.setZero();
      std::copy(materialized.begin(), materialized.end(),
                storage.data() + offset);
      double got = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] = Desc{storage.data() + offset, n};
      ctx.out = Desc{&got, 1};
      product.forward(ctx);
      const uint64_t bits = reduction_bits(got);
      if (bits != direct || bits != indexed_transpose) {
        ++failures;
        std::printf("FAIL %s offset %d: got %llx want %llx\n", label, offset,
                    static_cast<unsigned long long>(bits),
                    static_cast<unsigned long long>(direct));
      }
    }
  };
  check_minus_surface(5, "len5 outer-minus product");
  check_minus_surface(2 * packet_width + 1,
                      "packet-boundary outer-minus product");

  // The corpus surface indexes a row of a column-major matrix and transposes
  // it.  That Eigen block remains strided, disables packet access on the
  // outer subtraction, and makes prod reduce from coefficient zero in
  // ascending scalar order.  Search deterministic valid probabilities for a
  // case that distinguishes that order from the packet variant, then pin the
  // raw-expression oracle and the materialized graph kernel to the scalar
  // result.  Scalar-only Eigen builds have no distinct packet order.
  const int matrix_n = 2 * packet_width + 1;
  Eigen::MatrixXd matrix(2, matrix_n);
  std::vector<double> factors(static_cast<size_t>(matrix_n));
  bool distinguished = packet_width == 1;
  uint64_t state = 0x4d4154524958524fULL;
  for (int trial = 0; trial < 4096 && !distinguished; ++trial) {
    for (int i = 0; i < matrix_n; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const double p = 0.01 + 0.98 * static_cast<double>(state >> 11) /
                                  static_cast<double>(uint64_t{1} << 53);
      matrix(0, i) = 0.25;
      matrix(1, i) = p;
      factors[static_cast<size_t>(i)] = 1.0 - p;
    }
    double scalar = factors[0];
    for (int i = 1; i < matrix_n; ++i)
      scalar *= factors[static_cast<size_t>(i)];

    double packet = 0.0;
    KernelCtx packet_ctx;
    packet_ctx.n_in = 1;
    packet_ctx.in[0] = Desc{factors.data(), matrix_n};
    packet_ctx.out = Desc{&packet, 1};
    packet_ctx.variant = 0;
    product.forward(packet_ctx);
    if (reduction_bits(packet) == reduction_bits(scalar)) continue;

    const double direct = stan::math::prod(Eigen::VectorXd::Ones(matrix_n) -
                                           matrix.row(1).transpose());
    double got = 0.0;
    KernelCtx scalar_ctx = packet_ctx;
    scalar_ctx.out = Desc{&got, 1};
    scalar_ctx.variant = 1;
    product.forward(scalar_ctx);
    if (reduction_bits(direct) != reduction_bits(scalar) ||
        reduction_bits(got) != reduction_bits(scalar)) {
      ++failures;
      std::printf("FAIL matrix-row product did not use scalar grouping\n");
    }
    distinguished = true;
  }
  if (!distinguished) {
    ++failures;
    std::printf("FAIL matrix-row oracle did not distinguish packet order\n");
  }
}

static void test_reduction_view_grouping() {
  using namespace stanli;
  const Kernel& product = kernel(OP_PROD_VEC);
  const Kernel& extrema = kernel(OP_EXTREMA_VEC);
  const int phase_modulus = static_cast<int>(extrema_phase_modulus());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double tie_pool[] = {0.0, -0.0, 2.0, -3.0, 2.0, nan, -0.0,
                             0.0, -4.0, 9.0, -4.0, 1.0, nan, -2.0};
  const int tie_pool_size =
      static_cast<int>(sizeof(tie_pool) / sizeof(*tie_pool));

  // A graph value has already been materialized, so the reduction opcode
  // carries the source view's phase explicitly. Compare both native kernels
  // with Stan Math reducing the original Eigen segment.
  for (int n = 1; n <= 257; ++n) {
    for (int offset = 0; offset < phase_modulus; ++offset) {
      Eigen::VectorXd product_base(offset + n + 2);
      Eigen::VectorXd extrema_base(offset + n + 2);
      for (int i = 0; i < product_base.size(); ++i) {
        const int centered = (i * 37 + n * 11) % 29 - 14;
        product_base[i] = 1.0 + centered * 0x1p-16;
        extrema_base[i] = tie_pool[i % tie_pool_size];
      }
      const Eigen::VectorXd product_owned = product_base.segment(offset, n);
      const Eigen::VectorXd extrema_owned = extrema_base.segment(offset, n);
      const double product_want =
          stan::math::prod(product_base.segment(offset, n));
      const double extrema_wants[] = {
          stan::math::min(extrema_base.segment(offset, n)),
          stan::math::max(extrema_base.segment(offset, n))};

      int phase = offset;
      double product_got = 0.0;
      KernelCtx product_ctx;
      product_ctx.n_in = 1;
      product_ctx.in[0] = Desc{const_cast<double*>(product_owned.data()), n};
      product_ctx.out = Desc{&product_got, 1};
      product_ctx.variant = 4;
      product_ctx.idata = &phase;
      product_ctx.n_idata = 1;
      product.forward(product_ctx);
      if (!same_reduction_value(product_got, product_want)) {
        ++failures;
        std::printf("FAIL phased product n=%d offset=%d\n", n, offset);
      }

      for (int maximum = 0; maximum < 2; ++maximum) {
        double got = 0.0;
        KernelCtx ctx;
        ctx.n_in = 1;
        ctx.in[0] = Desc{const_cast<double*>(extrema_owned.data()), n};
        ctx.out = Desc{&got, 1};
        ctx.variant = static_cast<uint8_t>(maximum | 4);
        ctx.idata = &phase;
        ctx.n_idata = 1;
        extrema.forward(ctx);
        if (!same_reduction_value(got, extrema_wants[maximum])) {
          ++failures;
          std::printf("FAIL phased extrema n=%d offset=%d %s\n", n, offset,
                      maximum ? "max" : "min");
        }
      }
    }
  }

  // The contract here is exact parity with the original view. Some Eigen
  // configurations use the same scalar traversal for every tested size, so
  // requiring a value difference from an unphased reduction would be brittle.

  // Gathered arguments have no packet access. Repeated indices are retained
  // in index order and the scalar reduction's reverse path must scatter back
  // through OP_GATHER rather than treating the gather as a copy.
  bool gather_distinguished = false;
  for (int n = 1; n <= 20; ++n) {
    Eigen::VectorXd source(n);
    for (int i = 0; i < n; ++i) source[i] = tie_pool[i % tie_pool_size];
    std::vector<int> positions(static_cast<size_t>(n));
    std::vector<double> gathered(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      positions[static_cast<size_t>(i)] = n - i;
      gathered[static_cast<size_t>(i)] = source[n - i - 1];
    }
    const stan::model::index_multi index(positions);
    for (int maximum = 0; maximum < 2; ++maximum) {
      const double want =
          maximum
              ? stan::math::max(stan::model::rvalue(source, "source", index))
              : stan::math::min(stan::model::rvalue(source, "source", index));
      double got = 0.0;
      double selected = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] = Desc{gathered.data(), n};
      ctx.out = Desc{&got, 1};
      ctx.variant = static_cast<uint8_t>(maximum | 2);
      ctx.scratch = &selected;
      extrema.forward(ctx);
      if (!same_reduction_value(got, want)) {
        ++failures;
        std::printf("FAIL gathered extrema n=%d %s\n", n,
                    maximum ? "max" : "min");
      }
      const Eigen::Map<const Eigen::VectorXd> packet(gathered.data(), n);
      const double packet_want =
          maximum ? stan::math::max(packet) : stan::math::min(packet);
      gather_distinguished |= !same_reduction_value(packet_want, want);
    }
  }
  if (!gather_distinguished) {
    ++failures;
    std::printf(
        "FAIL gathered extrema oracle never distinguished packet order\n");
  }

  {
    Graph graph;
    const int source = graph.add_slot(3, true);
    const int gathered = graph.add_slot(4, false);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_GATHER, {source}, gathered, {2, 2, 0, 2});
    const int product_op = graph.add_op(OP_PROD_VEC, {gathered}, out);
    graph.ops[product_op].variant = 1;
    graph.result_slot = out;
    Executor executor(std::move(graph));
    const std::array<double, 3> values = {2.0, 3.0, 5.0};
    std::copy(values.begin(), values.end(), executor.params_data());
    double adj[3] = {};
    const double value = executor.gradient(adj);
    if (value != 250.0 || adj[0] != 125.0 || adj[1] != 0.0 || adj[2] != 150.0) {
      ++failures;
      std::printf("FAIL gathered product adjoints were not accumulated\n");
    }
  }
  {
    Graph graph;
    const int source = graph.add_slot(3, true);
    const int gathered = graph.add_slot(4, false);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_GATHER, {source}, gathered, {1, 1, 2, 0});
    const int extrema_op = graph.add_op(OP_EXTREMA_VEC, {gathered}, out);
    graph.ops[extrema_op].variant = 2;
    graph.result_slot = out;
    Executor executor(std::move(graph));
    const std::array<double, 3> values = {5.0, 2.0, 7.0};
    std::copy(values.begin(), values.end(), executor.params_data());
    double adj[3] = {};
    const double value = executor.gradient(adj);
    if (value != 2.0 || adj[0] != 0.0 || adj[1] != 1.0 || adj[2] != 0.0) {
      ++failures;
      std::printf(
          "FAIL gathered extrema adjoint did not route through gather\n");
    }
  }
}

static void test_extrema_exact_grouping() {
  using namespace stanli;
  const Kernel& extrema = kernel(OP_EXTREMA_VEC);
  if (extrema.backward == nullptr || extrema.scratch_size == nullptr) {
    ++failures;
    std::printf("FAIL extrema kernel has no differentiable implementation\n");
  }

  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::vector<double>> cases = {
      {},
      {1.0},
      {0.0, -0.0},
      {-0.0, 0.0},
      {inf, -inf, 3.0},
      {-inf, inf, -3.0},
      {2.0, 2.0, -4.0, -4.0, 2.0},
      {nan, 1.0, -2.0},
      {1.0, nan, -2.0},
      {1.0, -2.0, nan},
      {nan, nan, 0.0, -0.0},
  };
  const int packet_width = std::max(
      1, static_cast<int>(Eigen::internal::packet_traits<double>::size));
  for (int n : {packet_width - 1, packet_width, packet_width + 1,
                2 * packet_width - 1, 2 * packet_width, 2 * packet_width + 1}) {
    if (n < 0) continue;
    std::vector<double> values(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double magnitude =
          std::ldexp(1.0 + static_cast<double>((i * 7 + 3) % 11) * 0x1p-52,
                     (i % 7) * 137 - 411);
      values[static_cast<size_t>(i)] = i % 2 == 0 ? magnitude : -magnitude;
    }
    if (n > 2) values[static_cast<size_t>(n - 1)] = values[1];
    cases.push_back(std::move(values));
  }

  for (size_t c = 0; c < cases.size(); ++c) {
    const std::vector<double>& values = cases[c];
    Eigen::VectorXd pinned(static_cast<Eigen::Index>(values.size()));
    std::copy(values.begin(), values.end(), pinned.data());
    const double wants[] = {stan::math::min(pinned), stan::math::max(pinned)};
    for (int variant = 0; variant < 2; ++variant) {
      for (int offset = 0; offset < packet_width; ++offset) {
        Eigen::VectorXd storage(static_cast<Eigen::Index>(
            values.size() + static_cast<size_t>(packet_width)));
        storage.setZero();
        std::copy(values.begin(), values.end(), storage.data() + offset);
        double got = 0.0;
        KernelCtx ctx;
        ctx.n_in = 1;
        ctx.in[0] =
            Desc{storage.data() + offset, static_cast<int64_t>(values.size())};
        ctx.out = Desc{&got, 1};
        ctx.variant = static_cast<uint8_t>(variant);
        extrema.forward(ctx);
        if (!same_reduction_value(got, wants[variant])) {
          ++failures;
          std::printf(
              "FAIL extrema kernel case %zu variant %d offset %d: got %llx "
              "want %llx\n",
              c, variant, offset,
              static_cast<unsigned long long>(reduction_bits(got)),
              static_cast<unsigned long long>(reduction_bits(wants[variant])));
        }
      }

      std::vector<double> active_values = values;
      Eigen::Matrix<stan::math::var, -1, 1> active(
          static_cast<Eigen::Index>(values.size()));
      for (size_t i = 0; i < values.size(); ++i)
        active(static_cast<Eigen::Index>(i)) = values[i];
      stan::math::var want_active =
          variant == 0 ? stan::math::min(active) : stan::math::max(active);
      double got_active = 0.0;
      double selected = 0.0;
      std::vector<double> adj(values.size(), 0.0);
      KernelCtx active_ctx;
      active_ctx.n_in = 1;
      active_ctx.in[0] =
          Desc{active_values.data(), static_cast<int64_t>(values.size())};
      active_ctx.out = Desc{&got_active, 1};
      active_ctx.scratch = &selected;
      active_ctx.variant = static_cast<uint8_t>(variant | 2);
      extrema.forward(active_ctx);
      active_ctx.in_adj[0] = Desc{adj.data(), static_cast<int64_t>(adj.size())};
      active_ctx.out_adj = 1.0;
      extrema.backward(active_ctx);
      want_active.grad();
      if (!same_reduction_value(got_active, want_active.val())) {
        ++failures;
        std::printf("FAIL active extrema value case %zu variant %d\n", c,
                    variant);
      }
      for (size_t i = 0; i < adj.size(); ++i) {
        if (adj[i] == active(static_cast<Eigen::Index>(i)).adj()) continue;
        ++failures;
        std::printf(
            "FAIL active extrema adjoint case %zu variant %d index %zu\n", c,
            variant, i);
      }
      stan::math::recover_memory();
    }
  }

  // The generic interpreter keeps scalar-fold behavior outside the audited
  // direct write_array surface, except that Stan's defined empty-container
  // results/errors must not become an out_of_range accident.
  std::map<std::string, const mir::FunDef*> funs;
  MirInterp<double> interp(funs, "empty extrema legacy test");
  mir::Expr arg;
  arg.kind = mir::Expr::Var;
  arg.name = "a";
  arg.type_ = "(UArray UReal)";
  arg.unsized.leaf = mir::UnsizedLeaf::Real;
  arg.unsized.depth = 1;
  mir::Expr call;
  call.kind = mir::Expr::FunApp;
  call.name = "min";
  call.fn_lib = mir::Expr::Lib::StanLib;
  call.type_ = "UReal";
  call.unsized.leaf = mir::UnsizedLeaf::Real;
  call.args = {arg};
  DataMap::Entry empty_real;
  empty_real.dims = {0};
  interp.env()["a"] = empty_real;
  if (reduction_bits(interp.eval(call).r.at(0)) != reduction_bits(inf)) {
    ++failures;
    std::printf("FAIL empty real legacy min is not +infinity\n");
  }
  call.name = "max";
  if (reduction_bits(interp.eval(call).r.at(0)) != reduction_bits(-inf)) {
    ++failures;
    std::printf("FAIL empty real legacy max is not -infinity\n");
  }
  arg.type_ = "(UArray UInt)";
  arg.unsized.leaf = mir::UnsizedLeaf::Int;
  call.args = {arg};
  DataMap::Entry empty_int;
  empty_int.is_int = true;
  empty_int.dims = {0};
  interp.env()["a"] = empty_int;
  for (const char* name : {"min", "max"}) {
    call.name = name;
    bool invalid = false;
    try {
      (void)interp.eval(call);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    if (!invalid) {
      ++failures;
      std::printf(
          "FAIL empty int legacy %s did not preserve invalid_argument\n", name);
    }
  }
}

int main() {
  test_matrix_transpose_add();
  using namespace stanli;
  test_expression_layout_policy();
  test_matrix_scalar_division();
  test_power_seed_grouping();
  test_reduction_overloads();
  test_product_exact_grouping();
  test_reduction_view_grouping();
  test_extrema_exact_grouping();
  const int N = 4;

  // Binary: all shape combos. Scalars are length-1 slots; the var reference
  // uses the scalar overloads (v(0)) exactly as generated C++ would.

  // ADD
  check_case("add vv", OP_ADD, N, {A, B}, [](auto& v) {
    return stan::math::sum(stan::math::add(v[0], v[1]));
  });
  check_case("add vs", OP_ADD, N, {A, {S}}, [](auto& v) {
    return stan::math::sum(stan::math::add(v[0], v[1](0)));
  });
  check_case("add sv", OP_ADD, N, {{S}, B}, [](auto& v) {
    return stan::math::sum(stan::math::add(v[0](0), v[1]));
  });
  check_case("add ss", OP_ADD, 1, {{S}, {T}},
             [](auto& v) { return v[0](0) + v[1](0); });
  // SUB
  check_case("sub vv", OP_SUB, N, {A, B}, [](auto& v) {
    return stan::math::sum(stan::math::subtract(v[0], v[1]));
  });
  check_case("sub vs", OP_SUB, N, {A, {S}}, [](auto& v) {
    return stan::math::sum(stan::math::subtract(v[0], v[1](0)));
  });
  check_case("sub sv", OP_SUB, N, {{S}, B}, [](auto& v) {
    return stan::math::sum(stan::math::subtract(v[0](0), v[1]));
  });
  check_case("sub ss", OP_SUB, 1, {{S}, {T}},
             [](auto& v) { return v[0](0) - v[1](0); });
  // MUL (vv = elt_multiply, matching EltTimes__)
  check_case("mul vv", OP_MUL, N, {A, B}, [](auto& v) {
    return stan::math::sum(stan::math::elt_multiply(v[0], v[1]));
  });
  check_case("mul vs", OP_MUL, N, {A, {S}}, [](auto& v) {
    return stan::math::sum(stan::math::multiply(v[0], v[1](0)));
  });
  check_case("mul sv", OP_MUL, N, {{S}, B}, [](auto& v) {
    return stan::math::sum(stan::math::multiply(v[0](0), v[1]));
  });
  check_case("mul ss", OP_MUL, 1, {{S}, {T}},
             [](auto& v) { return v[0](0) * v[1](0); });
  // DIV (vv = elt_divide, vs = divide)
  check_case("div vv", OP_DIV, N, {A, B}, [](auto& v) {
    return stan::math::sum(stan::math::elt_divide(v[0], v[1]));
  });
  check_case_params(
      "div vs", OP_DIV, N, {A, {T}}, {true, true},
      [](auto& v) {
        return stan::math::sum(stan::math::divide(v[0], v[1](0)));
      },
      0, kDivMatrixScalar | kDivMatrixActive | kDivScalarActive);
  check_case("div ss", OP_DIV, 1, {{S}, {T}},
             [](auto& v) { return v[0](0) / v[1](0); });
  check_case_params(
      "div scalar lanes", OP_DIV, N, {A, B}, {true, true},
      [](auto& v) {
        VecV out(v[0].size());
        for (int i = 0; i < out.size(); ++i) out[i] = v[0][i] / v[1][i];
        return stan::math::sum(out);
      },
      0, kDivScalarLanes);
  check_case_params(
      "div scalar broadcast lanes", OP_DIV, N, {A, {T}}, {true, true},
      [](auto& v) {
        VecV out(v[0].size());
        for (int i = 0; i < out.size(); ++i) out[i] = v[0][i] / v[1][0];
        return stan::math::sum(out);
      },
      0, kDivScalarLanes);
  // POW: all shape combos, like the binaries above. Bases stay positive so
  // fractional exponents remain in support on both sides of the comparison.
  const std::vector<double> P{0.5, 1.2, 2.0, 0.3};
  check_case("pow vv", OP_POW, N, {P, B}, [](auto& v) {
    return stan::math::sum(stan::math::pow(v[0], v[1]));
  });
  check_case("pow vs", OP_POW, N, {P, {T}}, [](auto& v) {
    return stan::math::sum(stan::math::pow(v[0], v[1](0)));
  });
  check_case("pow sv", OP_POW, N, {{S}, B}, [](auto& v) {
    return stan::math::sum(stan::math::pow(v[0](0), v[1]));
  });
  check_case("pow ss", OP_POW, 1, {{S}, {T}},
             [](auto& v) { return stan::math::pow(v[0](0), v[1](0)); });
  // pow at a base of exactly zero, one shape per guard site. Both partials
  // are nonfinite there -- b*v/a is 0/0, and log(a) is -inf meeting v = 0 --
  // and stan-math answers with a zero contribution instead, in all four of
  // its rev overloads. The bases carrying the zero sit next to nonzero
  // neighbours so the vector shapes cannot pass with a whole-op skip, and
  // the exponents opposite a zero base stay positive so the FORWARD stays
  // finite: this is a gradient bug, and 0^-1.7 = inf would hide it behind a
  // nonfinite lp. stanc3's pow.stan, validate_exponentiation_good.stan and
  // mem_patterns/ad_scalar_data_matrix.stan all reach this at the
  // all-zeros evaluation point, where CmdStan returns 0 and stanli did not.
  const std::vector<double> Z{0.0, 1.2, 2.0, 0.3};
  const std::vector<double> E{1.5, 0.7, 2.4, 2.2};
  const double U = 2.3;
  check_case("pow vv at zero", OP_POW, N, {Z, E}, [](auto& v) {
    return stan::math::sum(stan::math::pow(v[0], v[1]));
  });
  check_case("pow vs at zero", OP_POW, N, {Z, {U}}, [](auto& v) {
    return stan::math::sum(stan::math::pow(v[0], v[1](0)));
  });
  check_case("pow sv at zero", OP_POW, N, {{0.0}, E}, [](auto& v) {
    return stan::math::sum(stan::math::pow(v[0](0), v[1]));
  });
  check_case("pow ss at zero", OP_POW, 1, {{0.0}, {U}},
             [](auto& v) { return stan::math::pow(v[0](0), v[1](0)); });
  // The same zero base with a DATA exponent, where stan-math's non-var
  // dispatch reaches sqrt, the base itself, square, inv_square, inv or
  // inv_sqrt instead of the guard. Which of those three laws applies is the
  // op's variant, from the static types stanc3 emitted. The nonzero lanes
  // are powers of two so std::pow and the redirects agree to the bit there
  // and only the zero lane is under test.
  const std::vector<bool> data_exp{true, false};
  const std::vector<double> ZP{0.0, 4.0, 0.25, 16.0};
  const std::vector<double> DISPATCHED{1.0, 2.0, 0.5, -1.0, -2.0, -0.5, 3.0};
  for (const double y : DISPATCHED) {
    check_case_params(
        "pow ss zero scalar law " + std::to_string(y), OP_POW, 1, {{0.0}, {y}},
        data_exp,
        [](auto& v) { return stan::math::pow(v[0](0), v[1](0).val()); }, 0,
        stanli::kPowZeroBaseScalar);
    // An Eigen base with a scalar exponent takes the matrix redirects, whose
    // inv_square is prim's inv(square(x)) and is NaN rather than -inf at zero.
    check_case_params(
        "pow vs zero matrix law " + std::to_string(y), OP_POW, N, {ZP, {y}},
        data_exp,
        [](auto& v) {
          return stan::math::sum(stan::math::pow(v[0], v[1](0).val()));
        },
        0, stanli::kPowZeroBaseMatrix);
    // A widened lane keeps the scalar law: reroll turns a loop of scalar
    // `pow(param, y)` into a vector base against a vector data exponent, and
    // each of those lanes was its own scalar call.
    check_case_params(
        "pow vv zero scalar law " + std::to_string(y), OP_POW, N,
        {ZP, std::vector<double>(ZP.size(), y)}, data_exp,
        [](auto& v) {
          var s = 0.0;
          for (int i = 0; i < v[0].size(); ++i)
            s += stan::math::pow(v[0](i), v[1](i).val());
          return s;
        },
        0, stanli::kPowZeroBaseScalar);
    // Written as `pow(vector, vector)` it is stan-math's Eigen/Eigen overload,
    // which has no redirect and keeps the guard's zero.
    check_case_params(
        "pow vv zero eigen law " + std::to_string(y), OP_POW, N,
        {ZP, std::vector<double>(ZP.size(), y)}, data_exp,
        [](auto& v) {
          return stan::math::sum(
              stan::math::pow(v[0], stan::math::value_of(v[1]).eval()));
        },
        0, stanli::kPowZeroBaseGuarded);
  }
  check_case_params("pow ss at zero var exp 1", OP_POW, 1, {{0.0}, {1.0}},
                    {true, true},
                    [](auto& v) { return stan::math::pow(v[0](0), v[1](0)); });

  // Unaries, vector + scalar shapes.
  check_case("neg v", OP_NEG, N, {A},
             [](auto& v) { return stan::math::sum(stan::math::minus(v[0])); });
  check_case("exp v", OP_EXPV, N, {A},
             [](auto& v) { return stan::math::sum(stan::math::exp(v[0])); });
  check_case("exp s", OP_EXPV, 1, {{S}},
             [](auto& v) { return stan::math::exp(v[0](0)); });
  check_case(
      "log v", OP_LOGV, N, {{1.5, 0.7, 0.4, 2.2}},
      [](auto& v) { return stan::math::sum(stan::math::log(v[0])); }, 16);
  check_case("inv_logit v", OP_INV_LOGIT, N, {A}, [](auto& v) {
    return stan::math::sum(stan::math::inv_logit(v[0]));
  });
  // inv_logit is the one unary whose container and scalar overloads compute
  // different expressions, so its two shapes need values that separate them.
  // -0.45 and -1.53 are points where Eigen's PACKET exp disagrees with libm's
  // by a ulp -- they fail if the vector shape evaluates over contiguous
  // doubles instead of mirroring the strided (scalar-libm) `.val()` the
  // Matrix<var> overload gets. 1.1 and 2.0 are points where Eigen's logistic
  // functor, `e/(1+e)`, disagrees with stan's scalar `inv_logit`, `1/(1+e^-x)`
  // -- they fail if the vector shape is "simplified" to the scalar function.
  check_case(
      "inv_logit v ulp", OP_INV_LOGIT, N, {{-0.45, 2.0, 1.1, -1.53}},
      [](auto& v) { return stan::math::sum(stan::math::inv_logit(v[0])); });
  // And the mirror of that last point: at length 1 the reference IS the
  // scalar function, so 1.1 fails here if the two shapes are unified.
  check_case("inv_logit s", OP_INV_LOGIT, 1, {{1.1}},
             [](auto& v) { return stan::math::inv_logit(v[0](0)); });
  check_case("sqrt v", OP_SQRT, N, {{0.5, 1.2, 2.0, 0.3}},
             [](auto& v) { return stan::math::sum(stan::math::sqrt(v[0])); });
  // sqrt at exactly zero, both shapes. d/dx sqrt(x) is 1/(2 sqrt(x)), which
  // at x = 0 is a division by zero; stan-math's rev overload answers with a
  // zero contribution instead (rev/fun/sqrt.hpp: `if (vi.val() != 0.0)`),
  // and a kernel that divides anyway hands +inf back to whatever produced
  // the zero, where inf * 0 turns into NaN one op later. accel_gp reaches
  // this: its spd_cov_exp_quad underflows exp() to exact zero for the
  // largest Laplacian eigenvalue, and sqrt of that fed NaN into the
  // sdgp/lscale gradients. Nonzero neighbours in the same vector keep the
  // vector branch honest -- the guard has to be elementwise.
  check_case("sqrt v at zero", OP_SQRT, N, {{0.0, 1.2, 2.0, 0.3}},
             [](auto& v) { return stan::math::sum(stan::math::sqrt(v[0])); });
  check_case("sqrt s at zero", OP_SQRT, 1, {{0.0}},
             [](auto& v) { return stan::math::sqrt(v[0](0)); });
  check_case("square v", OP_SQUARE, N, {A},
             [](auto& v) { return stan::math::sum(stan::math::square(v[0])); });
  check_case("log1m v", OP_LOG1M, N, {{0.2, -0.5, 0.7, 0.05}},
             [](auto& v) { return stan::math::sum(stan::math::log1m(v[0])); });
  // DOT
  check_case("dot", OP_DOT, 1, {A, B},
             [](auto& v) { return stan::math::dot_product(v[0], v[1]); });

  test_gather_bwd_repeated_indices();
  test_gather_bwd_null_in_adj();

  // Reductions must preserve Stan Math's scalar/owning-Eigen grouping in the
  // value sweep and its exact reverse expression. Short examples do not cross
  // enough coefficients to distinguish prefix/suffix folds.
  std::vector<double> product_values(62);
  uint64_t reduction_state = 0x5052323535524544ULL;
  for (size_t i = 0; i < product_values.size(); ++i) {
    reduction_state =
        reduction_state * 6364136223846793005ULL + 1442695040888963407ULL;
    const double magnitude = 0.5 + static_cast<double>(reduction_state >> 11) /
                                       static_cast<double>(uint64_t{1} << 53);
    product_values[i] = i % 3 == 0 ? -magnitude : magnitude;
  }
  {
    constexpr double seed = 0x1.4bc8f7f1101b4p-1;
    Graph graph;
    const int input = graph.add_slot((int64_t)product_values.size(), true);
    const int product = graph.add_slot(1, false);
    const int weight = graph.add_slot(1, false);
    const int lp = graph.add_slot(1, false);
    graph.add_op(OP_PROD_VEC, {input}, product);
    graph.ops.back().variant = 2;
    graph.add_op(OP_MUL, {product, weight}, lp);
    graph.result_slot = lp;
    Executor ex(std::move(graph));
    std::copy(product_values.begin(), product_values.end(), ex.params_data());
    ex.value_ptr(weight)[0] = seed;
    std::vector<double> got(product_values.size());
    const double got_lp = ex.gradient(got.data());

    VecV reference((Eigen::Index)product_values.size());
    for (size_t i = 0; i < product_values.size(); ++i)
      reference((Eigen::Index)i) = product_values[i];
    var want_lp = seed * stan::math::prod(reference);
    want_lp.grad();
    expect_eq_or_both_nan("prod active reverse lp", got_lp, want_lp.val());
    for (size_t i = 0; i < got.size(); ++i)
      expect_eq_or_both_nan("prod active reverse g" + std::to_string(i), got[i],
                            reference((Eigen::Index)i).adj());
    stan::math::recover_memory();
  }

  const std::vector<double> dispersion_values = {
      -0x1.eed17f1e56cfap-1, 0x1.d7236aa8481ap-3,   -0x1.5739d23914f38p-3,
      0x1.f9947b4a8e84cp+0,  -0x1.939f5dd3c17fcp-2, 0x1.ac1c7d9bca5ap-3,
      -0x1.bdaa1148dc42p-5,  -0x1.878a0e048d69ap-1, 0x1.cde266be7caf8p+0,
      0x1.a516e5df7c134p-1,  0x1.fda3afe2a467p-3,   0x1.95f599d080ap-2,
      -0x1.8c365ff5309e8p-2, -0x1.6cc56196b8368p-1};
  check_case("sd packet reduction", OP_SD, 1, {dispersion_values},
             [](auto& v) { return stan::math::sd(v[0]); });

  const std::vector<double> variance_values = {
      -0x1.a1113046c0eb4p+0, 0x1.2b06fc967f838p-1,  -0x1.f1da834a6269p-4,
      0x1.1aff6118016c4p-1,  -0x1.0b5282797d2ep-1,  -0x1.4a8a06f6f264ap+0,
      -0x1.4c8ac5f162b16p-1, 0x1.de0b55a45268p-3,   -0x1.cf3f6d897a2dcp-2,
      -0x1.5e092e1d37172p+0, -0x1.481cade96bbd8p-1, -0x1.16dd852648f3cp-2,
      -0x1.183b2f928f00bp+0, 0x1.74ea191bb084ap+0,  -0x1.524fc26f53946p+0,
      0x1.2f60c1bb19678p-1,  -0x1.ea2fe6af70cbcp-1, 0x1.2ebd281cf3754p-1,
      -0x1.b24b43e8d3206p-1, -0x1.444a2d87e964cp+0, 0x1.cda5412561c6cp-1,
      0x1.367d5c0e9ebfap+0,  -0x1.07431f46256f8p+0, -0x1.99787f870601cp-1,
      0x1.f2244bd31308p-1,   0x1.4b5c2aebb7fap+0,   0x1.1862475d31d1p-3,
      -0x1.eb96cd19916d8p-3, 0x1.057f0309d7d18p-2,  -0x1.3c78760cc3ecp-4,
      -0x1.baf4364aba14dp+0, -0x1.210b1a49ba46p-1,  -0x1.9ad3b4faaf06cp-1,
      0x1.e59fc5c9cabb4p+0,  0x1.94c0e00da58fp-2,   -0x1.1c02689124baap-1,
      -0x1.abb727a329779p+0, 0x1.da16b74b11b6p-3,   -0x1.2cee8b6a8b83p-4,
      -0x1.6c530bf63304ep-1, 0x1.f20db2973a4fp-3,   0x1.6e9b7964687ecp-1,
      -0x1.5464a2a1801ap-4,  0x1.5ee91b82af3a8p-1,  -0x1.1875cb52a1ab3p+0};
  check_case("variance packet reduction", OP_VARIANCE, 1, {variance_values},
             [](auto& v) { return stan::math::variance(v[0]); });

  return finish("test_eltwise");
}
