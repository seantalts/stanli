// Batched OP_LSE2 / OP_LOG_MIX: any argument len 1 (broadcast) or len N,
// out len N, out[n] = f(args at n). Two obligations:
//
//  1. Value and gradient match the stan-math var reference (the C++ stanc3
//     would generate for the source loop), exactly.
//  2. The fused op is bit-identical to the unrolled graph it will replace
//     under re-rolling: INDEX -> scalar op -> SET_INDEX chain -> SUM_VEC.
//     This includes the broadcast-adjoint accumulation order.
#include "graph_helpers.hpp"

#include <stan/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace stanli;

static int failures = 0;
static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want && !(std::isnan(got) && std::isnan(want))) {
    ++failures;
    std::printf("FAIL %-28s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

static void expect_ulp(const std::string& what, double got, double want,
                       int max_ulp) {
  if (got == want || (std::isnan(got) && std::isnan(want))) return;
  double ulps = std::numeric_limits<double>::infinity();
  if (std::isfinite(got) && std::isfinite(want))
    ulps = std::abs(got - want) /
           std::max(std::nextafter(std::abs(want), 1e308) - std::abs(want),
                    std::numeric_limits<double>::denorm_min());
  if (ulps > (double)max_ulp) {
    ++failures;
    std::printf("FAIL %-28s got %.17g want %.17g (%.1f ulp)\n", what.c_str(),
                got, want, ulps);
  }
}

using stan::math::var;

static const std::vector<double> TH{0.35, 0.62, 0.18, 0.91};
static const std::vector<double> A{-1.3, 0.4, 2.1, -0.7};
static const std::vector<double> B{0.9, -2.2, 1.5, 0.1};
static const double S = 0.55, U = -0.8, W = 1.2;
// log_diff_exp(a, b) is real only for a > b.
static const std::vector<double> HI{3.2, 1.7, 4.5, 0.6};
static const std::vector<double> LO{-0.4, 1.1, 2.0, -3.3};
static const double SHI = 5.0;

// ---- 1. Against the var reference -----------------------------------------
// vals[k] is len 1 or len N; ref applies the scalar stan-math function per
// element (broadcasting len-1 args), accumulating lp ascending like SUM_VEC.
template <typename F>
static void check_ref(const std::string& tag, uint16_t opcode, int64_t out_len,
                      const std::vector<std::vector<double>>& vals, F&& f) {
  auto r = stanli::testutil::run_op_sum(opcode, out_len, vals,
                                        std::vector<bool>(vals.size(), true));
  std::vector<std::vector<var>> vs;
  for (const auto& v : vals) vs.emplace_back(v.begin(), v.end());
  var lp = 0.0;
  for (int64_t n = 0; n < out_len; ++n) {
    std::vector<var> a;
    for (auto& v : vs) a.push_back(v[v.size() == 1 ? 0 : n]);
    lp += f(a);
  }
  lp.grad();
  expect_eq(tag + " lp", r.value, lp.val());
  size_t gi = 0;
  for (auto& v : vs)
    for (auto& x : v)
      expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], x.adj()), ++gi;
  stan::math::recover_memory();
}

// ---- 1b. Per-element partials ----------------------------------------------
// A distinct output adjoint per element (OP_DOT against a weight vector)
// separates the elements: grad at a non-broadcast argument element n is
// w[n] * partial(n), so equal gradients are equal partials one by one.
static std::vector<double> run_weighted(
    uint16_t opcode, int64_t out_len,
    const std::vector<std::vector<double>>& vals,
    const std::vector<double>& w) {
  stanli::Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (const auto& v : vals) {
    slots.push_back(g.add_slot((int64_t)v.size(), true));
    n_par += (int64_t)v.size();
  }
  const int out = g.add_slot(out_len, false);
  const int ws = g.add_slot(out_len, false);
  const int lp = g.add_slot(1, false);
  stanli::Op op;
  op.opcode = opcode;
  op.out = out;
  for (int s : slots) op.in[op.n_in++] = s;
  g.ops.push_back(op);
  g.add_op(OP_DOT, {out, ws}, lp);
  g.result_slot = lp;

  stanli::Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i)
    for (size_t j = 0; j < vals[i].size(); ++j)
      ex.param_ptr(slots[i])[j] = vals[i][j];
  for (int64_t i = 0; i < out_len; ++i) ex.value_ptr(ws)[i] = w[(size_t)i];
  std::vector<double> grad((size_t)n_par, 0.0);
  ex.gradient(grad.data());
  return grad;
}

