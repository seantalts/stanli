// STANLI_SCALAR_BINARY_LIST ops vs the stan-math var overloads CmdStan's
// generated C++ would run: value and every gradient lane, bitwise, across
// the four shape combos (vv, vs, sv, ss). In-support inputs per function.
#include "graph_helpers.hpp"

#include <stanli/builtin_registry.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-28s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

using stan::math::var;

static void expect_bits(const std::string& what, double got, double want) {
  if ((std::isnan(got) && std::isnan(want)) ||
      std::memcmp(&got, &want, sizeof(double)) == 0)
    return;
  ++failures;
  std::printf("FAIL %-28s got %a want %a\n", what.c_str(), got, want);
}

// Exercise arbitrary upstream seeds and mixed activity through Stan's own
// weighted expression. An outer tape must survive both success and rejection;
// kernels must accumulate into existing adjoints, including aliased slots.
template <bool A0, bool A1, typename F>
static void check_seeded_scalar(const std::string& tag, uint16_t opcode,
                                double av, double bv, double seed, bool alias,
                                F&& f) {
  stan::math::nested_rev_autodiff outer;
  var sentinel = 2.0;
  var outer_result = sentinel * sentinel;
  double want_a = -0.0, want_b = -0.0;
  bool ref_threw = false;
  try {
    stan::math::nested_rev_autodiff reference;
    using T0 = std::conditional_t<A0, var, double>;
    using T1 = std::conditional_t<A1, var, double>;
    const T0 a(av);
    const T1 b(bv);
    var y = f(a, b);
    var weighted = 0.0;
    weighted += y * seed;
    stan::math::grad(weighted.vi_);
    if constexpr (A0) want_a += a.adj();
    if constexpr (A1) {
      if (alias)
        want_a += b.adj();
      else
        want_b += b.adj();
    }
  } catch (const std::domain_error&) {
    ref_threw = true;
  }
  double got_a = -0.0, got_b = -0.0, out = 0;
  stanli::KernelCtx ctx;
  ctx.n_in = 2;
  ctx.in[0] = {&av, 1};
  ctx.in[1] = {&bv, 1};
  ctx.in_adj[0] = {A0 ? &got_a : nullptr, 1};
  ctx.in_adj[1] = {A1 ? (alias ? &got_a : &got_b) : nullptr, 1};
  ctx.out = {&out, 1};
  ctx.out_adj = seed;
  bool got_threw = false;
  try {
    stanli::kernel(opcode).backward(ctx);
  } catch (const std::domain_error&) {
    got_threw = true;
  }
  expect_eq(tag + " rejection", got_threw, ref_threw);
  if (!ref_threw && !got_threw) {
    expect_bits(tag + " da", got_a, want_a);
    expect_bits(tag + " db", got_b, want_b);
    // Existing nonzero adjoints must accumulate on a subsequent invocation.
    if (std::isfinite(got_a) && std::isfinite(got_b)) {
      got_a = 0.25;
      got_b = -0.75;
      stanli::kernel(opcode).backward(ctx);
      // The alias case rounds twice, so its zero-seed oracle above is the
      // relevant contract; check nonzero accumulation with separate slots.
      if (!alias) {
        expect_bits(tag + " accumulate da", got_a, 0.25 + want_a);
        expect_bits(tag + " accumulate db", got_b, -0.75 + want_b);
      }
    }
  }
  expect_bits(tag + " outer untouched", sentinel.adj(), 0.0);
  stan::math::grad(outer_result.vi_);
  expect_bits(tag + " outer usable", sentinel.adj(), 4.0);
}

template <typename F>
static void check_seeds(const std::string& name, uint16_t opcode, double a,
                        double b, F&& f) {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (double seed : {1.0, -0.7, 0.0, -0.0, inf, -inf, nan}) {
    check_seeded_scalar<true, true>(name + " vv", opcode, a, b, seed, false, f);
    check_seeded_scalar<true, false>(name + " vd", opcode, a, b, seed, false,
                                     f);
    check_seeded_scalar<false, true>(name + " dv", opcode, a, b, seed, false,
                                     f);
    check_seeded_scalar<true, true>(name + " alias", opcode, a, a, seed, true,
                                    f);
  }
}

