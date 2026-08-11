// One-op graphs per density: value and every parameter gradient must match
// an in-process var-path evaluation of the same call, bitwise.
#include "graph_helpers.hpp"

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <functional>
#include <string>
#include <vector>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-32s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}
static void expect_nan(const std::string& what, double got, double want) {
  if (!std::isnan(got) || !std::isnan(want)) {
    ++failures;
    std::printf("FAIL %-32s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}
// For comparisons across different instantiation activity. Kernels bind every
// argument as rvar; a reference with data (double) arguments makes stan-math
// take different to_ref_if caching paths, which reassociates shared
// subexpressions. The recorder itself is exact (see the same-activity bitwise
// checks); this bounds the all-rvar evaluation-order divergence.
static void expect_close(const std::string& what, double got, double want) {
  const double tol = 1e-13 * (std::abs(want) > 1 ? std::abs(want) : 1.0);
  if (std::abs(got - want) > tol) {
    ++failures;
    std::printf("FAIL %-32s got %.17g want %.17g (tol %g)\n", what.c_str(), got,
                want, tol);
  }
}

static std::vector<double> ys{1.3, -0.4, 2.2, 0.1, -1.7};
static std::vector<double> pos{0.9, 1.7, 0.35, 2.4, 1.1};
static std::vector<double> unit{0.2, 0.5, 0.75, 0.9, 0.33};
static const int N = 5;

// ---- elementwise variant (bit 6) -------------------------------------------
// The fused elementwise op against the N scalar lane ops it replaces: INDEX
// each vector argument per lane, run the scalar density with the same
// mask/propto bits, SET_INDEX the lp into a threaded vector, SUM_VEC. That is
// the exact graph shape lowering emits for a per-observation loop, so values,
// per-element lps, and every gradient must match bitwise.
struct EltRun {
  double value;
  std::vector<double> out;
  std::vector<double> grad;
};

static EltRun run_elt_fused(uint16_t opcode, uint8_t variant, int64_t n,
                            const std::vector<std::vector<double>>& vals,
                            const std::vector<bool>& params,
                            std::vector<int> idata) {
  using namespace stanli;
  Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (size_t i = 0; i < vals.size(); ++i) {
    slots.push_back(g.add_slot((int64_t)vals[i].size(), params[i]));
    if (params[i]) n_par += (int64_t)vals[i].size();
  }
  const int out = g.add_slot(n, false);
  const int lp = g.add_slot(1, false);
  stanli::Op op;
  op.opcode = opcode;
  op.variant = (uint8_t)(variant | 0x40u);
  op.out = out;
  for (int s : slots) op.in[op.n_in++] = s;
  if (!idata.empty()) {
    g.idata_pool.push_back(std::move(idata));
    op.idata = g.idata_pool.back().data();
    op.n_idata = (int64_t)g.idata_pool.back().size();
  }
  g.ops.push_back(op);
  g.add_op(OP_SUM_VEC, {out}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i)
    for (size_t j = 0; j < vals[i].size(); ++j)
      ex.value_ptr(slots[i])[j] = vals[i][j];
  EltRun r;
  r.grad.assign(n_par, 0.0);
  r.value = ex.gradient(r.grad.data());
  r.out.assign(ex.value_ptr(out), ex.value_ptr(out) + n);
  return r;
}

static EltRun run_elt_lanes(
    uint16_t opcode, uint8_t variant, int64_t n,
    const std::vector<std::vector<double>>& vals,
    const std::vector<bool>& params,
    const std::function<std::vector<int>(int64_t)>& lane_idata) {
  using namespace stanli;
  Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (size_t i = 0; i < vals.size(); ++i) {
    slots.push_back(g.add_slot((int64_t)vals[i].size(), params[i]));
    if (params[i]) n_par += (int64_t)vals[i].size();
  }
  int vec = g.add_slot(n, false);
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
    op.variant = variant;
    op.out = fi;
    for (int s : args) op.in[op.n_in++] = s;
    if (lane_idata) {
      auto id = lane_idata(i);
      if (!id.empty()) {
        g.idata_pool.push_back(std::move(id));
        op.idata = g.idata_pool.back().data();
        op.n_idata = (int64_t)g.idata_pool.back().size();
      }
    }
    g.ops.push_back(op);
    const int next = g.add_slot(n, false);
    g.add_op(OP_SET_INDEX, {vec, fi}, next, {(int)i});
    vec = next;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {vec}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i)
    for (size_t j = 0; j < vals[i].size(); ++j)
      ex.value_ptr(slots[i])[j] = vals[i][j];
  EltRun r;
  r.grad.assign(n_par, 0.0);
  r.value = ex.gradient(r.grad.data());
  r.out.assign(ex.value_ptr(vec), ex.value_ptr(vec) + n);
  return r;
}

static void check_elt(
    const std::string& tag, uint16_t opcode, uint8_t variant, int64_t n,
    const std::vector<std::vector<double>>& vals,
    const std::vector<bool>& params, std::vector<int> fused_idata = {},
    const std::function<std::vector<int>(int64_t)>& lane_idata = nullptr) {
  const EltRun f =
      run_elt_fused(opcode, variant, n, vals, params, std::move(fused_idata));
  const EltRun l = run_elt_lanes(opcode, variant, n, vals, params, lane_idata);
  expect_eq(tag + " lp", f.value, l.value);
  for (int64_t i = 0; i < n; ++i)
    expect_eq(tag + " out" + std::to_string(i), f.out[i], l.out[i]);
  for (size_t i = 0; i < f.grad.size(); ++i)
    expect_eq(tag + " g" + std::to_string(i), f.grad[i], l.grad[i]);
}

int main() {
  using namespace stanli;
  using stan::math::var;

  // ---- uniform_lpdf out of support: -inf, no partials --------------------
  // stan-math reports out-of-support y through an early return that never
  // reaches the partials sink. This was the first instance caught by the
  // dogs_log reference: CmdStan said -inf while stanli said finite.
  {
    auto r = testutil::run_one_op(OP_UNIFORM_LPDF, {{0.1}, {-100.0}, {0.0}},
                                  {true, false, false});
    expect_eq("uniform oos value", r.value,
              -std::numeric_limits<double>::infinity());
    expect_eq("uniform oos dy", r.grad[0], 0.0);
    auto r2 = testutil::run_one_op(OP_UNIFORM_LPDF, {{-0.5}, {-100.0}, {0.0}},
                                   {true, false, false});
    var vy = -0.5;
    var lp = stan::math::uniform_lpdf<false>(vy, -100.0, 0.0);
    lp.grad();
    expect_eq("uniform in value", r2.value, lp.val());
    expect_eq("uniform in dy", r2.grad[0], vy.adj());
    stan::math::recover_memory();
  }

  // ---- inv_gamma_lpdf early return: -inf, no partials ------------------
  // Stan Math returns LOG_ZERO before building its partials propagator when
  // any y is nonpositive. The returned value and zero pullback are both part
  // of the contract: a recorder cannot infer them from a build() call that
  // never happens.
  {
    Graph g;
    const int y = g.add_slot(1, true);
    const int alpha = g.add_slot(1, false);
    const int beta = g.add_slot(1, false);
    const int lp_slot = g.add_slot(1, false);
    g.add_op(OP_INV_GAMMA_LPDF, {y, alpha, beta}, lp_slot);
    g.result_slot = lp_slot;
    Executor ex(std::move(g));
    ex.value_ptr(alpha)[0] = 2.0;
    ex.value_ptr(beta)[0] = 3.0;

    // Seed the density scratch with a real derivative, then cross support.
    // The second call must not reuse either the value or that derivative.
    ex.params_data()[0] = 1.5;
    double grad = 0;
    const double valid = ex.gradient(&grad);
    var vy = 1.5;
    var valid_ref = stan::math::inv_gamma_lpdf<false>(vy, 2.0, 3.0);
    valid_ref.grad();
    expect_eq("inv_gamma valid value", valid, valid_ref.val());
    expect_eq("inv_gamma valid dy", grad, vy.adj());
    stan::math::recover_memory();

    ex.params_data()[0] = -1.0;
    grad = std::numeric_limits<double>::quiet_NaN();
    const double invalid = ex.gradient(&grad);
    var bad_y = -1.0;
    var invalid_ref = stan::math::inv_gamma_lpdf<false>(bad_y, 2.0, 3.0);
    invalid_ref.grad();
    expect_eq("inv_gamma oos value", invalid, invalid_ref.val());
    expect_eq("inv_gamma oos dy", grad, bad_y.adj());
    stan::math::recover_memory();
  }

  // A vector call returns one LOG_ZERO for the whole density. This is the
  // non-rerolled shape lowering emits for a vectorized sampling statement.
  {
    const std::vector<double> yv{1.5, -1.0, 2.25};
    auto r = testutil::run_one_op(OP_INV_GAMMA_LPDF, {yv, {2.0}, {3.0}},
                                  {true, false, false});
    Eigen::Matrix<var, -1, 1> y_ref(3);
    for (int i = 0; i < 3; ++i) y_ref(i) = yv[(size_t)i];
    var lp_ref = stan::math::inv_gamma_lpdf<false>(y_ref, 2.0, 3.0);
    lp_ref.grad();
    expect_eq("inv_gamma vector value", r.value, lp_ref.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("inv_gamma vector dy" + std::to_string(i), r.grad[(size_t)i],
                y_ref(i).adj());
    stan::math::recover_memory();
  }

  // A literal early return is disconnected, not merely an edge whose local
  // derivative happens to be zero. Squaring -inf sends an infinite adjoint
  // toward the density; Stan leaves y untouched rather than forming inf * 0.
  {
    Graph g;
    const int y = g.add_slot(1, true);
    const int alpha = g.add_slot(1, false);
    const int beta = g.add_slot(1, false);
    const int lp_slot = g.add_slot(1, false);
    const int squared = g.add_slot(1, false);
    g.add_op(OP_INV_GAMMA_LPDF, {y, alpha, beta}, lp_slot);
    g.add_op(OP_SQUARE, {lp_slot}, squared);
    g.result_slot = squared;
    Executor ex(std::move(g));
    ex.value_ptr(alpha)[0] = 2.0;
    ex.value_ptr(beta)[0] = 3.0;
    ex.params_data()[0] = -1.0;
    double grad = std::numeric_limits<double>::quiet_NaN();
    const double value = ex.gradient(&grad);

    var y_ref = -1.0;
    var lp_ref = stan::math::inv_gamma_lpdf<false>(y_ref, 2.0, 3.0);
    var value_ref = stan::math::square(lp_ref);
    value_ref.grad();
    expect_eq("inv_gamma disconnected value", value, value_ref.val());
    expect_eq("inv_gamma disconnected dy", grad, y_ref.adj());
    stan::math::recover_memory();
  }

  // Conversely, a normal build remains connected even when its local
  // derivative happens to be zero. At y=beta/(alpha+1), inv_gamma's d/dy is
  // exactly zero; an infinite upstream adjoint therefore produces NaN on
  // both Stan's edge and the Graph edge. The connectivity flag must not
  // suppress that multiplication.
  {
    Graph g;
    const int y = g.add_slot(1, true);
    const int alpha = g.add_slot(1, false);
    const int beta = g.add_slot(1, false);
    const int infinity = g.add_slot(1, false);
    const int lp_slot = g.add_slot(1, false);
    const int scaled = g.add_slot(1, false);
    g.add_op(OP_INV_GAMMA_LPDF, {y, alpha, beta}, lp_slot);
    g.add_op(OP_MUL, {lp_slot, infinity}, scaled);
    g.result_slot = scaled;
    Executor ex(std::move(g));
    ex.value_ptr(alpha)[0] = 2.0;
    ex.value_ptr(beta)[0] = 3.0;
    ex.value_ptr(infinity)[0] = std::numeric_limits<double>::infinity();
    ex.params_data()[0] = 1.0;
    double grad = 0;
    const double value = ex.gradient(&grad);

    var y_ref = 1.0;
    var lp_ref = stan::math::inv_gamma_lpdf<false>(y_ref, 2.0, 3.0);
    var value_ref = lp_ref * std::numeric_limits<double>::infinity();
    value_ref.grad();
    expect_eq("inv_gamma connected value", value, value_ref.val());
    expect_nan("inv_gamma connected dy", grad, y_ref.adj());
    stan::math::recover_memory();
  }

  // The fused elementwise kernel reuses one sink across lanes. An invalid
  // lane between valid lanes must record LOG_ZERO and zero only its own
  // partial, without inheriting the preceding lane's value.
  {
    const std::vector<double> yv{1.5, -1.0, 2.25};
    const EltRun r =
        run_elt_fused(OP_INV_GAMMA_LPDF, 0x01, 3, {yv, {2.0}, {3.0}},
                      {true, false, false}, {});
    double total = 0;
    for (int i = 0; i < 3; ++i) {
      var y_ref = yv[(size_t)i];
      var lp_ref = stan::math::inv_gamma_lpdf<false>(y_ref, 2.0, 3.0);
      lp_ref.grad();
      total += lp_ref.val();
      expect_eq("inv_gamma elt value" + std::to_string(i), r.out[(size_t)i],
                lp_ref.val());
      expect_eq("inv_gamma elt dy" + std::to_string(i), r.grad[(size_t)i],
                y_ref.adj());
      stan::math::recover_memory();
    }
    expect_eq("inv_gamma elt sum", r.value, total);
  }

  // Connectivity is per lane in the fused form: only the invalid lane is
  // disconnected when downstream arithmetic sends it an infinite adjoint.
  {
    const std::vector<double> yv{1.5, -1.0, 2.25};
    Graph g;
    const int y = g.add_slot(3, true);
    const int alpha = g.add_slot(1, false);
    const int beta = g.add_slot(1, false);
    const int lp = g.add_slot(3, false);
    const int squared = g.add_slot(3, false);
    const int total = g.add_slot(1, false);
    Op density;
    density.opcode = OP_INV_GAMMA_LPDF;
    density.variant = 0x41;
    density.out = lp;
    density.in[0] = y;
    density.in[1] = alpha;
    density.in[2] = beta;
    density.n_in = 3;
    g.ops.push_back(density);
    g.add_op(OP_SQUARE, {lp}, squared);
    g.add_op(OP_SUM_VEC, {squared}, total);
    g.result_slot = total;
    Executor ex(std::move(g));
    ex.value_ptr(alpha)[0] = 2.0;
    ex.value_ptr(beta)[0] = 3.0;
    for (int i = 0; i < 3; ++i) ex.params_data()[i] = yv[(size_t)i];
    double grad[3] = {0, 0, 0};
    const double value = ex.gradient(grad);

    double value_ref = 0;
    for (int i = 0; i < 3; ++i) {
      var y_ref = yv[(size_t)i];
      var lp_ref = stan::math::inv_gamma_lpdf<false>(y_ref, 2.0, 3.0);
      var squared_ref = stan::math::square(lp_ref);
      squared_ref.grad();
      value_ref += squared_ref.val();
      expect_eq("inv_gamma elt disconnected dy" + std::to_string(i), grad[i],
                y_ref.adj());
      stan::math::recover_memory();
    }
    expect_eq("inv_gamma elt disconnected value", value, value_ref);
  }

  // ---- normal_lpdf(y_pv, mu_ps, sigma_ps): all three parameters ----------
  {
    auto r = testutil::run_one_op(OP_NORMAL_LPDF, {ys, {0.25}, {1.4}},
                                  {true, true, true});
    Eigen::Matrix<var, -1, 1> vy(N);
    for (int i = 0; i < N; ++i) vy(i) = ys[i];
    var vmu = 0.25, vsig = 1.4;
    var lp = stan::math::normal_lpdf<false>(vy, vmu, vsig);
    lp.grad();
    expect_eq("normal ppp value", r.value, lp.val());
    for (int i = 0; i < N; ++i)
      expect_eq("normal ppp dy" + std::to_string(i), r.grad[i], vy(i).adj());
    expect_eq("normal ppp dmu", r.grad[N], vmu.adj());
    expect_eq("normal ppp dsigma", r.grad[N + 1], vsig.adj());
    stan::math::recover_memory();
  }

  // ---- normal_lpdf(y_data, mu_ps, sigma_ps) ------------------------------
  {
    auto r = testutil::run_one_op(OP_NORMAL_LPDF, {ys, {0.25}, {1.4}},
                                  {false, true, true});
    Eigen::Map<Eigen::VectorXd> ymap(ys.data(), N);
    var vmu = 0.25, vsig = 1.4;
    var lp = stan::math::normal_lpdf<false>(ymap, vmu, vsig);
    lp.grad();
    expect_eq("normal dpp value", r.value, lp.val());
    expect_eq("normal dpp dmu", r.grad[0], vmu.adj());
    expect_eq("normal dpp dsigma", r.grad[1], vsig.adj());
    stan::math::recover_memory();
  }

  // ---- normal_lpdf(y_data, mu_pv, sigma_data): vector location -----------
  {
    std::vector<double> mus{0.1, -0.2, 0.3, 0.05, -0.6};
    std::vector<double> sigs{15, 10, 16, 11, 9};
    auto r = testutil::run_one_op(OP_NORMAL_LPDF, {ys, mus, sigs},
                                  {false, true, false});
    Eigen::Map<Eigen::VectorXd> ymap(ys.data(), N);
    Eigen::Map<Eigen::VectorXd> smap(sigs.data(), N);
    Eigen::Matrix<var, -1, 1> vmu(N);
    for (int i = 0; i < N; ++i) vmu(i) = mus[i];
    var lp = stan::math::normal_lpdf<false>(ymap, vmu, smap);
    lp.grad();
    expect_eq("normal dpd value", r.value, lp.val());
    for (int i = 0; i < N; ++i)
      expect_eq("normal dpd dmu" + std::to_string(i), r.grad[i], vmu(i).adj());
    stan::math::recover_memory();
  }

  // ---- cauchy_lpdf(y_ps, 0_data, 5_data) ---------------------------------
  {
    auto r = testutil::run_one_op(OP_CAUCHY_LPDF, {{2.3}, {0.0}, {5.0}},
                                  {true, false, false});
    var vt = 2.3;
    var lp = stan::math::cauchy_lpdf<false>(vt, 0.0, 5.0);
    lp.grad();
    expect_eq("cauchy value", r.value, lp.val());
    expect_eq("cauchy dy", r.grad[0], vt.adj());
    stan::math::recover_memory();
  }

  // ---- student_t_lpdf(y_data, nu_ps, mu_ps, sigma_ps) --------------------
  {
    auto r = testutil::run_one_op(OP_STUDENT_T_LPDF, {ys, {4.0}, {0.25}, {1.4}},
                                  {false, true, true, true});
    Eigen::Map<Eigen::VectorXd> ymap(ys.data(), N);
    var vnu = 4.0, vmu = 0.25, vsig = 1.4;
    var lp = stan::math::student_t_lpdf<false>(ymap, vnu, vmu, vsig);
    lp.grad();
    expect_eq("student_t value", r.value, lp.val());
    expect_close("student_t dnu", r.grad[0], vnu.adj());
    expect_close("student_t dmu", r.grad[1], vmu.adj());
    expect_close("student_t dsigma", r.grad[2], vsig.adj());
    stan::math::recover_memory();

    // Same-activity reference: y promoted to var as the kernel promotes it
    // to rvar. The recorder mechanism must be bitwise here.
    Eigen::Matrix<var, -1, 1> vy(N);
    for (int i = 0; i < N; ++i) vy(i) = ys[i];
    var wnu = 4.0, wmu = 0.25, wsig = 1.4;
    var lp2 = stan::math::student_t_lpdf<false>(vy, wnu, wmu, wsig);
    lp2.grad();
    expect_eq("student_t allvar value", r.value, lp2.val());
    expect_eq("student_t allvar dnu", r.grad[0], wnu.adj());
    expect_eq("student_t allvar dmu", r.grad[1], wmu.adj());
    expect_eq("student_t allvar dsigma", r.grad[2], wsig.adj());
    stan::math::recover_memory();
  }

  // ---- gamma_lpdf(y_data, alpha_ps, beta_ps) -----------------------------
  {
    auto r = testutil::run_one_op(OP_GAMMA_LPDF, {pos, {2.5}, {1.3}},
                                  {false, true, true});
    Eigen::Map<Eigen::VectorXd> ymap(pos.data(), N);
    var va = 2.5, vb = 1.3;
    var lp = stan::math::gamma_lpdf<false>(ymap, va, vb);
    lp.grad();
    expect_eq("gamma value", r.value, lp.val());
    expect_eq("gamma dalpha", r.grad[0], va.adj());
    expect_eq("gamma dbeta", r.grad[1], vb.adj());
    stan::math::recover_memory();
  }

  // ---- beta_lpdf(y_data, alpha_ps, beta_ps) ------------------------------
  {
    auto r = testutil::run_one_op(OP_BETA_LPDF, {unit, {2.0}, {3.0}},
                                  {false, true, true});
    Eigen::Map<Eigen::VectorXd> ymap(unit.data(), N);
    var va = 2.0, vb = 3.0;
    var lp = stan::math::beta_lpdf<false>(ymap, va, vb);
    lp.grad();
    expect_eq("beta value", r.value, lp.val());
    expect_eq("beta dalpha", r.grad[0], va.adj());
    expect_eq("beta dbeta", r.grad[1], vb.adj());
    stan::math::recover_memory();
  }

  // ---- poisson_log_lpmf(n_idata; alpha_pv) -------------------------------
  {
    std::vector<double> alpha{0.2, 0.4, 0.6, 0.8, 1.0};
    auto r = testutil::run_one_op(OP_POISSON_LOG_LPMF, {alpha}, {true},
                                  {2, 0, 5, 1, 3});
    std::vector<int> n{2, 0, 5, 1, 3};
    Eigen::Matrix<var, -1, 1> va(N);
    for (int i = 0; i < N; ++i) va(i) = alpha[i];
    var lp = stan::math::poisson_log_lpmf<false>(n, va);
    lp.grad();
    expect_eq("poisson_log value", r.value, lp.val());
    for (int i = 0; i < N; ++i)
      expect_eq("poisson_log da" + std::to_string(i), r.grad[i], va(i).adj());
    stan::math::recover_memory();
  }

  // ---- bernoulli_logit_lpmf(y_idata; alpha_pv) ---------------------------
  {
    std::vector<double> alpha{0.5, -1.2, 0.3, 2.0, -0.7};
    auto r = testutil::run_one_op(OP_BERNOULLI_LOGIT_LPMF, {alpha}, {true},
                                  {1, 0, 0, 1, 1});
    std::vector<int> y{1, 0, 0, 1, 1};
    Eigen::Matrix<var, -1, 1> va(N);
    for (int i = 0; i < N; ++i) va(i) = alpha[i];
    var lp = stan::math::bernoulli_logit_lpmf<false>(y, va);
    lp.grad();
    expect_eq("bernoulli_logit value", r.value, lp.val());
    for (int i = 0; i < N; ++i)
      expect_eq("bernoulli_logit da" + std::to_string(i), r.grad[i],
                va(i).adj());
    stan::math::recover_memory();
  }

  // ---- elementwise variant (bit 6) vs the scalar lanes it replaces --------
  {
    std::vector<double> mus{0.1, -0.2, 0.3, 0.05, -0.6};

    // The gauss_mix shape: y data vector, mu/sigma broadcast scalar params.
    check_elt("elt normal svv", OP_NORMAL_LPDF, 0x06, N, {ys, {0.25}, {1.4}},
              {false, true, true});
    check_elt("elt normal svv propto", OP_NORMAL_LPDF, 0x86, N,
              {ys, {0.25}, {1.4}}, {false, true, true});

    // Every activity mask, propto off and on, all-vector arguments.
    for (unsigned m = 1; m < 8; ++m) {
      check_elt("elt normal mask" + std::to_string(m), OP_NORMAL_LPDF,
                (uint8_t)m, N, {ys, mus, pos}, {true, true, true});
      check_elt("elt normal mask" + std::to_string(m) + " propto",
                OP_NORMAL_LPDF, (uint8_t)(0x80u | m), N, {ys, mus, pos},
                {true, true, true});
    }

    // idata-outcome lpmfs: fused idata is the concatenated outcomes, each
    // lane carries its own element.
    std::vector<int> yb{1, 0, 0, 1, 1};
    std::vector<double> alpha{0.5, -1.2, 0.3, 2.0, -0.7};
    check_elt("elt bern_logit", OP_BERNOULLI_LOGIT_LPMF, 0x81, N, {alpha},
              {true}, yb, [&](int64_t i) { return std::vector<int>{yb[i]}; });
    std::vector<int> counts{3, 0, 7, 1, 2};
    check_elt("elt neg_binom2 vs", OP_NEG_BINOMIAL_2_LPMF, 0x03, N,
              {pos, {3.5}}, {true, true}, counts,
              [&](int64_t i) { return std::vector<int>{counts[i]}; });

    // binomial: int groups; y vector, trials a language-level scalar (-1).
    std::vector<int> fused_b{N, 3, 0, 7, 1, 2, -1, 20};
    check_elt(
        "elt binomial", OP_BINOMIAL_LPMF, 0x01, N, {unit}, {true}, fused_b,
        [&](int64_t i) { return std::vector<int>{-1, counts[i], -1, 20}; });
  }

  if (failures == 0) std::printf("test_densities OK\n");
  return failures == 0 ? 0 : 1;
}