template <typename F>
static void check_partials(const std::string& tag, uint16_t opcode,
                           int64_t out_len,
                           const std::vector<std::vector<double>>& vals, F&& f,
                           int max_ulp = 0) {
  std::vector<double> w((size_t)out_len);
  for (int64_t i = 0; i < out_len; ++i) w[(size_t)i] = 0.75 - 0.5 * (double)i;
  const std::vector<double> got = run_weighted(opcode, out_len, vals, w);

  std::vector<std::vector<var>> vs;
  for (const auto& v : vals) vs.emplace_back(v.begin(), v.end());
  var lp = 0.0;
  for (int64_t n = 0; n < out_len; ++n) {
    std::vector<var> a;
    for (auto& v : vs) a.push_back(v[v.size() == 1 ? 0 : n]);
    lp += w[(size_t)n] * f(a);
  }
  lp.grad();
  size_t gi = 0;
  for (auto& v : vs)
    for (auto& x : v)
      expect_ulp(tag + " p" + std::to_string(gi), got[gi], x.adj(), max_ulp),
          ++gi;
  stan::math::recover_memory();
}

// ---- 2. Against the unrolled graph ----------------------------------------
// Reference: the shape lowering emits for
//   for (n in 1:N) outvec[n] = f(args...[n]);  target += sum(outvec);
// i.e. per element INDEX each vector arg, run the scalar op, SET_INDEX into
// a threaded vector; then SUM_VEC. Fused: one op with a len-N out, SUM_VEC.
struct GraphRun {
  double value;
  std::vector<double> out;  // the len-N intermediate
  std::vector<double> grad;
};

static GraphRun run_fused(uint16_t opcode, int64_t n,
                          const std::vector<std::vector<double>>& vals) {
  stanli::Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (const auto& v : vals) {
    slots.push_back(g.add_slot((int64_t)v.size(), true));
    n_par += (int64_t)v.size();
  }
  const int out = g.add_slot(n, false);
  const int lp = g.add_slot(1, false);
  stanli::Op op;
  op.opcode = opcode;
  op.out = out;
  for (int s : slots) op.in[op.n_in++] = s;
  g.ops.push_back(op);
  g.add_op(OP_SUM_VEC, {out}, lp);
  g.result_slot = lp;

  stanli::Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i)
    for (size_t j = 0; j < vals[i].size(); ++j)
      ex.value_ptr(slots[i])[j] = vals[i][j];
  GraphRun r;
  r.grad.assign(n_par, 0.0);
  r.value = ex.gradient(r.grad.data());
  r.out.assign(ex.value_ptr(out), ex.value_ptr(out) + n);
  return r;
}

static GraphRun run_unrolled(uint16_t opcode, int64_t n,
                             const std::vector<std::vector<double>>& vals) {
  stanli::Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (const auto& v : vals) {
    slots.push_back(g.add_slot((int64_t)v.size(), true));
    n_par += (int64_t)v.size();
  }
  int vec = g.add_slot(n, false);  // zeros; SET_INDEX chain threads through
  for (int64_t i = 0; i < n; ++i) {
    std::vector<int> args;
    for (int s : slots) {
      if (g.slots[s].len == 1) {
        args.push_back(s);
      } else {
        const int e = g.add_slot(1, false);
        g.add_op(OP_INDEX, {s}, e, {(int)i});
        args.push_back(e);
      }
    }
    const int fi = g.add_slot(1, false);
    stanli::Op op;
    op.opcode = opcode;
    op.out = fi;
    for (int s : args) op.in[op.n_in++] = s;
    g.ops.push_back(op);
    const int next = g.add_slot(n, false);
    g.add_op(OP_SET_INDEX, {vec, fi}, next, {(int)i});
    vec = next;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {vec}, lp);
  g.result_slot = lp;

  stanli::Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i)
    for (size_t j = 0; j < vals[i].size(); ++j)
      ex.value_ptr(slots[i])[j] = vals[i][j];
  GraphRun r;
  r.grad.assign(n_par, 0.0);
  r.value = ex.gradient(r.grad.data());
  r.out.assign(ex.value_ptr(vec), ex.value_ptr(vec) + n);
  return r;
}

