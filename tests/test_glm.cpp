// Logistic GLM: graph log_prob + gradient vs an all-var reference in the
// same op order, at three fixed parameter vectors. case0-2 (OP_MATVEC): 10
// ULP. The rest: bitwise.
#include "expect.hpp"
#include "models.hpp"
#include <stanli/density_registry.hpp>
#include <stanli/program.hpp>

#include <stan/math.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// The three direct GLM kernels do not pass through the generated scalar
// density wrappers. Empty outcomes return zero before Stan Math builds a
// propagator, so each must still preserve the returned value and disconnected
// topology through its own recorder call.
static void check_empty_glm(uint16_t opcode, const std::string& name) {
  using namespace stanli;
  using stan::math::var;
  constexpr int K = 2;

  Graph g;
  const int X_slot = g.add_slot(0, false);
  const int alpha_slot = g.add_slot(1, true);
  const int beta_slot = g.add_slot(K, true);
  const bool has_phi = opcode == OP_NEG_BINOMIAL_2_LOG_GLM_LPMF;
  const int phi_slot = has_phi ? g.add_slot(1, true) : -1;
  const int infinity_slot = g.add_slot(1, false);
  const int lp_slot = g.add_slot(1, false);
  const int scaled_slot = g.add_slot(1, false);
  if (has_phi) {
    g.add_op(opcode, {X_slot, alpha_slot, beta_slot, phi_slot}, lp_slot,
             {0, K, 0});
  } else {
    g.add_op(opcode, {X_slot, alpha_slot, beta_slot}, lp_slot, {0, K, 0});
  }
  g.add_op(OP_MUL, {lp_slot, infinity_slot}, scaled_slot);
  g.result_slot = scaled_slot;

  Executor ex(std::move(g));
  ex.value_ptr(infinity_slot)[0] = std::numeric_limits<double>::infinity();
  ex.params_data()[0] = 0.4;
  ex.params_data()[1] = -0.3;
  ex.params_data()[2] = 0.8;
  if (has_phi) ex.params_data()[3] = 1.7;
  std::vector<double> grad(has_phi ? 4 : 3, 0.0);
  const double value = ex.gradient(grad.data());

  const std::vector<int> y;
  const Eigen::Matrix<double, -1, -1> X(0, K);
  var alpha = 0.4;
  Eigen::Matrix<var, -1, 1> beta(K);
  beta << -0.3, 0.8;
  var phi = 1.7;
  var lp_ref;
  if (opcode == OP_BERNOULLI_LOGIT_GLM_LPMF) {
    lp_ref = stan::math::bernoulli_logit_glm_lpmf<false>(y, X, alpha, beta);
  } else if (opcode == OP_POISSON_LOG_GLM_LPMF) {
    lp_ref = stan::math::poisson_log_glm_lpmf<false>(y, X, alpha, beta);
  } else {
    lp_ref =
        stan::math::neg_binomial_2_log_glm_lpmf<false>(y, X, alpha, beta, phi);
  }
  var value_ref = lp_ref * std::numeric_limits<double>::infinity();
  value_ref.grad();
  expect_eq_or_both_nan(name + " empty value", value, value_ref.val());
  expect_eq_or_both_nan(name + " empty alpha", grad[0], alpha.adj());
  for (int k = 0; k < K; ++k)
    expect_eq_or_both_nan(name + " empty beta" + std::to_string(k), grad[1 + k],
                          beta(k).adj());
  if (has_phi) expect_eq_or_both_nan(name + " empty phi", grad[3], phi.adj());
  stan::math::recover_memory();
}

