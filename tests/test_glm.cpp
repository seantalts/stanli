// Logistic GLM: graph log_prob + gradient vs an all-var reference in the
// same op order, at three fixed parameter vectors. case0-2 (OP_MATVEC): 10
// ULP. The rest: bitwise.
#include "models.hpp"
#include <stanli/density_registry.hpp>

#include <stan/math.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want && !(std::isnan(got) && std::isnan(want))) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

static int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? (-(int64_t(1) << 63)) - i : i;
}
static void expect_ulp(const std::string& what, double got, double want,
                       int64_t budget) {
  const int64_t d = std::abs(ulp_key(got) - ulp_key(want));
  if (d > budget) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
  }
}

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
             {0, K});
  } else {
    g.add_op(opcode, {X_slot, alpha_slot, beta_slot}, lp_slot, {0, K});
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
  expect_eq(name + " empty value", value, value_ref.val());
  expect_eq(name + " empty alpha", grad[0], alpha.adj());
  for (int k = 0; k < K; ++k)
    expect_eq(name + " empty beta" + std::to_string(k), grad[1 + k],
              beta(k).adj());
  if (has_phi) expect_eq(name + " empty phi", grad[3], phi.adj());
  stan::math::recover_memory();
}

// A per-row intercept: brms writes one whenever the model has a group-level
// term, and stan-math takes alpha as a vector there.
static void check_vector_alpha(const std::string& tag, uint16_t opcode,
                               bool propto) {
  using namespace stanli;
  using stan::math::var;
  const int rows = 6, cols = 3;
  const bool has_phi = opcode == OP_NEG_BINOMIAL_2_LOG_GLM_LPMF;
  const bool bern = opcode == OP_BERNOULLI_LOGIT_GLM_LPMF;

  std::vector<double> X((size_t)rows * cols), a((size_t)rows), b((size_t)cols);
  for (int j = 0; j < cols; ++j)
    for (int i = 0; i < rows; ++i)
      X[(size_t)j * rows + i] = std::sin(0.31 * i + 0.7 * j);
  for (int i = 0; i < rows; ++i) a[(size_t)i] = 0.15 - 0.08 * i;
  for (int i = 0; i < cols; ++i) b[(size_t)i] = 0.2 + 0.11 * i;
  std::vector<int> idata;
  for (int i = 0; i < rows; ++i) idata.push_back(bern ? i % 2 : 1 + (i % 4));
  idata.push_back(rows);
  idata.push_back(cols);

  const double seed = -0.73;
  Graph g;
  const int Xs = g.add_slot(rows * cols, false);
  const int as = g.add_slot(rows, true);
  const int bs = g.add_slot(cols, true);
  const int ps = has_phi ? g.add_slot(1, true) : -1;
  const int lp = g.add_slot(1, false);
  const int ss = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  const int op = has_phi ? g.add_op(opcode, {Xs, as, bs, ps}, lp, idata)
                         : g.add_op(opcode, {Xs, as, bs}, lp, idata);
  g.ops[(size_t)op].variant =
      (uint8_t)((propto ? 0x80u : 0u) | (has_phi ? 0x0eu : 0x06u));
  g.add_op(OP_MUL, {lp, ss}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  std::copy(X.begin(), X.end(), ex.value_ptr(Xs));
  std::copy(a.begin(), a.end(), ex.param_ptr(as));
  std::copy(b.begin(), b.end(), ex.param_ptr(bs));
  if (has_phi) ex.param_ptr(ps)[0] = 1.7;
  ex.value_ptr(ss)[0] = seed;
  std::vector<double> grad((size_t)(rows + cols + (has_phi ? 1 : 0)), 0.0);
  const double got = ex.gradient(grad.data());

  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<double, -1, -1> Xd(rows, cols);
  for (size_t i = 0; i < X.size(); ++i) Xd.data()[i] = X[i];
  Eigen::Matrix<var, -1, 1> av(rows), bv(cols);
  for (int i = 0; i < rows; ++i) av(i) = a[(size_t)i];
  for (int i = 0; i < cols; ++i) bv(i) = b[(size_t)i];
  var phi = 1.7;
  std::vector<int> y(idata.begin(), idata.begin() + rows);
  var ref;
  if (bern) {
    ref = propto ? stan::math::bernoulli_logit_glm_lpmf<true>(y, Xd, av, bv)
                 : stan::math::bernoulli_logit_glm_lpmf<false>(y, Xd, av, bv);
  } else if (opcode == OP_POISSON_LOG_GLM_LPMF) {
    ref = propto ? stan::math::poisson_log_glm_lpmf<true>(y, Xd, av, bv)
                 : stan::math::poisson_log_glm_lpmf<false>(y, Xd, av, bv);
  } else {
    ref = propto ? stan::math::neg_binomial_2_log_glm_lpmf<true>(y, Xd, av, bv,
                                                                 phi)
                 : stan::math::neg_binomial_2_log_glm_lpmf<false>(y, Xd, av, bv,
                                                                  phi);
  }
  var scaled = ref * seed;
  stan::math::grad(scaled.vi_);
  expect_eq(tag + " total", got, scaled.val());
  size_t at = 0;
  for (int i = 0; i < rows; ++i)
    expect_eq(tag + " da" + std::to_string(i), grad[at++], av(i).adj());
  for (int i = 0; i < cols; ++i)
    expect_eq(tag + " db" + std::to_string(i), grad[at++], bv(i).adj());
  if (has_phi) expect_eq(tag + " dphi", grad[at], phi.adj());
}

static void check_vector_alphas() {
  using namespace stanli;
  for (bool propto : {false, true}) {
    const std::string s = propto ? " propto" : "";
    check_vector_alpha("bern glm valpha" + s, OP_BERNOULLI_LOGIT_GLM_LPMF,
                       propto);
    check_vector_alpha("pois glm valpha" + s, OP_POISSON_LOG_GLM_LPMF, propto);
    check_vector_alpha("nb2 glm valpha" + s, OP_NEG_BINOMIAL_2_LOG_GLM_LPMF,
                       propto);
  }
}

// The design matrix is an ordinary differentiable argument, and a model may
// build it from parameters. Dropping its pullback surfaced as a 2-3.5%
// CmdStan gradient divergence in the signature-reference gate: the kernels
// bound X as a plain double map, so d(lp)/dX was silently zero while alpha
// and beta stayed exact.
static void check_active_design(const std::string& tag, uint16_t opcode,
                                bool propto, int alpha_len) {
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
  idata.push_back(rows);
  idata.push_back(cols);

  const double seed = -0.73;
  Graph g;
  const int Xs = g.add_slot(rows * cols, true);
  const int as = g.add_slot(alpha_len, true);
  const int bs = g.add_slot(cols, true);
  const int ps = has_phi ? g.add_slot(1, true) : -1;
  const int lp = g.add_slot(1, false);
  const int ss = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  const int op = has_phi ? g.add_op(opcode, {Xs, as, bs, ps}, lp, idata)
                         : g.add_op(opcode, {Xs, as, bs}, lp, idata);
  g.ops[(size_t)op].variant =
      (uint8_t)((propto ? 0x80u : 0u) | (has_phi ? 0x0fu : 0x07u));
  g.add_op(OP_MUL, {lp, ss}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  std::copy(X.begin(), X.end(), ex.param_ptr(Xs));
  std::copy(a.begin(), a.end(), ex.param_ptr(as));
  std::copy(b.begin(), b.end(), ex.param_ptr(bs));
  if (has_phi) ex.param_ptr(ps)[0] = 1.7;
  ex.value_ptr(ss)[0] = seed;
  std::vector<double> grad(X.size() + a.size() + b.size() + (has_phi ? 1 : 0),
                           0.0);
  const double got = ex.gradient(grad.data());

  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<var, -1, -1> Xv(rows, cols);
  for (size_t i = 0; i < X.size(); ++i) Xv.data()[i] = X[i];
  Eigen::Matrix<var, -1, 1> av(alpha_len), bv(cols);
  for (int i = 0; i < alpha_len; ++i) av(i) = a[(size_t)i];
  for (int i = 0; i < cols; ++i) bv(i) = b[(size_t)i];
  var phi = 1.7;
  std::vector<int> y(idata.begin(), idata.begin() + rows);
  auto with_alpha = [&](const auto& alpha) {
    if (bern) {
      return propto
                 ? stan::math::bernoulli_logit_glm_lpmf<true>(y, Xv, alpha, bv)
                 : stan::math::bernoulli_logit_glm_lpmf<false>(y, Xv, alpha,
                                                               bv);
    } else if (opcode == OP_POISSON_LOG_GLM_LPMF) {
      return propto ? stan::math::poisson_log_glm_lpmf<true>(y, Xv, alpha, bv)
                    : stan::math::poisson_log_glm_lpmf<false>(y, Xv, alpha, bv);
    }
    return propto ? stan::math::neg_binomial_2_log_glm_lpmf<true>(y, Xv, alpha,
                                                                  bv, phi)
                  : stan::math::neg_binomial_2_log_glm_lpmf<false>(y, Xv, alpha,
                                                                   bv, phi);
  };
  var ref = alpha_len == 1 ? with_alpha(av(0)) : with_alpha(av);
  var scaled = ref * seed;
  stan::math::grad(scaled.vi_);
  expect_eq(tag + " total", got, scaled.val());
  size_t at = 0;
  for (size_t i = 0; i < X.size(); ++i)
    expect_eq(tag + " dX" + std::to_string(i), grad[at++], Xv.data()[i].adj());
  for (int i = 0; i < alpha_len; ++i)
    expect_eq(tag + " da" + std::to_string(i), grad[at++], av(i).adj());
  for (int i = 0; i < cols; ++i)
    expect_eq(tag + " db" + std::to_string(i), grad[at++], bv(i).adj());
  if (has_phi) expect_eq(tag + " dphi", grad[at], phi.adj());
}

static void check_active_designs() {
  using namespace stanli;
  for (bool propto : {false, true}) {
    for (int alpha_len : {1, 6}) {
      const std::string s = (propto ? std::string(" propto") : std::string()) +
                            (alpha_len == 1 ? "" : " valpha");
      check_active_design("bern glm dX" + s, OP_BERNOULLI_LOGIT_GLM_LPMF,
                          propto, alpha_len);
      check_active_design("pois glm dX" + s, OP_POISSON_LOG_GLM_LPMF, propto,
                          alpha_len);
      check_active_design("nb2 glm dX" + s, OP_NEG_BINOMIAL_2_LOG_GLM_LPMF,
                          propto, alpha_len);
    }
  }
}

// Compare the compact GLM kernels with a weighted nested-tape reference.
static void check_tail_glm(const std::string& tag, uint16_t opcode, bool propto,
                           const std::vector<int>& idata, int rows, int cols,
                           int alpha_len, int beta_len, double seed = -0.73) {
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
  g.ops[(size_t)op].variant = propto ? 0x80u : 0x00u;
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
    ref = propto
              ? stan::math::categorical_logit_glm_lpmf<true>(yy, Xv, av, bm)
              : stan::math::categorical_logit_glm_lpmf<false>(yy, Xv, av, bm);
    var scaled_cat = ref * seed;
    stan::math::grad(scaled_cat.vi_);
    expect_eq(tag + " total", got, scaled_cat.val());
    size_t at = 0;
    for (size_t i = 0; i < X.size(); ++i)
      expect_eq(tag + " dX" + std::to_string(i), grad[at++],
                Xv.data()[i].adj());
    for (int i = 0; i < alpha_len; ++i)
      expect_eq(tag + " da" + std::to_string(i), grad[at++], av(i).adj());
    for (int i = 0; i < beta_len; ++i)
      expect_eq(tag + " db" + std::to_string(i), grad[at++],
                bm.data()[i].adj());
    return;
  } else {
    std::vector<int> yy(idata.begin(), idata.begin() + rows);
    ref = propto ? stan::math::ordered_logistic_glm_lpmf<true>(yy, Xv, av, bv)
                 : stan::math::ordered_logistic_glm_lpmf<false>(yy, Xv, av, bv);
  }
  var scaled = ref * seed;
  stan::math::grad(scaled.vi_);
  expect_eq(tag + " total", got, scaled.val());
  size_t at = 0;
  for (size_t i = 0; i < X.size(); ++i)
    expect_eq(tag + " dX" + std::to_string(i), grad[at++], Xv.data()[i].adj());
  for (int i = 0; i < alpha_len; ++i)
    expect_eq(tag + " da" + std::to_string(i), grad[at++], av(i).adj());
  for (int i = 0; i < beta_len; ++i)
    expect_eq(tag + " db" + std::to_string(i), grad[at++], bv(i).adj());
}

static void check_tail_glms() {
  using namespace stanli;
  const int rows = 5, cols = 3;
  std::vector<int> bin;
  for (int i = 0; i < rows; ++i) bin.push_back(1 + (i % 4));
  for (int i = 0; i < rows; ++i) bin.push_back(5 + i);
  bin.push_back(rows);
  bin.push_back(cols);
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
  cat.push_back(rows);
  cat.push_back(cols);
  check_tail_glm("cat glm", OP_CATEGORICAL_LOGIT_GLM_LPMF, false, cat, rows,
                 cols, cats, cols * cats);
  check_tail_glm("cat glm propto", OP_CATEGORICAL_LOGIT_GLM_LPMF, true, cat,
                 rows, cols, cats, cols * cats);

  // in = {X, beta, cutpoints}: the cutpoints ride the `alpha` slot here and
  // must stay ordered.
  check_tail_glm("ord glm", OP_ORDERED_LOGISTIC_GLM_LPMF, false, cat, rows,
                 cols, cols, cats - 1);
  check_tail_glm("ord glm propto", OP_ORDERED_LOGISTIC_GLM_LPMF, true, cat,
                 rows, cols, cols, cats - 1);
  check_tail_glm("ord glm empty", OP_ORDERED_LOGISTIC_GLM_LPMF, false,
                 {0, cols}, 0, cols, cols, cats - 1,
                 std::numeric_limits<double>::infinity());
}

// Shared-cutpoint recorder versus the established nested Stan Math tape.
// Per-observation cutpoints exercise the deliberately retained fallback.
static void check_ordered_density(bool array_cuts, bool scalar_location,
                                  bool propto, double weight, int width,
                                  bool probit = false, double shift = 0) {
  using namespace stanli;
  using stan::math::var;
  const int n = scalar_location ? 1 : 4;
  const int nc = array_cuts ? n : 1;
  std::vector<int> y;
  for (int i = 0; i < n; ++i) y.push_back(1 + i % (width + 1));
  auto layout = y;
  layout.insert(layout.end(),
                {kVectorizedDensityLayoutMarker, width, array_cuts ? nc : -1});
  Graph g;
  const int ls = g.add_slot(n, true), cs = g.add_slot(width * nc, true);
  const int ws = g.add_slot(1, false), out = g.add_slot(1, false);
  const int scaled = g.add_slot(1, false);
  g.add_op(probit ? OP_ORDERED_PROBIT_LPMF : OP_ORDERED_LOGISTIC_LPMF, {ls, cs},
           out, layout);
  g.ops.back().variant = propto ? 0x83u : 0;
  g.add_op(OP_MUL, {out, ws}, scaled);
  g.result_slot = scaled;
  Executor ex(std::move(g));
  ex.value_ptr(ws)[0] = weight;
  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<var, -1, 1> location(n);
  std::vector<Eigen::Matrix<var, -1, 1>> cuts(nc);
  for (int i = 0; i < n; ++i)
    location(i) = ex.param_ptr(ls)[i] = shift - 0.6 + 0.7 * i;
  for (int j = 0; j < nc; ++j) {
    cuts[j].resize(width);
    for (int k = 0; k < width; ++k)
      cuts[j](k) = ex.param_ptr(cs)[j * width + k] = -1.2 + k + 0.1 * j;
  }
  std::vector<double> grad(n + width * nc);
  const double got = ex.gradient(grad.data());
  const auto call = [&](const auto& l, const auto& c) {
    if (probit)
      return propto ? stan::math::ordered_probit_lpmf<true>(y, l, c)
                    : stan::math::ordered_probit_lpmf<false>(y, l, c);
    return propto ? stan::math::ordered_logistic_lpmf<true>(y, l, c)
                  : stan::math::ordered_logistic_lpmf<false>(y, l, c);
  };
  var density;
  if (array_cuts)
    density = scalar_location ? call(location(0), cuts) : call(location, cuts);
  else
    density =
        scalar_location ? call(location(0), cuts[0]) : call(location, cuts[0]);
  var ref = density * weight;
  stan::math::grad(ref.vi_);
  expect_eq("ordered value", got, ref.val());
  const auto compare_gradient = [&](const char* name, double actual,
                                    double expected) {
    // The recorder and var CDF instantiations can differ in the far-tail
    // approximation's rounding; at +/-30 the observed relative error is
    // 6e-14. Keep a stricter gate than the 1e-9 external corpus oracle.
    if (probit && std::isfinite(expected))
      expect_eq(name,
                std::abs(actual - expected) <= 1e-12 * (1 + std::abs(expected)),
                true);
    else
      expect_eq(name, actual, expected);
  };
  for (int i = 0; i < n; ++i)
    compare_gradient("ordered location", grad[i], location(i).adj());
  for (int j = 0; j < nc; ++j)
    for (int k = 0; k < width; ++k)
      compare_gradient("ordered cutpoint", grad[n + j * width + k],
                       cuts[j](k).adj());

  // Validation must still run, including after a preceding successful call.
  if (width > 1) {
    ex.param_ptr(cs)[1] = ex.param_ptr(cs)[0];
    bool threw = false;
    try {
      ex.gradient(grad.data());
    } catch (const std::domain_error&) {
      threw = true;
    }
    expect_eq("ordered invalid cutpoints", threw, true);
    ex.param_ptr(cs)[1] += 1.0;
    expect_eq("ordered recovery after error", ex.gradient(grad.data()), got);
  }
}

// Compare full/proportional values and weighted gradients with the all-var
// oracle, including simplex boundaries, inactive edges and invalid inputs.
static void check_multinomial_density() {
  using namespace stanli;
  using stan::math::var;
  for (bool propto : {false, true})
    for (bool active : {false, true})
      for (double weight : {1.0, -2.75, 0.0, 1e308, 1e-308,
                            std::numeric_limits<double>::infinity()})
        for (const auto& values : std::vector<std::vector<double>>{
                 {0.2, 0.3, 0.5}, {0.0, 0.5, 0.5}, {1.0}}) {
          const int n = values.size();
          std::vector<int> counts(n);
          for (int i = 0; i < n; ++i) counts[i] = i == 2 ? 4 : i;
          Graph g;
          const int theta = g.add_slot(n, active), w = g.add_slot(1, false);
          const int out = g.add_slot(1, false), scaled = g.add_slot(1, false);
          g.add_op(OP_MULTINOMIAL_LPMF, {theta}, out, counts);
          g.ops.back().variant = propto ? 0x80u : 0u;
          g.add_op(OP_MUL, {out, w}, scaled);
          g.result_slot = scaled;
          Executor ex(std::move(g));
          std::copy(values.begin(), values.end(), ex.value_ptr(theta));
          ex.value_ptr(w)[0] = weight;
          std::vector<double> grad(active ? n : 1);
          const double got = ex.gradient(grad.data());
          stan::math::nested_rev_autodiff nested;
          Eigen::Matrix<var, -1, 1> probabilities(n);
          for (int i = 0; i < n; ++i) probabilities(i) = values[i];
          var density =
              propto
                  ? stan::math::multinomial_lpmf<true>(counts, probabilities)
                  : stan::math::multinomial_lpmf<false>(counts, probabilities);
          var ref = density * weight;
          stan::math::grad(ref.vi_);
          expect_eq("multinomial value", got, ref.val());
          if (active)
            for (int i = 0; i < n; ++i)
              expect_eq("multinomial weighted gradient", grad[i],
                        probabilities(i).adj());
          ex.value_ptr(theta)[0] = -0.1;
          bool threw = false;
          try {
            ex.gradient(grad.data());
          } catch (const std::domain_error&) {
            threw = true;
          }
          expect_eq("multinomial invalid simplex", threw, true);
        }
}

static void check_dirichlet_recorder() {
  using namespace stanli;
  using stan::math::var;
  for (unsigned mask : {0u, 1u, 2u, 3u})
    for (bool propto : {false, true})
      for (bool arrays : {false, true})
        for (double weight : {1.0, -2.75, 0.0}) {
          constexpr int width = 3;
          const int n = arrays ? 2 : 1;
          Graph g;
          const int t = g.add_slot(width * n, true),
                    a = g.add_slot(width, true);
          const int w = g.add_slot(1, false), lp = g.add_slot(1, false);
          const int scaled = g.add_slot(1, false);
          g.add_op(OP_DIRICHLET_LPDF, {t, a}, lp, {width, arrays ? n : -1, -1});
          g.ops.back().variant = 0x40u | mask | (propto ? 0x80u : 0u);
          g.add_op(OP_MUL, {lp, w}, scaled);
          g.result_slot = scaled;
          Executor ex(std::move(g));
          ex.value_ptr(w)[0] = weight;
          stan::math::nested_rev_autodiff nested;
          std::vector<Eigen::Matrix<var, -1, 1>> tv(n);
          std::vector<Eigen::VectorXd> td(n);
          Eigen::Matrix<var, -1, 1> av(width);
          Eigen::VectorXd ad(width);
          for (int j = 0; j < n; ++j) {
            tv[j].resize(width);
            td[j].resize(width);
            for (int k = 0; k < width; ++k)
              tv[j](k) = td[j](k) = ex.param_ptr(t)[j * width + k] =
                  (k + 1) / 6.0;
          }
          for (int k = 0; k < width; ++k)
            av(k) = ad(k) = ex.param_ptr(a)[k] = 0.5 + k;
          var density = 0.0;
          const auto call = [&](const auto& theta, const auto& alpha) {
            density = propto ? stan::math::dirichlet_lpdf<true>(theta, alpha)
                             : stan::math::dirichlet_lpdf<false>(theta, alpha);
          };
          const auto alpha = [&](const auto& theta) {
            if (mask & 2u)
              call(theta, av);
            else
              call(theta, ad);
          };
          if (!(propto && mask == 0)) {
            if (mask & 1u) {
              if (arrays)
                alpha(tv);
              else
                alpha(tv[0]);
            } else {
              if (arrays)
                alpha(td);
              else
                alpha(td[0]);
            }
          }
          var ref = density * weight;
          stan::math::grad(ref.vi_);
          std::vector<double> grad(width * (n + 1));
          expect_eq("dirichlet recorder value", ex.gradient(grad.data()),
                    ref.val());
          for (int j = 0; j < n; ++j)
            for (int k = 0; k < width; ++k)
              expect_eq("dirichlet recorder theta", grad[j * width + k],
                        tv[j](k).adj());
          for (int k = 0; k < width; ++k)
            expect_eq("dirichlet recorder alpha", grad[n * width + k],
                      av(k).adj());
          if (!(propto && mask == 0)) {
            ex.param_ptr(a)[0] = -1;
            bool threw = false;
            try {
              ex.gradient(grad.data());
            } catch (const std::domain_error&) {
              threw = true;
            }
            expect_eq("dirichlet recorder validation", threw, true);
            ex.param_ptr(a)[0] = 0.5;
            expect_eq("dirichlet recorder error recovery",
                      ex.gradient(grad.data()), ref.val());
          }
        }
}

static void check_probit_alias() {
  using namespace stanli;
  using stan::math::var;
  for (double weight : {1.0, -2.75, 0.0}) {
    Graph g;
    const int input = g.add_slot(3, true), w = g.add_slot(1, false);
    const int lp = g.add_slot(1, false), scaled = g.add_slot(1, false);
    const std::vector<int> outcomes{1, 2, 4};
    g.add_op(OP_ORDERED_PROBIT_LPMF, {input, input}, lp,
             {1, 2, 4, kVectorizedDensityLayoutMarker, 3, -1});
    g.add_op(OP_MUL, {lp, w}, scaled);
    g.result_slot = scaled;
    Executor ex(std::move(g));
    ex.value_ptr(w)[0] = weight;
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<var, -1, 1> locations(3), cuts(3);
    for (int i = 0; i < 3; ++i)
      locations(i) = cuts(i) = ex.param_ptr(input)[i] = -1.0 + 1.5 * i;
    // Bind independent copies, matching the legacy kernel's two input edges,
    // then scatter both into the aliased caller slot.
    for (int i = 0; i < 3; ++i) cuts(i) = cuts(i).val();
    var ref =
        stan::math::ordered_probit_lpmf(outcomes, locations, cuts) * weight;
    stan::math::grad(ref.vi_);
    double gradient[3];
    expect_eq("probit aliased value", ex.gradient(gradient), ref.val());
    for (int i = 0; i < 3; ++i) {
      const double expected = locations(i).adj() + cuts(i).adj();
      expect_eq(
          "probit aliased gradient",
          std::abs(gradient[i] - expected) <= 1e-12 * (1 + std::abs(expected)),
          true);
    }
  }
}

static void check_wiener_fixed_observation() {
  using namespace stanli;
  using stan::math::var;
  for (bool propto : {false, true})
    for (int active_mask = 0; active_mask < 4; ++active_mask)
      for (double weight : {1.0, -2.75, 0.0, 1e308, 1e-308,
                            std::numeric_limits<double>::infinity()})
        for (double y : {0.201, 0.8, 2.0}) {
          Graph g;
          std::vector<int> slots;
          for (int i = 0; i < 5; ++i)
            slots.push_back(g.add_slot(1, i == 0   ? (active_mask & 1)
                                          : i == 3 ? (active_mask & 2)
                                                   : true));
          const int w = g.add_slot(1, false), lp = g.add_slot(1, false),
                    scaled = g.add_slot(1, false);
          g.add_op(OP_WIENER_LPDF,
                   {slots[0], slots[1], slots[2], slots[3], slots[4]}, lp);
          g.ops.back().variant = propto ? 0x9fu : 0x1fu;
          g.add_op(OP_MUL, {lp, w}, scaled);
          g.result_slot = scaled;
          Executor ex(std::move(g));
          const double vals[] = {y, 1.4, 0.2, 0.35, -0.4};
          for (int i = 0; i < 5; ++i) ex.value_ptr(slots[i])[0] = vals[i];
          ex.value_ptr(w)[0] = weight;
          stan::math::nested_rev_autodiff nested;
          var v[5];
          for (int i = 0; i < 5; ++i) v[i] = vals[i];
          var ref = (propto ? stan::math::wiener_lpdf<true>(v[0], v[1], v[2],
                                                            v[3], v[4])
                            : stan::math::wiener_lpdf<false>(v[0], v[1], v[2],
                                                             v[3], v[4])) *
                    weight;
          stan::math::grad(ref.vi_);
          std::vector<double> grad(ex.n_params());
          const double got = ex.gradient(grad.data());
          expect_eq("wiener value", got, ref.val());
          int at = 0;
          for (int i = 0; i < 5; ++i) {
            if ((i == 0 && !(active_mask & 1)) ||
                (i == 3 && !(active_mask & 2)))
              continue;
            const double want = v[i].adj();
            if (std::isfinite(want))
              expect_eq(
                  "wiener weighted gradient",
                  std::abs(grad[at++] - want) <= 1e-12 * (1 + std::abs(want)),
                  true);
            else
              expect_eq("wiener nonfinite seed", grad[at++], want);
          }
          ex.value_ptr(slots[0])[0] = 0.1;
          bool threw = false;
          try {
            ex.gradient(grad.data());
          } catch (const std::domain_error&) {
            threw = true;
          }
          expect_eq("wiener validation", threw, true);
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

  // eta in the same order as OP_MATVEC then OP_BCAST_FMA (b = 1.0).
  // kX is column-major (Stan/Eigen convention).
  Eigen::Matrix<var, -1, 1> eta(N);
  for (int r = 0; r < N; ++r) {
    var acc = 0.0;
    for (int c = 0; c < K; ++c) acc += LogisticGlm::kX[c * N + r] * beta(c);
    eta(r) = alpha + 1.0 * acc;
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
  for (bool array_cuts : {false, true})
    for (bool scalar_location : {false, true})
      for (bool propto : {false, true})
        for (double weight : {1.0, -2.75, 0.0})
          for (int width : {0, 1, 3}) {
            check_ordered_density(array_cuts, scalar_location, propto, weight,
                                  width);
            if (width)
              for (double shift : {-30.0, 0.0, 30.0})
                check_ordered_density(array_cuts, scalar_location, propto,
                                      weight, width, true, shift);
          }
  check_multinomial_density();
  check_dirichlet_recorder();
  check_probit_alias();
  check_wiener_fixed_observation();
  check_vector_alphas();
  check_active_designs();

  check_empty_glm(OP_BERNOULLI_LOGIT_GLM_LPMF, "bernoulli_logit_glm");
  check_empty_glm(OP_POISSON_LOG_GLM_LPMF, "poisson_log_glm");
  check_empty_glm(OP_NEG_BINOMIAL_2_LOG_GLM_LPMF, "neg_binomial_2_log_glm");

  if (failures == 0) std::printf("test_glm OK\n");
  return failures == 0 ? 0 : 1;
}