static void check_bitwise(const std::string& tag, uint16_t opcode, int64_t n,
                          const std::vector<std::vector<double>>& vals) {
  const GraphRun f = run_fused(opcode, n, vals);
  const GraphRun u = run_unrolled(opcode, n, vals);
  expect_eq(tag + " lp", f.value, u.value);
  for (int64_t i = 0; i < n; ++i)
    expect_eq(tag + " out" + std::to_string(i), f.out[i], u.out[i]);
  for (size_t i = 0; i < f.grad.size(); ++i)
    expect_eq(tag + " g" + std::to_string(i), f.grad[i], u.grad[i]);
}

// ---- 3. Packed row-wise log_sum_exp ---------------------------------------
struct RowsRun {
  double value;
  std::vector<double> out;
  std::vector<double> grad;
};

static RowsRun run_rows(const std::vector<double>& x, int K,
                        const std::vector<double>& weights = {}) {
  const int64_t R = (int64_t)x.size() / K;
  Graph g;
  const int in = g.add_slot((int64_t)x.size(), true);
  const int out = g.add_slot(R, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP_ROWS, {in}, out, {K});
  int weight_slot = -1;
  if (weights.empty()) {
    g.add_op(OP_SUM_VEC, {out}, lp);
  } else {
    weight_slot = g.add_slot(R, false);
    g.add_op(OP_DOT, {out, weight_slot}, lp);
  }
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (size_t i = 0; i < x.size(); ++i) ex.param_ptr(in)[i] = x[i];
  for (size_t i = 0; i < weights.size(); ++i)
    ex.value_ptr(weight_slot)[i] = weights[i];
  RowsRun r;
  r.grad.resize(x.size());
  r.value = ex.gradient(r.grad.data());
  r.out.assign(ex.value_ptr(out), ex.value_ptr(out) + R);
  return r;
}

static void check_row_edge(const std::string& tag,
                           const std::vector<double>& x) {
  const RowsRun got = run_rows(x, (int)x.size());
  Eigen::Matrix<var, -1, 1> xv((int)x.size());
  for (int i = 0; i < (int)x.size(); ++i) xv(i) = x[(size_t)i];
  var want = stan::math::log_sum_exp(xv);
  want.grad();
  expect_eq(tag + " value", got.value, want.val());
  expect_eq(tag + " row value", got.out[0], want.val());
  for (int i = 0; i < (int)x.size(); ++i)
    expect_ulp(tag + " g" + std::to_string(i), got.grad[(size_t)i], xv(i).adj(),
               1);
  stan::math::recover_memory();
}