// A per-row intercept: brms writes one whenever the model has a group-level
// term, and stan-math takes alpha as a vector there. The design matrix is
// also an ordinary differentiable argument, and a model may build it from
// parameters. Dropping its pullback surfaced as a 2-3.5% CmdStan gradient
// divergence in the signature-reference gate: the kernels bound X as a plain
// double map, so d(lp)/dX was silently zero while alpha and beta stayed exact.
static void check_glm(const std::string& tag, uint16_t opcode, bool propto,
                      int alpha_len, bool x_active) {
  using namespace stanli;
  using stan::math::var;
  const int rows = 6, cols = 3;
  const bool has_phi = opcode == OP_NEG_BINOMIAL_2_LOG_GLM_LPMF;
  const bool bern = opcode == OP_BERNOULLI_LOGIT_GLM_LPMF;

  std::vector<double> X((size_t)rows * cols), a((size_t)alpha_len),
      b((size_t)cols);
  for (int j = 0; j < cols; ++j)
    for (int i = 0; i < rows; ++i)
      X[(size_t)j * rows + i] = std::sin(0.31 * i + 0.7 * j);
  for (int i = 0; i < alpha_len; ++i) a[(size_t)i] = 0.15 - 0.08 * i;
  for (int i = 0; i < cols; ++i) b[(size_t)i] = 0.2 + 0.11 * i;
  std::vector<int> idata;
  for (int i = 0; i < rows; ++i) idata.push_back(bern ? i % 2 : 1 + (i % 4));
  append_glm_layout(idata, rows, cols, 0);

  const double seed = -0.73;
  Graph g;
  const int Xs = g.add_slot(rows * cols, x_active);
  const int as = g.add_slot(alpha_len, true);
  const int bs = g.add_slot(cols, true);
  const int ps = has_phi ? g.add_slot(1, true) : -1;
  const int lp = g.add_slot(1, false);
  const int ss = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  const int op = has_phi ? g.add_op(opcode, {Xs, as, bs, ps}, lp, idata)
                         : g.add_op(opcode, {Xs, as, bs}, lp, idata);
  g.ops[(size_t)op].variant =
      (uint8_t)((propto ? 0x80u : 0u) | (has_phi ? 0x0eu : 0x06u) |
                (x_active ? 0x01u : 0u));
  g.add_op(OP_MUL, {lp, ss}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  std::copy(X.begin(), X.end(), x_active ? ex.param_ptr(Xs) : ex.value_ptr(Xs));
  std::copy(a.begin(), a.end(), ex.param_ptr(as));
  std::copy(b.begin(), b.end(), ex.param_ptr(bs));
  if (has_phi) ex.param_ptr(ps)[0] = 1.7;
  ex.value_ptr(ss)[0] = seed;
  std::vector<double> grad(
      (x_active ? X.size() : 0) + a.size() + b.size() + (has_phi ? 1 : 0), 0.0);
  const double got = ex.gradient(grad.data());

  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<double, -1, -1> Xd(rows, cols);
  Eigen::Matrix<var, -1, -1> Xv(rows, cols);
  for (size_t i = 0; i < X.size(); ++i) Xd.data()[i] = X[i];
  if (x_active)
    for (size_t i = 0; i < X.size(); ++i) Xv.data()[i] = X[i];
  Eigen::Matrix<var, -1, 1> av(alpha_len), bv(cols);
  for (int i = 0; i < alpha_len; ++i) av(i) = a[(size_t)i];
  for (int i = 0; i < cols; ++i) bv(i) = b[(size_t)i];
  var phi = 1.7;
  std::vector<int> y(idata.begin(), idata.begin() + rows);
  auto call = [&](const auto& x, const auto& alpha) {
    if (bern) {
      return propto
                 ? stan::math::bernoulli_logit_glm_lpmf<true>(y, x, alpha, bv)
                 : stan::math::bernoulli_logit_glm_lpmf<false>(y, x, alpha, bv);
    } else if (opcode == OP_POISSON_LOG_GLM_LPMF) {
      return propto ? stan::math::poisson_log_glm_lpmf<true>(y, x, alpha, bv)
                    : stan::math::poisson_log_glm_lpmf<false>(y, x, alpha, bv);
    }
    return propto ? stan::math::neg_binomial_2_log_glm_lpmf<true>(y, x, alpha,
                                                                  bv, phi)
                  : stan::math::neg_binomial_2_log_glm_lpmf<false>(y, x, alpha,
                                                                   bv, phi);
  };
  const auto with_alpha = [&](const auto& x) {
    return alpha_len == 1 ? call(x, av(0)) : call(x, av);
  };
  var ref = x_active ? with_alpha(Xv) : with_alpha(Xd);
  var scaled = ref * seed;
  stan::math::grad(scaled.vi_);
  expect_eq_or_both_nan(tag + " total", got, scaled.val());
  size_t at = 0;
  if (x_active)
    for (size_t i = 0; i < X.size(); ++i)
      expect_eq_or_both_nan(tag + " dX" + std::to_string(i), grad[at++],
                            Xv.data()[i].adj());
  for (int i = 0; i < alpha_len; ++i)
    expect_eq_or_both_nan(tag + " da" + std::to_string(i), grad[at++],
                          av(i).adj());
  for (int i = 0; i < cols; ++i)
    expect_eq_or_both_nan(tag + " db" + std::to_string(i), grad[at++],
                          bv(i).adj());
  if (has_phi) expect_eq_or_both_nan(tag + " dphi", grad[at], phi.adj());
}

static void check_glms() {
  using namespace stanli;
  const std::pair<const char*, uint16_t> glms[] = {
      {"bern glm", OP_BERNOULLI_LOGIT_GLM_LPMF},
      {"pois glm", OP_POISSON_LOG_GLM_LPMF},
      {"nb2 glm", OP_NEG_BINOMIAL_2_LOG_GLM_LPMF}};
  for (bool propto : {false, true}) {
    const std::string p = propto ? " propto" : "";
    for (const auto& [name, opcode] : glms)
      check_glm(std::string(name) + " valpha" + p, opcode, propto, 6, false);
  }
  for (bool propto : {false, true})
    for (int alpha_len : {1, 6}) {
      const std::string s = (propto ? std::string(" propto") : std::string()) +
                            (alpha_len == 1 ? "" : " valpha");
      for (const auto& [name, opcode] : glms)
        check_glm(std::string(name) + " dX" + s, opcode, propto, alpha_len,
                  true);
    }
}

// Compare the compact GLM kernels with a weighted nested-tape reference.
static void check_tail_glm(const std::string& tag, uint16_t opcode, bool propto,
                           const std::vector<int>& idata, int rows, int cols,
                           int alpha_len, int beta_len, double seed = -0.73,
                           bool scalar_y = false) {
  using namespace stanli;
  using stan::math::var;
  std::vector<double> X((size_t)rows * cols), a((size_t)alpha_len),
      b((size_t)beta_len);
  for (int j = 0; j < cols; ++j)
    for (int i = 0; i < rows; ++i)
      X[(size_t)j * rows + i] = std::sin(0.31 * i + 0.7 * j);
  for (int i = 0; i < alpha_len; ++i) a[(size_t)i] = 0.15 - 0.08 * i;
  for (int i = 0; i < beta_len; ++i) b[(size_t)i] = 0.2 + 0.11 * i;

  Graph g;
  const int Xs = g.add_slot(rows * cols, true);
  const int as = g.add_slot(alpha_len, true);
  const int bs = g.add_slot(beta_len, true);
  const int lp = g.add_slot(1, false);
  const int ss = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  const int op = g.add_op(opcode, {Xs, as, bs}, lp, idata);
  g.ops[(size_t)op].variant = propto ? 0x87u : 0x00u;
  g.add_op(OP_MUL, {lp, ss}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  std::copy(X.begin(), X.end(), ex.param_ptr(Xs));
  std::copy(a.begin(), a.end(), ex.param_ptr(as));
  std::copy(b.begin(), b.end(), ex.param_ptr(bs));
  ex.value_ptr(ss)[0] = seed;
  std::vector<double> grad(X.size() + a.size() + b.size(), 0.0);
  const double got = ex.gradient(grad.data());

  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<var, -1, -1> Xv(rows, cols);
  for (size_t i = 0; i < X.size(); ++i) Xv.data()[i] = X[i];
  Eigen::Matrix<var, -1, 1> av(alpha_len), bv(beta_len);
  for (int i = 0; i < alpha_len; ++i) av(i) = a[(size_t)i];
  for (int i = 0; i < beta_len; ++i) bv(i) = b[(size_t)i];
  var ref;
  if (opcode == OP_BINOMIAL_LOGIT_GLM_LPMF) {
    std::vector<int> nn(idata.begin(), idata.begin() + rows);
    std::vector<int> NN(idata.begin() + rows, idata.begin() + 2 * rows);
    auto call = [&](const auto& a) {
      return propto
                 ? stan::math::binomial_logit_glm_lpmf<true>(nn, NN, Xv, a, bv)
                 : stan::math::binomial_logit_glm_lpmf<false>(nn, NN, Xv, a,
                                                              bv);
    };
    ref = alpha_len == 1 ? call(av(0)) : call(av);
  } else if (opcode == OP_CATEGORICAL_LOGIT_GLM_LPMF) {
    std::vector<int> yy(idata.begin(), idata.begin() + rows);
    Eigen::Matrix<var, -1, -1> bm(cols, beta_len / cols);
    for (int i = 0; i < beta_len; ++i) bm.data()[i] = bv(i);
    auto call = [&](const auto& y) {
      return propto
                 ? stan::math::categorical_logit_glm_lpmf<true>(y, Xv, av, bm)
                 : stan::math::categorical_logit_glm_lpmf<false>(y, Xv, av, bm);
    };
    ref = scalar_y ? call(yy[0]) : call(yy);
    var scaled_cat = ref * seed;
    stan::math::grad(scaled_cat.vi_);
    if (rows <= kPlainSumMax) {
      expect_eq_or_both_nan(tag + " total", got, scaled_cat.val());
    } else {
      // Past kPlainSumMax rows the kernel adds the rows' log probabilities
      // with compensation rather than Stan Math's three sums.
      ExactSum rows_lp;
      for (int n = 0; n < rows; ++n) {
        Eigen::VectorXd lin(beta_len / cols);
        for (int j = 0; j < lin.size(); ++j) {
          double v = 0.0;
          for (int c = 0; c < cols; ++c)
            v += X[(size_t)c * rows + n] * b[(size_t)j * cols + c];
          lin(j) = v + a[(size_t)j];
        }
        rows_lp.add(stan::math::categorical_logit_lpmf(
            scalar_y ? yy[0] : yy[(size_t)n], lin));
      }
      const double want = rows_lp.value() * seed;
      if (std::isfinite(want) || std::isfinite(got))
        expect_ulp(tag + " total", got, want, 2);
      else
        expect_eq_or_both_nan(tag + " total", got, want);
    }
    size_t at = 0;
    for (size_t i = 0; i < X.size(); ++i)
      expect_eq_or_both_nan(tag + " dX" + std::to_string(i), grad[at++],
                            Xv.data()[i].adj());
    for (int i = 0; i < alpha_len; ++i)
      expect_eq_or_both_nan(tag + " da" + std::to_string(i), grad[at++],
                            av(i).adj());
    for (int i = 0; i < beta_len; ++i)
      expect_eq_or_both_nan(tag + " db" + std::to_string(i), grad[at++],
                            bm.data()[i].adj());
    expect_eq_or_both_nan(tag + " value-only", ex.forward_value_only(), got);
    std::vector<double> repeated(grad.size());
    expect_eq_or_both_nan(tag + " reused total", ex.gradient(repeated.data()),
                          got);
    for (size_t i = 0; i < grad.size(); ++i)
      expect_eq_or_both_nan(tag + " reused gradient", repeated[i], grad[i]);
    return;
  } else {
    std::vector<int> yy(idata.begin(), idata.begin() + rows);
    ref = propto ? stan::math::ordered_logistic_glm_lpmf<true>(yy, Xv, av, bv)
                 : stan::math::ordered_logistic_glm_lpmf<false>(yy, Xv, av, bv);
  }
  var scaled = ref * seed;
  stan::math::grad(scaled.vi_);
  expect_eq_or_both_nan(tag + " total", got, scaled.val());
  size_t at = 0;
  for (size_t i = 0; i < X.size(); ++i)
    expect_eq_or_both_nan(tag + " dX" + std::to_string(i), grad[at++],
                          Xv.data()[i].adj());
  for (int i = 0; i < alpha_len; ++i)
    expect_eq_or_both_nan(tag + " da" + std::to_string(i), grad[at++],
                          av(i).adj());
  for (int i = 0; i < beta_len; ++i)
    expect_eq_or_both_nan(tag + " db" + std::to_string(i), grad[at++],
                          bv(i).adj());
}

static void check_tail_glms() {
  using namespace stanli;
  const int rows = 5, cols = 3;
  std::vector<int> bin;
  for (int i = 0; i < rows; ++i) bin.push_back(1 + (i % 4));
  for (int i = 0; i < rows; ++i) bin.push_back(5 + i);
  append_glm_layout(bin, rows, cols, 0);
  check_tail_glm("binom glm", OP_BINOMIAL_LOGIT_GLM_LPMF, false, bin, rows,
                 cols, 1, cols);
  check_tail_glm("binom glm propto", OP_BINOMIAL_LOGIT_GLM_LPMF, true, bin,
                 rows, cols, 1, cols);
  check_tail_glm("binom glm valpha", OP_BINOMIAL_LOGIT_GLM_LPMF, false, bin,
                 rows, cols, rows, cols);
  check_tail_glm("binom glm valpha propto", OP_BINOMIAL_LOGIT_GLM_LPMF, true,
                 bin, rows, cols, rows, cols);

  const int cats = 3;
  std::vector<int> cat;
  for (int i = 0; i < rows; ++i) cat.push_back(1 + (i % cats));
  append_glm_layout(cat, rows, cols, 0);
  check_tail_glm("cat glm", OP_CATEGORICAL_LOGIT_GLM_LPMF, false, cat, rows,
                 cols, cats, cols * cats);
  check_tail_glm("cat glm propto", OP_CATEGORICAL_LOGIT_GLM_LPMF, true, cat,
                 rows, cols, cats, cols * cats);

  // Empty outcomes and one-category models disconnect the density; infinite
  // output weights must not invent a derivative connection. Scalar outcomes
  // retain their distinct Stan Math overload.
  for (int nr : {0, 1, 2, 40})
    for (int nc : {1, 3})
      for (int categories : {1, 2, 3, 8})
        for (bool propto : {false, true})
          for (bool scalar : {false, true}) {
            if (scalar && nr == 0) continue;
            std::vector<int> outcomes;
            for (int i = 0; i < nr; ++i)
              outcomes.push_back(scalar ? 1 : 1 + i % categories);
            append_glm_layout(outcomes, nr, nc,
                              scalar ? kGlmScalarFirstOutcome : 0);
            for (double weight : {0.0, -0.0, 1.0, -1.3, 1e308, 1e-308,
                                  std::numeric_limits<double>::infinity()})
              check_tail_glm("categorical recorder",
                             OP_CATEGORICAL_LOGIT_GLM_LPMF, propto, outcomes,
                             nr, nc, categories, nc * categories, weight,
                             scalar);
          }

  // in = {X, beta, cutpoints}: the cutpoints ride the `alpha` slot here and
  // must stay ordered.
  check_tail_glm("ord glm", OP_ORDERED_LOGISTIC_GLM_LPMF, false, cat, rows,
                 cols, cols, cats - 1);
  check_tail_glm("ord glm propto", OP_ORDERED_LOGISTIC_GLM_LPMF, true, cat,
                 rows, cols, cols, cats - 1);
  check_tail_glm("ord glm empty", OP_ORDERED_LOGISTIC_GLM_LPMF, false,
                 {0, cols, 0}, 0, cols, cols, cats - 1,
                 std::numeric_limits<double>::infinity());
}

// Reverse replay must accumulate into an adjoint already written by a
// later consumer in the same order as Stan's tape. Grouping the density's
// two logit contributions before adding that existing 0.1 changes one bit.
static void test_categorical_existing_adjoint() {
  using namespace stanli;
  Graph g;
  const int beta = g.add_slot(3, true);
  const int outcome = g.add_slot(1, false);
  const int cat = g.add_slot(1, false);
  const int cat_op = g.add_op(OP_CATEGORICAL, {outcome, beta}, cat);
  g.ops[(size_t)cat_op].variant =
      kCategoricalLogit | kCategoricalScalarOutcome | kCategoricalArgAutodiff;
  const int beta0 = g.add_slot(1, false);
  g.add_op(OP_INDEX, {beta}, beta0, {0});
  const int scale = g.add_slot(1, false);
  const int linear = g.add_slot(1, false);
  g.add_op(OP_MUL, {beta0, scale}, linear);
  const int total = g.add_slot(1, false);
  g.add_op(OP_ADD, {cat, linear}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  std::fill(ex.params_data(), ex.params_data() + 3, -2.0);
  ex.value_ptr(outcome)[0] = 1.0;
  ex.value_ptr(scale)[0] = 0.1;
  double got_grad[3];
  const double got = ex.gradient(got_grad);

  Eigen::Matrix<stan::math::var, -1, 1> ref_beta(3);
  ref_beta << -2.0, -2.0, -2.0;
  stan::math::var ref_cat =
      stan::math::categorical_logit_lpmf<false>(1, ref_beta);
  stan::math::var ref_linear = ref_beta(0) * 0.1;
  stan::math::var ref = ref_cat + ref_linear;
  ref.grad();
  expect_eq("categorical existing-adjoint value", got, ref.val());
  for (int i = 0; i < 3; ++i)
    expect_eq("categorical existing-adjoint g" + std::to_string(i), got_grad[i],
              ref_beta(i).adj());
  stan::math::recover_memory();
}

// The native scalar probability rule has one selected reciprocal. Pin its
// non-unit upstream seed, += into an adjoint a later consumer already
// wrote, propto/type behavior, and value-only -> gradient reuse against the
// exact Stan tape it replaces.
static void test_categorical_native_scalar() {
  using namespace stanli;
  Graph g;
  const int theta = g.add_slot(3, true);
  const int outcome = g.add_slot(1, false);
  const int cat = g.add_slot(1, false);
  const int cat_op = g.add_op(OP_CATEGORICAL, {outcome, theta}, cat);
  g.ops[(size_t)cat_op].variant =
      kCategoricalScalarOutcome | kCategoricalArgAutodiff | 0x80u;
  const int cat_scale = g.add_slot(1, false);
  const int scaled_cat = g.add_slot(1, false);
  g.add_op(OP_MUL, {cat, cat_scale}, scaled_cat);
  const int selected = g.add_slot(1, false);
  g.add_op(OP_INDEX, {theta}, selected, {1});
  const int linear_scale = g.add_slot(1, false);
  const int linear = g.add_slot(1, false);
  g.add_op(OP_MUL, {selected, linear_scale}, linear);
  const int total = g.add_slot(1, false);
  g.add_op(OP_ADD, {scaled_cat, linear}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  const double point[3] = {0.2, 0.7, 0.1};
  std::copy(std::begin(point), std::end(point), ex.params_data());
  ex.value_ptr(outcome)[0] = 2.0;
  ex.value_ptr(cat_scale)[0] = 0.3;
  ex.value_ptr(linear_scale)[0] = -0.2;
  expect_eq("categorical native value-only propto", ex.forward_value_only(),
            point[1] * -0.2);
  double got_grad[3];
  const double got = ex.gradient(got_grad);

  Eigen::Matrix<stan::math::var, -1, 1> ref_theta(3);
  ref_theta << point[0], point[1], point[2];
  stan::math::var ref = stan::math::categorical_lpmf<true>(2, ref_theta) * 0.3 +
                        ref_theta(1) * -0.2;
  ref.grad();
  expect_eq("categorical native scalar value", got, ref.val());
  for (int i = 0; i < 3; ++i)
    expect_eq("categorical native scalar g" + std::to_string(i), got_grad[i],
              ref_theta(i).adj());
  stan::math::recover_memory();
}

// The scratch that the kernel asks for: one double per row for the logit
// scalar-outcome forms with an active argument, and none otherwise.
static int64_t categorical_scratch_len(uint8_t variant, int32_t rows,
                                       int32_t width) {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_CATEGORICAL);
  const int32_t lens[2] = {rows, rows * width};
  return kernel_call_scratch(kernel->scratch_size, OP_CATEGORICAL, variant, 2,
                             lens, 1, nullptr, 0, nullptr);
}

// Direct kernel edges for the same rule. A scalar categorical log only
// connects the selected probability: an infinite seed must leave an
// unselected signed zero untouched, while a zero seed over a selected zero
// still follows the connected log edge and forms 0 / 0. Reusing the context
// also pins that the probability form carries no hidden scratch state.
static void test_categorical_native_kernel_edges() {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_CATEGORICAL);
  expect_true("categorical native kernel registered", kernel != nullptr);
  // The forward keeps each row's log_sum_exp for the backward. Only the
  // logit form with an active argument computes one.
  expect_true("categorical kernel declares scratch",
              kernel && kernel->scratch_size != nullptr);
  const uint8_t logit_rows =
      kCategoricalRows | kCategoricalScalarOutcome | kCategoricalLogit;
  expect_true(
      "categorical probability scalar has no scratch",
      categorical_scratch_len(
          kCategoricalScalarOutcome | kCategoricalArgAutodiff, 1, 2) == 0);
  expect_true(
      "categorical logit scalar scratch is one row",
      categorical_scratch_len(kCategoricalScalarOutcome | kCategoricalLogit |
                                  kCategoricalArgAutodiff,
                              1, 5) == 1);
  expect_true(
      "categorical logit rows scratch is one per row",
      categorical_scratch_len(logit_rows | kCategoricalArgAutodiff, 7, 5) == 7);
  expect_true("categorical data logit rows have no scratch",
              categorical_scratch_len(logit_rows, 7, 5) == 0);
  expect_true("categorical array logit has no scratch",
              categorical_scratch_len(
                  kCategoricalLogit | kCategoricalArgAutodiff, 7, 5) == 0);
  if (kernel) {
    double outcome = 1.0;
    double theta[2] = {1.0, 0.0};
    double theta_adj[2] = {0.0, -0.0};
    double out = 0.0;
    KernelCtx ctx{};
    ctx.n_in = 2;
    ctx.in[0] = Desc{&outcome, 1};
    ctx.in[1] = Desc{theta, 2};
    ctx.in_adj[0] = Desc{nullptr, 1};
    ctx.in_adj[1] = Desc{theta_adj, 2};
    ctx.out = Desc{&out, 1};
    ctx.variant = kCategoricalScalarOutcome | kCategoricalArgAutodiff | 0x80u;

    kernel->forward(ctx);
    ctx.out_adj = std::numeric_limits<double>::infinity();
    kernel->backward(ctx);
    expect_true("categorical native infinite selected adjoint",
                std::isinf(theta_adj[0]) && theta_adj[0] > 0.0);
    expect_true("categorical native unselected signed zero",
                theta_adj[1] == 0.0 && std::signbit(theta_adj[1]));

    theta[0] = 0.0;
    theta[1] = 1.0;
    theta_adj[0] = 0.0;
    theta_adj[1] = -0.0;
    kernel->forward(ctx);
    expect_true("categorical native selected zero value",
                std::isinf(out) && out < 0.0);
    ctx.out_adj = 0.0;
    kernel->backward(ctx);
    expect_true("categorical native selected zero topology",
                std::isnan(theta_adj[0]));
    expect_true("categorical native repeated unselected zero",
                theta_adj[1] == 0.0 && std::signbit(theta_adj[1]));

    outcome = 2.0;
    theta[0] = 0.2;
    theta[1] = 0.8;
    ctx.in_adj[1] = Desc{nullptr, 2};
    kernel->forward(ctx);
    expect_eq("categorical native null-adjoint value", out, std::log(0.8));
    ctx.out_adj = 1.0;
    kernel->backward(ctx);  // active source type, but no graph adjoint edge
  }
}

// Scalar logits can bypass the var tape, but must retain the subtraction
// and scalar-exp reverse order even for prefilled or signed-zero adjoints.
static void test_categorical_logit_scalar_bits() {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_CATEGORICAL);
  const double inf = std::numeric_limits<double>::infinity();
  const auto same_bits = [](double a, double b) {
    return (std::isnan(a) && std::isnan(b)) ||
           std::memcmp(&a, &b, sizeof(double)) == 0;
  };
  for (int n : {1, 2, 3, 8, 32})
    for (int selected = 1; selected <= n; ++selected)
      for (double scale : {0.0, 1.0, 1000.0, 1e308})
        for (double weight : {0.0, -0.0, 1.0, -1.3, 1e308, 1e-308, inf})
          for (bool propto : {false, true}) {
            stan::math::nested_rev_autodiff nested;
            Eigen::VectorXd beta(n), adj(n);
            Eigen::Matrix<stan::math::var, -1, 1> ref_beta(n);
            for (int i = 0; i < n; ++i) {
              beta(i) = scale * std::sin(i + 1.0);
              ref_beta(i) = beta(i);
              adj(i) = i % 2 ? -0.0 : 0.3125;
              ref_beta(i).adj() = adj(i);
            }
            double outcome = selected, out = 0;
            KernelCtx ctx{};
            ctx.n_in = 2;
            ctx.in[0] = Desc{&outcome, 1};
            ctx.in[1] = Desc{beta.data(), n};
            ctx.in_adj[1] = Desc{adj.data(), n};
            ctx.out = Desc{&out, 1};
            ctx.out_adj = weight;
            ctx.variant = kCategoricalScalarOutcome | kCategoricalLogit |
                          kCategoricalArgAutodiff | (propto ? 0x80u : 0u);
            std::vector<double> scratch(
                (size_t)categorical_scratch_len(ctx.variant, 1, n),
                std::numeric_limits<double>::quiet_NaN());
            ctx.scratch = scratch.data();
            kernel->forward(ctx);
            kernel->backward(ctx);
            stan::math::var lp =
                propto ? stan::math::categorical_logit_lpmf<true>(selected,
                                                                  ref_beta)
                       : stan::math::categorical_logit_lpmf<false>(selected,
                                                                   ref_beta);
            stan::math::var ref = lp * weight;
            stan::math::grad(ref.vi_);
            expect_true("native scalar categorical logit value bits",
                        same_bits(out * weight, ref.val()));
            for (int i = 0; i < n; ++i)
              expect_true("native scalar categorical logit gradient bits",
                          same_bits(adj(i), ref_beta(i).adj()));
          }
}

// One op over several rows does each row's scalar-outcome arithmetic: the
// value is the rows' lps added left to right from zero, as the chained
// ADD_N of per-row terms adds them, and every gradient is that row's own.
// Outcome and logit checks still reject per row.
static void test_categorical_rows() {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_CATEGORICAL);
  const int rows = 3, k = 4;
  const double outcomes[rows] = {2.0, 4.0, 1.0};
  const auto run = [&](uint8_t form, const double* arg, const double* seed_adj,
                       double weight, double* adj) {
    double out = 0.0;
    KernelCtx ctx{};
    ctx.n_in = 2;
    ctx.in[0] = Desc{const_cast<double*>(outcomes), rows};
    ctx.in[1] = Desc{const_cast<double*>(arg), rows * k};
    ctx.in_adj[1] = Desc{adj, rows * k};
    ctx.out = Desc{&out, 1};
    ctx.out_adj = weight;
    ctx.variant = kCategoricalRows | kCategoricalScalarOutcome |
                  kCategoricalArgAutodiff | form;
    std::vector<double> scratch(
        (size_t)categorical_scratch_len(ctx.variant, rows, k),
        std::numeric_limits<double>::quiet_NaN());
    ctx.scratch = scratch.data();
    std::copy(seed_adj, seed_adj + rows * k, adj);
    kernel->forward(ctx);
    kernel->backward(ctx);
    return out;
  };
  const double logits[rows * k] = {0.3,  -1.2, 2.5, 0.0, 1e-3, 4.0,
                                   -0.5, 7.25, 0.1, 0.2, 0.3,  -9.0};
  const double probs[rows * k] = {0.125, 0.25,  0.5,  0.125, 0.5,   0.25,
                                  0.125, 0.125, 0.25, 0.25,  0.375, 0.125};
  double seed[rows * k];
  for (int i = 0; i < rows * k; ++i) seed[i] = i % 2 ? -0.0 : 0.3125;
  for (uint8_t form : {kCategoricalLogit, (uint8_t)0}) {
    const double* arg = form ? logits : probs;
    const std::string what =
        std::string("categorical rows ") + (form ? "logit" : "probability");
    double adj[rows * k];
    double got = 0.0;
    try {
      got = run(form, arg, seed, -1.3, adj);
    } catch (const std::exception& e) {
      expect_true(what + " evaluates (" + e.what() + ")", false);
      continue;
    }
    stan::math::nested_rev_autodiff nested;
    double want = 0.0;
    stan::math::var total = 0.0;
    Eigen::Matrix<stan::math::var, -1, 1> ref(rows * k);
    for (int i = 0; i < rows * k; ++i) ref(i) = arg[i];
    for (int i = 0; i < rows * k; ++i) ref(i).adj() = seed[i];
    for (int r = 0; r < rows; ++r) {
      const Eigen::Matrix<stan::math::var, -1, 1> row = ref.segment(r * k, k);
      const int y = (int)outcomes[r];
      stan::math::var lp =
          form ? stan::math::categorical_logit_lpmf<false>(y, row)
               : stan::math::categorical_lpmf<false>(y, row);
      want += lp.val();
      total += lp * -1.3;
    }
    stan::math::grad(total.vi_);
    expect_eq(what + " value", got, want);
    for (int i = 0; i < rows * k; ++i)
      expect_eq(what + " g" + std::to_string(i), adj[i], ref(i).adj());
  }

  const auto throws = [&](const double* arg, uint8_t form, double bad_y) {
    double y[rows] = {outcomes[0], bad_y, outcomes[2]};
    double out = 0.0, adj[rows * k] = {};
    KernelCtx ctx{};
    ctx.n_in = 2;
    ctx.in[0] = Desc{y, rows};
    ctx.in[1] = Desc{const_cast<double*>(arg), rows * k};
    ctx.in_adj[1] = Desc{adj, rows * k};
    ctx.out = Desc{&out, 1};
    ctx.variant = kCategoricalRows | kCategoricalScalarOutcome |
                  kCategoricalArgAutodiff | form;
    std::vector<double> scratch(
        (size_t)categorical_scratch_len(ctx.variant, rows, k));
    ctx.scratch = scratch.data();
    try {
      kernel->forward(ctx);
    } catch (const std::domain_error&) {
      return true;
    } catch (const std::exception&) {
      return false;
    }
    return false;
  };
  expect_true("categorical rows outcome above support",
              throws(logits, kCategoricalLogit, k + 1.0));
  expect_true("categorical rows outcome below support",
              throws(logits, kCategoricalLogit, 0.0));
  expect_true("categorical rows probability outcome support",
              throws(probs, 0, k + 1.0));
  double bad[rows * k];
  std::copy(std::begin(logits), std::end(logits), bad);
  bad[k + 2] = std::numeric_limits<double>::infinity();
  expect_true("categorical rows non-finite logit",
              throws(bad, kCategoricalLogit, outcomes[1]));
}