template <bool A0, bool A1, typename F>
static void check_weighted_shape(const std::string& tag, uint16_t opcode,
                                 std::vector<double> av, std::vector<double> bv,
                                 const std::vector<double>& seeds, F&& f) {
  stan::math::nested_rev_autodiff nested;
  using T0 = std::conditional_t<A0, var, double>;
  using T1 = std::conditional_t<A1, var, double>;
  std::vector<T0> a(av.begin(), av.end());
  std::vector<T1> b(bv.begin(), bv.end());
  std::vector<var> outputs;
  for (size_t i = 0; i < seeds.size(); ++i)
    outputs.push_back(f(a[av.size() == 1 ? 0 : i], b[bv.size() == 1 ? 0 : i]));
  var weighted = 0.0;
  for (size_t i = 0; i < seeds.size(); ++i) weighted += outputs[i] * seeds[i];
  stan::math::grad(weighted.vi_);
  std::vector<double> ga(av.size(), -0.0), gb(bv.size(), -0.0);
  std::vector<double> out(seeds.size());
  stanli::KernelCtx ctx;
  ctx.n_in = 2;
  ctx.in[0] = {av.data(), static_cast<int64_t>(av.size())};
  ctx.in[1] = {bv.data(), static_cast<int64_t>(bv.size())};
  ctx.in_adj[0] = {A0 ? ga.data() : nullptr, ctx.in[0].len};
  ctx.in_adj[1] = {A1 ? gb.data() : nullptr, ctx.in[1].len};
  ctx.out = {out.data(), static_cast<int64_t>(out.size())};
  ctx.out_adj_vec = {const_cast<double*>(seeds.data()), ctx.out.len};
  stanli::kernel(opcode).backward(ctx);
  if constexpr (A0)
    for (size_t i = 0; i < a.size(); ++i)
      expect_bits(tag + " da" + std::to_string(i), ga[i], 0.0 + a[i].adj());
  if constexpr (A1)
    for (size_t i = 0; i < b.size(); ++i)
      expect_bits(tag + " db" + std::to_string(i), gb[i], 0.0 + b[i].adj());
}

template <typename F>
static void check_shape(const std::string& tag, uint16_t opcode,
                        const std::vector<double>& av,
                        const std::vector<double>& bv, F&& f) {
  const int64_t n = std::max((int64_t)av.size(), (int64_t)bv.size());
  auto r = stanli::testutil::run_op_sum(opcode, n, {av, bv}, {true, true});
  // Reference: scalar var call per lane, scalars shared -- the graph
  // stan-math's own scalar overloads build for the same expression.
  std::vector<var> a, b;
  for (double x : av) a.emplace_back(x);
  for (double x : bv) b.emplace_back(x);
  var lp = 0.0;
  for (int64_t i = 0; i < n; ++i)
    lp += f(a[av.size() == 1 ? 0 : i], b[bv.size() == 1 ? 0 : i]);
  lp.grad();
  expect_eq(tag + " lp", r.value, lp.val());
  size_t gi = 0;
  for (const auto& x : a) {
    expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], x.adj());
    ++gi;
  }
  for (const auto& x : b) {
    expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], x.adj());
    ++gi;
  }
  stan::math::recover_memory();
}

template <typename F>
static void check_fn(const std::string& name, uint16_t opcode,
                     const std::vector<double>& av,
                     const std::vector<double>& bv, F&& f) {
  check_shape(name + " vv", opcode, av, bv, f);
  check_shape(name + " vs", opcode, av, {bv[0]}, f);
  check_shape(name + " sv", opcode, {av[0]}, bv, f);
  check_shape(name + " ss", opcode, {av[0]}, {bv[0]}, f);
}