static void check_rows() {
  const int K = 5;
  const std::vector<double> x{-1.3,
                              0.4,
                              2.1,
                              -0.7,
                              0.2,  // ordinary
                              1000.0,
                              999.0,
                              998.0,
                              997.0,
                              996.0,  // stable at large magnitudes
                              -std::numeric_limits<double>::infinity(),
                              -2.0,
                              -3.0,
                              -4.0,
                              -5.0,
                              0.1,
                              0.1,
                              0.1,
                              0.1,
                              0.1};  // ties
  const int R = (int)x.size() / K;
  const RowsRun got = run_rows(x, K);

  Eigen::Matrix<var, -1, 1> xv((int)x.size());
  for (int i = 0; i < (int)x.size(); ++i) xv(i) = x[(size_t)i];
  std::vector<var> row_lp;
  row_lp.reserve((size_t)R);
  var lp = 0.0;
  for (int r = 0; r < R; ++r) {
    Eigen::Matrix<var, -1, 1> row(K);
    for (int k = 0; k < K; ++k) row(k) = xv(r * K + k);
    row_lp.push_back(stan::math::log_sum_exp(row));
    lp += row_lp.back();
  }
  lp.grad();

  expect_eq("rows lp", got.value, lp.val());
  for (int r = 0; r < R; ++r)
    expect_eq("rows out" + std::to_string(r), got.out[(size_t)r],
              row_lp[(size_t)r].val());
  for (int i = 0; i < (int)x.size(); ++i)
    expect_ulp("rows g" + std::to_string(i), got.grad[(size_t)i], xv(i).adj(),
               1);
  stan::math::recover_memory();

  // A nonuniform consumer verifies that backward uses each row's own output
  // adjoint rather than treating the rows as an implicit sum.
  const std::vector<double> weights{0.5, -1.25, 2.0, 0.0};
  const RowsRun weighted = run_rows(x, K, weights);
  Eigen::Matrix<var, -1, 1> wx((int)x.size());
  for (int i = 0; i < (int)x.size(); ++i) wx(i) = x[(size_t)i];
  var weighted_lp = 0.0;
  for (int r = 0; r < R; ++r) {
    Eigen::Matrix<var, -1, 1> row(K);
    for (int k = 0; k < K; ++k) row(k) = wx(r * K + k);
    weighted_lp += weights[(size_t)r] * stan::math::log_sum_exp(row);
  }
  weighted_lp.grad();
  for (int i = 0; i < (int)x.size(); ++i)
    expect_ulp("weighted rows g" + std::to_string(i), weighted.grad[(size_t)i],
               wx(i).adj(), 1);
  stan::math::recover_memory();

  Graph shape;
  const int in = shape.add_slot((int64_t)x.size(), true);
  const int out = shape.add_slot(R, false);
  const int oi = shape.add_op(OP_LOG_SUM_EXP_ROWS, {in}, out, {K});
  const Kernel* kernel = find_kernel(OP_LOG_SUM_EXP_ROWS);
  expect("rows kernel registered", kernel != nullptr);
  if (kernel) {
    expect("rows scratch callback", kernel->scratch_size != nullptr);
    if (kernel->scratch_size)
      expect("rows scratch=input len",
             kernel->scratch_size(shape.ops[(size_t)oi], shape.slots.data()) ==
                 (int64_t)x.size());
  }
  expect("rows backward is value-free",
         has_op_trait(OP_LOG_SUM_EXP_ROWS, op_trait::kBackwardValueFree));
  expect("rows opcode name", std::string(opcode_name(OP_LOG_SUM_EXP_ROWS)) ==
                                 "OP_LOG_SUM_EXP_ROWS");

  // Widths past Eigen's packet boundary, and one that is not a multiple of
  // it.
  std::vector<double> wide(16), odd(7);
  for (size_t i = 0; i < wide.size(); ++i)
    wide[i] = 0.37 * (double)i - 2.1 + 0.05 * (double)(i % 3);
  for (size_t i = 0; i < odd.size(); ++i) odd[i] = 1.9 - 0.61 * (double)i;
  check_row_edge("rows K=16", wide);
  check_row_edge("rows K=7", odd);
  {
    std::vector<double> many;
    for (int r = 0; r < 5; ++r)
      many.insert(many.end(), wide.begin(), wide.begin() + 9);
    for (size_t i = 0; i < many.size(); ++i) many[i] += 0.011 * (double)i;
    const RowsRun g9 = run_rows(many, 9);
    Eigen::Matrix<var, -1, 1> xv((int)many.size());
    for (int i = 0; i < (int)many.size(); ++i) xv(i) = many[(size_t)i];
    var lp9 = 0.0;
    std::vector<var> rows9;
    for (int r = 0; r < 5; ++r) {
      Eigen::Matrix<var, -1, 1> row(9);
      for (int k = 0; k < 9; ++k) row(k) = xv(r * 9 + k);
      rows9.push_back(stan::math::log_sum_exp(row));
      lp9 += rows9.back();
    }
    lp9.grad();
    for (int r = 0; r < 5; ++r)
      expect_eq("rows9 out" + std::to_string(r), g9.out[(size_t)r],
                rows9[(size_t)r].val());
    for (int i = 0; i < (int)many.size(); ++i)
      expect_ulp("rows9 g" + std::to_string(i), g9.grad[(size_t)i], xv(i).adj(),
                 1);
    stan::math::recover_memory();
  }

  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  check_row_edge("rows K=1", {-3.25});
  check_row_edge("rows all -inf", {-inf, -inf, -inf});
  check_row_edge("rows +inf", {inf, 0.5, inf});
  check_row_edge("rows nan", {-1.0, nan, 2.0});
}