// The batched logit form must give each row the bits that a separate
// Stan Math call would give it, over widths, extreme scales, signed-zero and
// infinite seeds, prefilled adjoints and both propto settings.
static void test_categorical_logit_rows_bits() {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_CATEGORICAL);
  const double inf = std::numeric_limits<double>::infinity();
  const auto same_bits = [](double a, double b) {
    return (std::isnan(a) && std::isnan(b)) ||
           std::memcmp(&a, &b, sizeof(double)) == 0;
  };
  const int rows = 4;
  int failures = 0;
  for (int k : {1, 2, 3, 8, 32})
    for (double scale : {0.0, 1.0, 1000.0, 1e308})
      for (double weight : {0.0, -0.0, 1.0, -1.3, 1e308, 1e-308, inf})
        for (bool propto : {false, true}) {
          stan::math::nested_rev_autodiff nested;
          const int n = rows * k;
          std::vector<double> outcomes(rows), beta(n), adj(n);
          Eigen::Matrix<stan::math::var, -1, 1> ref_beta(n);
          for (int r = 0; r < rows; ++r) outcomes[r] = 1 + (3 * r + 1) % k;
          for (int i = 0; i < n; ++i) {
            beta[i] = scale * std::sin(1.7 * i + 1.0);
            ref_beta(i) = beta[i];
            adj[i] = i % 3 ? -0.0 : 0.3125;
            ref_beta(i).adj() = adj[i];
          }
          double out = 0.0;
          KernelCtx ctx{};
          ctx.n_in = 2;
          ctx.in[0] = Desc{outcomes.data(), rows};
          ctx.in[1] = Desc{beta.data(), n};
          ctx.in_adj[1] = Desc{adj.data(), n};
          ctx.out = Desc{&out, 1};
          ctx.out_adj = weight;
          ctx.variant = kCategoricalRows | kCategoricalScalarOutcome |
                        kCategoricalLogit | kCategoricalArgAutodiff |
                        (propto ? 0x80u : 0u);
          std::vector<double> scratch(
              (size_t)categorical_scratch_len(ctx.variant, rows, k),
              std::numeric_limits<double>::quiet_NaN());
          ctx.scratch = scratch.data();
          kernel->forward(ctx);
          kernel->backward(ctx);
          double want = 0.0;
          stan::math::var total = 0.0;
          for (int r = 0; r < rows; ++r) {
            const Eigen::Matrix<stan::math::var, -1, 1> row =
                ref_beta.segment(r * k, k);
            const int y = (int)outcomes[r];
            stan::math::var lp =
                propto ? stan::math::categorical_logit_lpmf<true>(y, row)
                       : stan::math::categorical_logit_lpmf<false>(y, row);
            want = r == 0 ? lp.val() : want + lp.val();
            total += lp * weight;
          }
          stan::math::grad(total.vi_);
          if (!same_bits(out, want)) ++failures;
          for (int i = 0; i < n; ++i)
            if (!same_bits(adj[i], ref_beta(i).adj())) ++failures;
        }
  expect_true("categorical logit rows value and gradient bits (" +
                  std::to_string(failures) + " mismatches)",
              failures == 0);
}