// The int-argument half of the surface, STANLI_SCALAR_BINARY_INT_FIRST_LIST
// and its SECOND twin. stan-math takes the order/count/exponent as an int,
// so the reference passes an int too, and only the real side has a
// gradient: `iv` never appears in the expected adjoints.
template <typename F>
static void check_int_shape(const std::string& tag, uint16_t opcode,
                            bool int_first, const std::vector<double>& rv,
                            const std::vector<int>& iv, F&& f) {
  const int64_t n = std::max((int64_t)rv.size(), (int64_t)iv.size());
  // An int argument reaches a kernel through an ordinary double slot.
  const std::vector<double> id(iv.begin(), iv.end());
  auto r =
      int_first
          ? stanli::testutil::run_op_sum(opcode, n, {id, rv}, {false, true})
          : stanli::testutil::run_op_sum(opcode, n, {rv, id}, {true, false});
  std::vector<var> a;
  for (double x : rv) a.emplace_back(x);
  var lp = 0.0;
  for (int64_t i = 0; i < n; ++i)
    lp += f(a[rv.size() == 1 ? 0 : i], iv[iv.size() == 1 ? 0 : i]);
  lp.grad();
  expect_eq(tag + " lp", r.value, lp.val());
  for (size_t gi = 0; gi < a.size(); ++gi)
    expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], a[gi].adj());
  stan::math::recover_memory();
}

template <typename F>
static void check_int_fn(const std::string& name, uint16_t opcode,
                         bool int_first, const std::vector<double>& rv,
                         const std::vector<int>& iv, F&& f) {
  check_int_shape(name + " vv", opcode, int_first, rv, iv, f);
  check_int_shape(name + " vs", opcode, int_first, rv, {iv[0]}, f);
  check_int_shape(name + " sv", opcode, int_first, {rv[0]}, iv, f);
  check_int_shape(name + " ss", opcode, int_first, {rv[0]}, {iv[0]}, f);
}

