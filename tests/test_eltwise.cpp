// Elementwise expression ops vs the stan-math var operations that stanc3's
// C++ backend would emit for the same MIR node. lp = sum(op(args)).
#include "graph_helpers.hpp"

#include <stanli/expression_layout.hpp>
#include <stanli/mir.hpp>
#include <stan/math.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got == want || (std::isnan(got) && std::isnan(want))) return;
  ++failures;
  std::printf("FAIL %-24s got %.17g want %.17g\n", what.c_str(), got, want);
}

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
        expect_eq("matrix transpose add value", output[i],
                  result.data()[i].val());
        expect_eq("matrix transpose add gradient", gradient[i],
                  a.data()[i].adj());
        expect_eq("matrix transpose add direct accumulation", other[i], 0);
      }
    }
  }
}

// OP_LOGV takes Eigen's packet log, a ulp off libm on some arguments. The
// gradients are 1/x and stay bitwise; only lp carries the difference, and a
// sum that cancels reports it as several ulps of the result.
static void expect_near_ulp(const std::string& what, double got, double want,
                            int budget) {
  if (got == want) return;
  const double u = std::nextafter(std::abs(want), 1e308) - std::abs(want);
  if (std::abs(got - want) <= budget * u) return;
  ++failures;
  std::printf("FAIL %-24s got %.17g want %.17g\n", what.c_str(), got, want);
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
  expect_near_ulp(tag + " lp", r.value, lp.val(), lp_ulp);
  size_t gi = 0;
  for (size_t k = 0; k < vs.size(); ++k) {
    if (!params[k]) continue;
    for (int i = 0; i < vs[k].size(); ++i)
      expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], vs[k](i).adj()),
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

static int64_t ulps(double a, double b) {
  if (a == b || (std::isnan(a) && std::isnan(b))) return 0;
  int64_t ia, ib;
  std::memcpy(&ia, &a, sizeof ia);
  std::memcpy(&ib, &b, sizeof ib);
  if ((ia < 0) != (ib < 0)) return INT64_MAX;
  const int64_t d = ia - ib;
  return d < 0 ? -d : d;
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
    for (int64_t i = 0; i < j; ++i) {
      const int64_t d = ulps(got[i], want[i]);
      if (d > budget) {
        ++failures;
        std::printf("FAIL %s g%lld got %.17g want %.17g (%lld ulps)\n", tag,
                    (long long)i, got[i], want[i], (long long)d);
      }
    }
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
          expect_eq("matrix/scalar division value", out[i], expected_out[i]);
          if (active_matrix)
            expect_eq("matrix/scalar division da", adj[i], a[i].adj());
        }
        if (active_scalar)
          expect_eq("matrix/scalar division db", scalar_adj, b.adj());
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
      expect_eq("power weighted value", output[i], result[i].val());
      expect_eq("power weighted base", da[i], av[i].adj());
      expect_eq("power weighted exponent", db[i], bv[i].adj());
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
    expect_eq("dot_self value grouping", output, result.val());
    for (int i = 0; i < n; ++i)
      expect_eq("dot_self gradient grouping", adj[i], active[i].adj());

    const Kernel* sum = find_kernel(OP_SUM_VEC);
    ctx.n_in = 1;
    ctx.variant = 1;  // owning Eigen double input
    sum->forward(ctx);
    Eigen::VectorXd owning = values.head(n);
    expect_eq("sum owning Eigen", output, stan::math::sum(owning));
    int offset = 1;
    ctx.in[0] = {values.data() + 1, n};
    ctx.variant = 2;
    ctx.idata = &offset;
    ctx.n_idata = 1;
    sum->forward(ctx);
    expect_eq("sum shifted Eigen", output,
              stan::math::sum(values.segment(1, n)));
  }
}

int main() {
  test_matrix_transpose_add();
  using namespace stanli;
  test_expression_layout_policy();
  test_matrix_scalar_division();
  test_power_seed_grouping();
  test_reduction_overloads();
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
    expect_eq("prod active reverse lp", got_lp, want_lp.val());
    for (size_t i = 0; i < got.size(); ++i)
      expect_eq("prod active reverse g" + std::to_string(i), got[i],
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

  if (failures == 0) std::printf("test_eltwise OK\n");
  return failures == 0 ? 0 : 1;
}