int main() {
  const int64_t N = 4;

  // -- var reference, every shape combination --
  auto lse2 = [](const std::vector<var>& a) {
    return stan::math::log_sum_exp(a[0], a[1]);
  };
  check_ref("lse2 vv", OP_LSE2, N, {A, B}, lse2);
  check_ref("lse2 vs", OP_LSE2, N, {A, {S}}, lse2);
  check_ref("lse2 sv", OP_LSE2, N, {{S}, B}, lse2);
  check_ref("lse2 ss", OP_LSE2, 1, {{S}, {U}}, lse2);

  auto lmix = [](const std::vector<var>& a) {
    return stan::math::log_mix(a[0], a[1], a[2]);
  };
  check_ref("log_mix vvv", OP_LOG_MIX, N, {TH, A, B}, lmix);
  check_ref("log_mix svv", OP_LOG_MIX, N, {{S}, A, B}, lmix);
  check_ref("log_mix vss", OP_LOG_MIX, N, {TH, {U}, {W}}, lmix);
  check_ref("log_mix sss", OP_LOG_MIX, 1, {{S}, {U}, {W}}, lmix);

  auto ldiff = [](const std::vector<var>& a) {
    return stan::math::log_diff_exp(a[0], a[1]);
  };
  check_ref("log_diff_exp vv", OP_LOG_DIFF_EXP, N, {HI, LO}, ldiff);
  check_ref("log_diff_exp vs", OP_LOG_DIFF_EXP, N, {HI, {U}}, ldiff);
  check_ref("log_diff_exp sv", OP_LOG_DIFF_EXP, N, {{SHI}, LO}, ldiff);
  check_ref("log_diff_exp ss", OP_LOG_DIFF_EXP, 1, {{SHI}, {U}}, ldiff);

  // -- every partial, element by element --
  check_partials("lse2 vv", OP_LSE2, N, {A, B}, lse2);
  check_partials("lse2 vs", OP_LSE2, N, {A, {S}}, lse2);
  check_partials("lse2 sv", OP_LSE2, N, {{S}, B}, lse2);
  check_partials("lse2 ss", OP_LSE2, 1, {{S}, {U}}, lse2);
  check_partials("lse2 wide", OP_LSE2, N, {{1000.0, 999.0, -1e3, 0.1}, B},
                 lse2);
  check_partials("lse2 tie", OP_LSE2, N, {A, A}, lse2);

  check_partials("log_mix vvv", OP_LOG_MIX, N, {TH, A, B}, lmix);
  check_partials("log_mix svv", OP_LOG_MIX, N, {{S}, A, B}, lmix);
  check_partials("log_mix vsv", OP_LOG_MIX, N, {TH, {U}, B}, lmix);
  check_partials("log_mix vvs", OP_LOG_MIX, N, {TH, A, {W}}, lmix);
  check_partials("log_mix vss", OP_LOG_MIX, N, {TH, {U}, {W}}, lmix);
  check_partials("log_mix ssv", OP_LOG_MIX, N, {{S}, {U}, B}, lmix);
  check_partials("log_mix svs", OP_LOG_MIX, N, {{S}, A, {W}}, lmix);
  check_partials("log_mix sss", OP_LOG_MIX, 1, {{S}, {U}, {W}}, lmix);
  // theta at the ends of its bound, and lambda1 <= lambda2 (the other arm of
  // stan-math's partial helper).
  check_partials("log_mix edge", OP_LOG_MIX, N,
                 {{0.0, 1.0, 1e-12, 1.0 - 1e-12}, A, B}, lmix);
  check_partials("log_mix ordered", OP_LOG_MIX, N, {TH, B, A}, lmix);
  check_partials("log_mix tie", OP_LOG_MIX, N, {TH, A, A}, lmix);

  // stan-math's log_diff_exp chain divides the output adjoint rather than
  // scaling a partial, so a non-unit adjoint rounds one step differently.
  check_partials("log_diff_exp vv", OP_LOG_DIFF_EXP, N, {HI, LO}, ldiff, 1);
  check_partials("log_diff_exp vs", OP_LOG_DIFF_EXP, N, {HI, {U}}, ldiff, 1);
  check_partials("log_diff_exp sv", OP_LOG_DIFF_EXP, N, {{SHI}, LO}, ldiff, 1);
  check_partials("log_diff_exp ss", OP_LOG_DIFF_EXP, 1, {{SHI}, {U}}, ldiff, 1);

  // -- bit-identical to the unrolled loop it replaces --
  check_bitwise("unroll lse2 vv", OP_LSE2, N, {A, B});
  check_bitwise("unroll lse2 vs", OP_LSE2, N, {A, {S}});
  check_bitwise("unroll lmix svv", OP_LOG_MIX, N, {{S}, A, B});
  check_bitwise("unroll lmix vvv", OP_LOG_MIX, N, {TH, A, B});

  check_rows();

  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_mixture: all passed\n");
  return 0;
}