int main() {
  using namespace stanli;
  const auto shape =
      [](BuiltinArgumentKind value, BuiltinContainerKind container,
         std::vector<int64_t> dimensions, int64_t size,
         BuiltinContainerKind leaf = BuiltinContainerKind::Scalar) {
        return BuiltinArgumentShape{value, container, leaf,
                                    std::move(dimensions), size};
      };
  const BuiltinSpec* choose_spec = builtin_spec("choose", 2);
  if (choose_spec == nullptr || choose_spec->opcode != OP_CHOOSE ||
      choose_spec->arguments[0] != BuiltinArgumentKind::Integer ||
      choose_spec->arguments[1] != BuiltinArgumentKind::Integer ||
      choose_spec->result != FunctionArgumentKind::Integer ||
      choose_spec->activity_mask != 0) {
    ++failures;
    std::printf("FAIL choose BuiltinSpec metadata\n");
  }
  {
    const BuiltinSpec* atan2_spec = builtin_spec("atan2", 2);
    const BuiltinLayout layout = builtin_layout(
        *atan2_spec,
        {shape(BuiltinArgumentKind::Real, BuiltinContainerKind::Scalar, {}, 1),
         shape(BuiltinArgumentKind::Real, BuiltinContainerKind::Matrix, {2, 3},
               6)});
    if (layout.lanes != 6 || layout.result_argument != 1) {
      ++failures;
      std::printf("FAIL scalar/matrix BuiltinLayout\n");
    }
    bool rejected = false;
    try {
      (void)builtin_layout(*atan2_spec,
                           {shape(BuiltinArgumentKind::Real,
                                  BuiltinContainerKind::Vector, {3}, 3),
                            shape(BuiltinArgumentKind::Real,
                                  BuiltinContainerKind::RowVector, {3}, 3)});
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    if (!rejected) {
      ++failures;
      std::printf("FAIL logical-shape mismatch accepted\n");
    }
  }
  {
    const BuiltinSpec* bessel = builtin_spec("bessel_first_kind", 2);
    const BuiltinLayout layout = builtin_layout(
        *bessel, {shape(BuiltinArgumentKind::Integer,
                        BuiltinContainerKind::Array, {2, 3}, 6),
                  shape(BuiltinArgumentKind::Real, BuiltinContainerKind::Matrix,
                        {2, 3}, 6)});
    if (layout.lanes != 6 || layout.result_argument != 1 ||
        layout.integer_matrix_rows != 2 || layout.integer_matrix_cols != 3) {
      ++failures;
      std::printf("FAIL mixed matrix/array BuiltinLayout\n");
    }
  }
  {
    const BuiltinSpec* softmax = builtin_spec("softmax", 1);
    const BuiltinLayout layout =
        builtin_layout(*softmax, {shape(BuiltinArgumentKind::Real,
                                        BuiltinContainerKind::Vector, {4}, 4)});
    if (layout.lanes != 4) {
      ++failures;
      std::printf("FAIL whole-value BuiltinLayout\n");
    }
  }
  {
    auto r = stanli::testutil::run_op_sum(
        OP_CHOOSE, 4, {{5, 6, 7, 8}, {0, 1, 2, 3}}, {false, false});
    const double want = stan::math::choose(5, 0) + stan::math::choose(6, 1) +
                        stan::math::choose(7, 2) + stan::math::choose(8, 3);
    expect_eq("choose integer kernel", r.value, want);
  }
#define F(fn) \
  [](const var& x, const var& y) -> var { return stan::math::fn(x, y); }

  // Free-sign pairs.
  const std::vector<double> xs{0.5, -1.2, 2.0, 0.3};
  const std::vector<double> ys{1.5, 0.7, -0.4, 2.2};
  // Positive pairs, for the log-domain functions.
  const std::vector<double> ps{0.9, 1.7, 0.35, 2.4};
  const std::vector<double> qs{1.1, 0.6, 2.2, 0.8};
  // n >= k >= 0, for lchoose.
  const std::vector<double> ns{7.5, 4.0, 9.25, 6.0};
  const std::vector<double> ks{2.5, 1.0, 3.0, 0.5};
  // a > b elementwise AND against each other's first element, so every
  // broadcast shape stays inside log_inv_logit_diff's support.
  const std::vector<double> hi{1.5, 0.7, 2.0, 2.2};
  const std::vector<double> lo{0.5, -1.2, -0.4, 0.3};

  check_fn("atan2", OP_ATAN2, xs, ys, F(atan2));
  check_fn("beta", OP_BETA_FN, ps, qs, F(beta));
  check_fn("fdim", OP_FDIM, xs, ys, F(fdim));
  check_fn("fmax", OP_FMAX, xs, ys, F(fmax));
  check_fn("fmin", OP_FMIN, xs, ys, F(fmin));
  check_fn("fmod", OP_FMOD, xs, ys, F(fmod));
  check_fn("gamma_p", OP_GAMMA_P, ps, qs, F(gamma_p));
  check_fn("gamma_q", OP_GAMMA_Q, ps, qs, F(gamma_q));
  check_fn("hypot", OP_HYPOT, xs, ys, F(hypot));
  check_fn("lbeta", OP_LBETA, ps, qs, F(lbeta));
  check_fn("lchoose", OP_LCHOOSE, ns, ks, F(binomial_coefficient_log));
  check_fn("lmultiply", OP_LMULTIPLY, xs, qs, F(lmultiply));
  check_fn("log_falling_factorial", OP_LOG_FALLING_FACTORIAL, ns, ks,
           F(log_falling_factorial));
  check_fn("log_inv_logit_diff", OP_LOG_INV_LOGIT_DIFF, hi, lo,
           F(log_inv_logit_diff));
  check_fn("log_modified_bessel_first_kind", OP_LOG_MODIFIED_BESSEL_1, ps, qs,
           F(log_modified_bessel_first_kind));
  check_fn("log_rising_factorial", OP_LOG_RISING_FACTORIAL, ps, qs,
           F(log_rising_factorial));
  check_fn("owens_t", OP_OWENS_T, xs, ys, F(owens_t));
#undef F

  // All affected functions, all activity masks, non-unit/non-finite seeds.
  // Some functions reject this common input: rejection and tape recovery
  // must agree too. The in-support shape checks above remain independent.
#define SEEDED(code, name, fn)                                                 \
  check_seeds(#name, code, 1.5, 0.5, [](const auto& a, const auto& b) -> var { \
    return stan::math::fn(a, b);                                               \
  });
  STANLI_SCALAR_BINARY_LIST(SEEDED)
#undef SEEDED
  // Non-unit seeds must reach shared broadcast vars in reverse lane order.
  // Selection functions can return that same var directly for several lanes;
  // these weights distinguish reverse accumulation from a forward loop.
  const std::vector<double> weights{1e16, 1.0, -1e16, 0.5};
  const auto maximum = [](const auto& a, const auto& b) -> var {
    return stan::math::fmax(a, b);
  };
  const auto lmultiply = [](const auto& a, const auto& b) -> var {
    return stan::math::lmultiply(a, b);
  };
  check_weighted_shape<true, false>("fmax shared output", OP_FMAX, {2.0},
                                    {-1.0, -2.0, -3.0, -4.0}, weights, maximum);
  check_weighted_shape<false, true>("fmax shared right", OP_FMAX,
                                    {-1.0, -2.0, -3.0, -4.0}, {2.0}, weights,
                                    maximum);
  check_weighted_shape<true, true>("lmultiply broadcast", OP_LMULTIPLY, {1.5},
                                   {0.5, 1.0, 2.0, 3.0}, weights, lmultiply);
  check_weighted_shape<true, false>("lmultiply data", OP_LMULTIPLY,
                                    {1.5, 1.0, 2.0, 0.5}, {2.0}, weights,
                                    lmultiply);
  check_weighted_shape<false, true>("lmultiply parameter", OP_LMULTIPLY,
                                    {1.5, 1.0, 2.0, 0.5}, {2.0}, weights,
                                    lmultiply);
  check_weighted_shape<true, true>("empty output", OP_LMULTIPLY, {}, {}, {},
                                   lmultiply);
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const auto& values :
       std::vector<std::pair<double, double>>{{0.0, 0.0},
                                              {-0.0, 0.0},
                                              {1.0, 0.0},
                                              {1.0, inf},
                                              {inf, 2.0},
                                              {nan, 2.0},
                                              {2.0, nan},
                                              {2.0, -1.0}}) {
    check_seeds("lmultiply edge", OP_LMULTIPLY, values.first, values.second,
                [](const auto& a, const auto& b) -> var {
                  return stan::math::lmultiply(a, b);
                });
    check_seeds("fmax edge", OP_FMAX, values.first, values.second,
                [](const auto& a, const auto& b) -> var {
                  return stan::math::fmax(a, b);
                });
  }

  // The int-argument functions. Domains: the second-kind Bessels and
  // lmgamma need a positive argument (lmgamma a positive one past
  // (k-1)/2), binary_log_loss a probability and a 0/1 outcome, and both
  // factorials a nonnegative count.