// Array outcomes add one callback contribution per observation. Preserve
// that exact accumulation order under a non-unit upstream adjoint; replacing
// it with count * adjoint changes the selected gradient's low bit. The zero
// unselected probability also pins Stan Math's disconnected-log topology.
static void test_categorical_array_scaled() {
  using namespace stanli;
  Graph g;
  const int theta = g.add_slot(2, true);
  const int outcomes = g.add_slot(6, false);
  const int cat = g.add_slot(1, false);
  const int cat_op = g.add_op(OP_CATEGORICAL, {outcomes, theta}, cat);
  g.ops[(size_t)cat_op].variant = kCategoricalArgAutodiff;
  const int scale = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  g.add_op(OP_MUL, {cat, scale}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  ex.params_data()[0] = 1.0;
  ex.params_data()[1] = 0.0;
  std::fill(ex.value_ptr(outcomes), ex.value_ptr(outcomes) + 6, 1.0);
  ex.value_ptr(scale)[0] = 0.1;
  double got_grad[2];
  const double got = ex.gradient(got_grad);

  Eigen::Matrix<stan::math::var, -1, 1> ref_theta(2);
  ref_theta << 1.0, 0.0;
  const std::vector<int> ref_outcomes(6, 1);
  stan::math::var ref =
      stan::math::categorical_lpmf<false>(ref_outcomes, ref_theta) * 0.1;
  ref.grad();
  expect_eq("categorical array scaled value", got, ref.val());
  expect_eq("categorical array scaled selected", got_grad[0],
            ref_theta(0).adj());
  expect_true("categorical array scaled unselected topology",
              std::isnan(got_grad[1]) && std::isnan(ref_theta(1).adj()));
  stan::math::recover_memory();
}

// Stan Math adds the rows' log normalizers, maxima and selected logits as
// three separate sums. With logits near 100 the second and third sums cancel
// to the result's scale, and their rounding errors survive (7 ULP here, 25
// ULP in grsm_latent_reg_irt). Every input and logit is exact in double, so
// the 60-digit value below is the exact log density at these inputs.
static void test_categorical_glm_long_sum() {
  using namespace stanli;
  using stan::math::var;
  const int rows = 400, cats = 4;
  std::vector<double> X(rows);
  std::vector<int> idata;
  for (int n = 0; n < rows; ++n) {
    X[(size_t)n] = ((n * 37) % 101 - 50) / 64.0;
    idata.push_back(1 + (n * 7) % 4);
  }
  append_glm_layout(idata, rows, 1, 0);
  const double alpha[cats] = {100, 99.75, 99.5, 99.125};
  const double beta[cats] = {0, 0.3125, 0.625, 0.9375};

  Graph g;
  const int Xs = g.add_slot(rows, true);
  const int as = g.add_slot(cats, true);
  const int bs = g.add_slot(cats, true);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_CATEGORICAL_LOGIT_GLM_LPMF, {Xs, as, bs}, lp, idata);
  g.result_slot = lp;
  Executor ex(std::move(g));
  std::copy(X.begin(), X.end(), ex.param_ptr(Xs));
  std::copy(alpha, alpha + cats, ex.param_ptr(as));
  std::copy(beta, beta + cats, ex.param_ptr(bs));
  std::vector<double> grad(rows + 2 * cats);
  const double got = ex.gradient(grad.data());
  // mpmath, 60 digits: -579.532230417468898853401775812
  expect_ulp("categorical glm long sum value", got, -579.5322304174689, 1);
  expect_eq("categorical glm long sum value-only", ex.forward_value_only(),
            got);

  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<var, -1, -1> Xv(rows, 1);
  for (int n = 0; n < rows; ++n) Xv(n, 0) = X[(size_t)n];
  Eigen::Matrix<var, -1, 1> av(cats);
  Eigen::Matrix<var, -1, -1> bm(1, cats);
  for (int j = 0; j < cats; ++j) {
    av(j) = alpha[j];
    bm(0, j) = beta[j];
  }
  std::vector<int> y(idata.begin(), idata.begin() + rows);
  var ref = stan::math::categorical_logit_glm_lpmf<false>(y, Xv, av, bm);
  stan::math::grad(ref.vi_);
  for (int n = 0; n < rows; ++n)
    expect_eq("categorical glm long sum dX", grad[(size_t)n], Xv(n, 0).adj());
  for (int j = 0; j < cats; ++j) {
    expect_eq("categorical glm long sum da", grad[(size_t)(rows + j)],
              av(j).adj());
    expect_eq("categorical glm long sum db", grad[(size_t)(rows + cats + j)],
              bm(0, j).adj());
  }
}