#define FI(fn) [](const var& x, int k) -> var { return stan::math::fn(k, x); }
  const std::vector<int> orders{0, 1, 2, 3};
  check_int_fn("bessel_first_kind", OP_BESSEL_1, true, xs, orders,
               FI(bessel_first_kind));
  check_int_fn("bessel_second_kind", OP_BESSEL_2, true, ps, orders,
               FI(bessel_second_kind));
  check_int_fn("modified_bessel_first_kind", OP_MODIFIED_BESSEL_1, true, xs,
               orders, FI(modified_bessel_first_kind));
  check_int_fn("modified_bessel_second_kind", OP_MODIFIED_BESSEL_2, true, ps,
               orders, FI(modified_bessel_second_kind));
  check_int_fn("binary_log_loss", OP_BINARY_LOG_LOSS, true,
               {0.2, 0.6, 0.35, 0.8}, {0, 1, 1, 0}, FI(binary_log_loss));
  check_int_fn("lmgamma", OP_LMGAMMA, true, {0.9, 1.7, 1.35, 2.4}, {1, 2, 1, 2},
               FI(lmgamma));
#undef FI
#define FI(fn) [](const var& x, int k) -> var { return stan::math::fn(x, k); }
  const std::vector<int> counts{0, 1, 2, 3};
  check_int_fn("falling_factorial", OP_FALLING_FACTORIAL, false, ps, counts,
               FI(falling_factorial));
  check_int_fn("rising_factorial", OP_RISING_FACTORIAL, false, ps, counts,
               FI(rising_factorial));
  check_int_fn("ldexp", OP_LDEXP, false, xs, {-2, 0, 1, 5}, FI(ldexp));