static void reference(const double* q, double* lp_out, double* grad_out) {
  using stan::math::var;
  using stanli::testmodels::LogisticGlm;
  const int N = LogisticGlm::N, K = LogisticGlm::K;

  var alpha = q[0];
  Eigen::Matrix<var, -1, 1> beta(K);
  for (int i = 0; i < K; ++i) beta(i) = q[1 + i];
  var zero = 0.0, p25 = 2.5, five = 5.0;

  // eta in the same order as OP_MATVEC then OP_ADD.
  // kX is column-major (Stan/Eigen convention).
  Eigen::Matrix<var, -1, 1> eta(N);
  for (int r = 0; r < N; ++r) {
    var acc = 0.0;
    for (int c = 0; c < K; ++c) acc += LogisticGlm::kX[c * N + r] * beta(c);
    eta(r) = acc + alpha;
  }
  std::vector<int> y(LogisticGlm::kYint, LogisticGlm::kYint + N);
  var lp1 = stan::math::bernoulli_logit_lpmf<false>(y, eta);
  var lp2 = stan::math::normal_lpdf<false>(beta, zero, p25);
  var lp3 = stan::math::normal_lpdf<false>(alpha, zero, five);
  var lp = lp1 + lp2 + lp3;
  lp.grad();

  *lp_out = lp.val();
  grad_out[0] = alpha.adj();
  for (int i = 0; i < K; ++i) grad_out[1 + i] = beta(i).adj();
  stan::math::recover_memory();
}

int main() {
  using namespace stanli;
  auto m = testmodels::logistic_glm();
  Executor ex(std::move(m.graph));
  testmodels::fill_logistic_glm_data(m, ex);
  const int NP = 4;

  const double qs[3][NP] = {
      {0.2, 0.5, -0.8, 1.1}, {-1.0, 0.0, 0.3, -0.2}, {2.2, -1.5, 0.9, 0.4}};

  for (int c = 0; c < 3; ++c) {
    ex.param_ptr(m.alpha)[0] = qs[c][0];
    for (int i = 0; i < 3; ++i) ex.param_ptr(m.beta)[i] = qs[c][1 + i];

    double grad[NP], lp_ref, grad_ref[NP];
    const double lp = ex.gradient(grad);
    reference(qs[c], &lp_ref, grad_ref);

    const std::string tag = "case" + std::to_string(c);
    expect_ulp(tag + " lp", lp, lp_ref, 10);
    for (int i = 0; i < NP; ++i)
      expect_ulp(tag + " g" + std::to_string(i), grad[i], grad_ref[i], 10);
  }

  check_tail_glms();
  check_glms();

  check_empty_glm(OP_BERNOULLI_LOGIT_GLM_LPMF, "bernoulli_logit_glm");
  check_empty_glm(OP_POISSON_LOG_GLM_LPMF, "poisson_log_glm");
  check_empty_glm(OP_NEG_BINOMIAL_2_LOG_GLM_LPMF, "neg_binomial_2_log_glm");

  test_categorical_existing_adjoint();
  test_categorical_native_scalar();
  test_categorical_native_kernel_edges();
  test_categorical_logit_scalar_bits();
  test_categorical_rows();
  test_categorical_logit_rows_bits();
  test_categorical_array_scaled();
  test_categorical_glm_long_sum();

  return finish("test_glm");
}