#undef FI

  // A matrix against an int array pairs n[i][j] with m(i, j), which is not
  // the flat pairing: the matrix is column-major and the array's trailing
  // extents are row-major. The lowering hands the kernel the leaf's rows
  // and cols to undo that (IntLane in kernels/scalar_binary.cpp). Distinct
  // exponents make a swapped pairing a factor of two, and the oracle is
  // stan-math's own (matrix, nested int vector) overload.
  {
    const std::vector<double> mcol{0.5, -1.25, 2.5, 0.75};  // column-major
    const std::vector<double> nrow{1, 3, -2, 4};            // row-major
    auto r = stanli::testutil::run_op_sum(OP_LDEXP, 4, {mcol, nrow},
                                          {true, false}, {2, 2});
    Eigen::Matrix<var, -1, -1> m(2, 2);
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i) m(i, j) = mcol[j * 2 + i];
    const std::vector<std::vector<int>> nn{{1, 3}, {-2, 4}};
    auto out = stan::math::ldexp(m, nn);
    // Column-major, the order the graph's OP_SUM_VEC walks its lanes.
    var lp = 0.0;
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i) lp += out(i, j);
    lp.grad();
    expect_eq("ldexp matrix-int lp", r.value, lp.val());
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i)
        expect_eq("ldexp matrix-int g" + std::to_string(j * 2 + i),
                  r.grad[j * 2 + i], m(i, j).adj());
    stan::math::recover_memory();
  }

  // A data argument must reach stan-math as a double, not a promoted var:
  // the var,double overloads of fmax and fmin give TIES to the var side,
  // and the var,var overloads give them to b. The conformance sweep hit
  // this through int arrays (int slots are always data) probed at exact
  // ties: fmax(2 + eps*theta, 2) at theta = 0.
  {
    auto r =
        stanli::testutil::run_op_sum(OP_FMAX, 1, {{2.0}, {2.0}}, {true, false});
    var a = 2.0;
    var lp = stan::math::fmax(a, 2.0);
    lp.grad();
    expect_eq("fmax tie-vs-data lp", r.value, lp.val());
    expect_eq("fmax tie-vs-data g0", r.grad[0], a.adj());
    stan::math::recover_memory();
  }
  {
    auto r =
        stanli::testutil::run_op_sum(OP_FMIN, 1, {{2.0}, {2.0}}, {true, false});
    var a = 2.0;
    var lp = stan::math::fmin(a, 2.0);
    lp.grad();
    expect_eq("fmin tie-vs-data lp", r.value, lp.val());
    expect_eq("fmin tie-vs-data g0", r.grad[0], a.adj());
    stan::math::recover_memory();
  }
  {
    // Data on the left, parameter on the right: dv overloads.
    auto r =
        stanli::testutil::run_op_sum(OP_FMAX, 1, {{2.0}, {2.0}}, {false, true});
    var b = 2.0;
    var lp = stan::math::fmax(2.0, b);
    lp.grad();
    expect_eq("fmax data-tie-v lp", r.value, lp.val());
    expect_eq("fmax data-tie-v g0", r.grad[0], b.adj());
    stan::math::recover_memory();
  }

  if (failures == 0) std::printf("test_scalar_binary OK\n");
  return failures == 0 ? 0 : 1;
}
