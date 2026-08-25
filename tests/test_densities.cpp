// One-op graphs per density: value and every parameter gradient must match
// an in-process var-path evaluation of the same call, bitwise.
#include "graph_helpers.hpp"

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
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

// Bernoulli's native forwards deliberately replace the generic rvar
// recorder, so pin their complete contract directly to this checkout's Stan
// Math. Equality below is bit equality (including signed zero), rather than
// the numerical equality used by the older density coverage above.
static void expect_bits(const std::string& what, double got, double want) {
  if (std::memcmp(&got, &want, sizeof(double)) != 0) {
    ++failures;
    std::printf("FAIL %-32s got %.17g want %.17g (bit mismatch)\n",
                what.c_str(), got, want);
  }
}

static void expect_true(const std::string& what, bool got) {
  if (!got) {
    ++failures;
    std::printf("FAIL %-32s false\n", what.c_str());
  }
}

static stanli::testutil::RunResult run_bernoulli_variant(
    uint16_t opcode, uint8_t variant, const std::vector<int>& y,
    const std::vector<double>& theta) {
  using namespace stanli;
  Graph g;
  const int theta_slot = g.add_slot((int64_t)theta.size(), true);
  const int lp = g.add_slot(1, false);
  Op op;
  op.opcode = opcode;
  op.variant = variant;
  op.out = lp;
  op.in[op.n_in++] = theta_slot;
  if (!y.empty()) {
    g.idata_pool.push_back(y);
    op.idata = g.idata_pool.back().data();
    op.n_idata = (int64_t)y.size();
  }
  g.ops.push_back(op);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (size_t i = 0; i < theta.size(); ++i) ex.params_data()[i] = theta[i];
  stanli::testutil::RunResult r;
  r.grad.assign(theta.size(), 0.0);
  r.value = ex.gradient(r.grad.data());
  return r;
}

static stanli::testutil::RunResult run_bernoulli_scaled(
    uint16_t opcode, uint8_t variant, const std::vector<int>& y, double theta,
    double scale) {
  using namespace stanli;
  Graph g;
  const int theta_slot = g.add_slot(1, true);
  const int scale_slot = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  const int result = g.add_slot(1, false);
  Op op;
  op.opcode = opcode;
  op.variant = variant;
  op.out = lp;
  op.in[op.n_in++] = theta_slot;
  if (!y.empty()) {
    g.idata_pool.push_back(y);
    op.idata = g.idata_pool.back().data();
    op.n_idata = (int64_t)y.size();
  }
  g.ops.push_back(op);
  g.add_op(OP_MUL, {lp, scale_slot}, result);
  g.result_slot = result;

  Executor ex(std::move(g));
  ex.params_data()[0] = theta;
  ex.value_ptr(scale_slot)[0] = scale;
  stanli::testutil::RunResult r;
  r.grad.assign(1, 0.0);
  r.value = ex.gradient(r.grad.data());
  return r;
}

struct BernoulliRef {
  double value = 0.0;
  std::vector<double> grad;
};

template <bool Logit, bool Propto, typename Y, typename Theta>
static auto bernoulli_math(const Y& y, const Theta& theta) {
  if constexpr (Logit)
    return stan::math::bernoulli_logit_lpmf<Propto>(y, theta);
  else
    return stan::math::bernoulli_lpmf<Propto>(y, theta);
}

static Eigen::VectorXi as_int_vector(const std::vector<int>& x) {
  Eigen::VectorXi out((Eigen::Index)x.size());
  for (size_t i = 0; i < x.size(); ++i) out((Eigen::Index)i) = x[i];
  return out;
}

template <bool Logit, bool Propto>
static BernoulliRef run_bernoulli_reference(const std::vector<int>& y,
                                            const std::vector<double>& theta,
                                            bool scalar_outcome, bool active) {
  using stan::math::var;
  BernoulliRef r;
  r.grad.assign(theta.size(), 0.0);
  const Eigen::VectorXi y_vec = as_int_vector(y);

  if (!active) {
    if (theta.size() == 1) {
      r.value = scalar_outcome ? bernoulli_math<Logit, Propto>(y[0], theta[0])
                               : bernoulli_math<Logit, Propto>(y_vec, theta[0]);
    } else {
      const Eigen::Map<const Eigen::VectorXd> theta_vec(theta.data(),
                                                        theta.size());
      r.value = scalar_outcome
                    ? bernoulli_math<Logit, Propto>(y[0], theta_vec)
                    : bernoulli_math<Logit, Propto>(y_vec, theta_vec);
    }
    return r;
  }

  if (theta.size() == 1) {
    var theta_var = theta[0];
    var lp;
    if (scalar_outcome)
      lp = bernoulli_math<Logit, Propto>(y[0], theta_var);
    else
      lp = bernoulli_math<Logit, Propto>(y_vec, theta_var);
    lp.grad();
    r.value = lp.val();
    r.grad[0] = theta_var.adj();
  } else {
    Eigen::Matrix<var, -1, 1> theta_var((Eigen::Index)theta.size());
    for (size_t i = 0; i < theta.size(); ++i)
      theta_var((Eigen::Index)i) = theta[i];
    var lp;
    if (scalar_outcome)
      lp = bernoulli_math<Logit, Propto>(y[0], theta_var);
    else
      lp = bernoulli_math<Logit, Propto>(y_vec, theta_var);
    lp.grad();
    r.value = lp.val();
    for (size_t i = 0; i < theta.size(); ++i)
      r.grad[i] = theta_var((Eigen::Index)i).adj();
  }
  stan::math::recover_memory();
  return r;
}

static BernoulliRef run_bernoulli_reference(uint16_t opcode, uint8_t variant,
                                            const std::vector<int>& y,
                                            const std::vector<double>& theta,
                                            bool scalar_outcome) {
  const bool logit = opcode == stanli::OP_BERNOULLI_LOGIT_LPMF;
  const bool propto = (variant & 0x80u) != 0;
  const bool active = variant == 0 || (variant & 0x01u) != 0;
  if (logit) {
    if (propto)
      return run_bernoulli_reference<true, true>(y, theta, scalar_outcome,
                                                 active);
    return run_bernoulli_reference<true, false>(y, theta, scalar_outcome,
                                                active);
  }
  if (propto)
    return run_bernoulli_reference<false, true>(y, theta, scalar_outcome,
                                                active);
  return run_bernoulli_reference<false, false>(y, theta, scalar_outcome,
                                               active);
}

static void check_bernoulli_run(const std::string& tag, uint16_t opcode,
                                uint8_t variant, const std::vector<int>& y,
                                const std::vector<double>& theta,
                                bool scalar_outcome) {
  const auto got = run_bernoulli_variant(opcode, variant, y, theta);
  const auto want =
      run_bernoulli_reference(opcode, variant, y, theta, scalar_outcome);
  expect_bits(tag + " value", got.value, want.value);
  expect_true(tag + " grad size", got.grad.size() == want.grad.size());
  const size_t n = std::min(got.grad.size(), want.grad.size());
  for (size_t i = 0; i < n; ++i)
    expect_bits(tag + " grad" + std::to_string(i), got.grad[i], want.grad[i]);
}

template <bool Logit, bool Propto>
static EltRun run_bernoulli_elt_reference(uint8_t variant,
                                          const std::vector<int>& y,
                                          const std::vector<double>& theta) {
  using stan::math::var;
  EltRun r;
  r.out.reserve(y.size());
  r.grad.assign(theta.size(), 0.0);
  // Bit 6 makes an otherwise-zero variant explicit, so elementwise activity
  // always comes from bit 0 rather than the variant == 0 default.
  const bool active = (variant & 0x01u) != 0;
  if (!active) {
    for (size_t i = 0; i < y.size(); ++i) {
      const double theta_i = theta[theta.size() == 1 ? 0 : i];
      r.out.push_back(bernoulli_math<Logit, Propto>(y[i], theta_i));
    }
    r.value = 0.0;
    for (double x : r.out) r.value += x;
    return r;
  }

  var total = 0.0;
  if (theta.size() == 1) {
    var theta_var = theta[0];
    for (size_t i = 0; i < y.size(); ++i) {
      var lane = bernoulli_math<Logit, Propto>(y[i], theta_var);
      r.out.push_back(lane.val());
      total += lane;
    }
    total.grad();
    r.value = total.val();
    r.grad[0] = theta_var.adj();
  } else {
    Eigen::Matrix<var, -1, 1> theta_var((Eigen::Index)theta.size());
    for (size_t i = 0; i < theta.size(); ++i)
      theta_var((Eigen::Index)i) = theta[i];
    for (size_t i = 0; i < y.size(); ++i) {
      var lane =
          bernoulli_math<Logit, Propto>(y[i], theta_var((Eigen::Index)i));
      r.out.push_back(lane.val());
      total += lane;
    }
    total.grad();
    r.value = total.val();
    for (size_t i = 0; i < theta.size(); ++i)
      r.grad[i] = theta_var((Eigen::Index)i).adj();
  }
  stan::math::recover_memory();
  return r;
}

static EltRun run_bernoulli_elt_reference(uint16_t opcode, uint8_t variant,
                                          const std::vector<int>& y,
                                          const std::vector<double>& theta) {
  const bool logit = opcode == stanli::OP_BERNOULLI_LOGIT_LPMF;
  const bool propto = (variant & 0x80u) != 0;
  if (logit) {
    if (propto)
      return run_bernoulli_elt_reference<true, true>(variant, y, theta);
    return run_bernoulli_elt_reference<true, false>(variant, y, theta);
  }
  if (propto)
    return run_bernoulli_elt_reference<false, true>(variant, y, theta);
  return run_bernoulli_elt_reference<false, false>(variant, y, theta);
}

static void check_bernoulli_elt(const std::string& tag, uint16_t opcode,
                                uint8_t variant, const std::vector<int>& y,
                                const std::vector<double>& theta) {
  const EltRun got =
      run_elt_fused(opcode, variant, (int64_t)y.size(), {theta}, {true}, y);
  const EltRun want = run_bernoulli_elt_reference(opcode, variant, y, theta);
  expect_bits(tag + " value", got.value, want.value);
  expect_true(tag + " out size", got.out.size() == want.out.size());
  const size_t n_out = std::min(got.out.size(), want.out.size());
  for (size_t i = 0; i < n_out; ++i)
    expect_bits(tag + " out" + std::to_string(i), got.out[i], want.out[i]);
  expect_true(tag + " grad size", got.grad.size() == want.grad.size());
  const size_t n_grad = std::min(got.grad.size(), want.grad.size());
  for (size_t i = 0; i < n_grad; ++i)
    expect_bits(tag + " grad" + std::to_string(i), got.grad[i], want.grad[i]);
}

struct CapturedException {
  bool thrown = false;
  std::string kind;
  std::string message;
};

template <typename F>
static CapturedException capture_exception(F&& f) {
  try {
    f();
    return {};
  } catch (const std::domain_error& e) {
    return {true, "domain_error", e.what()};
  } catch (const std::invalid_argument& e) {
    return {true, "invalid_argument", e.what()};
  } catch (const std::runtime_error& e) {
    return {true, "runtime_error", e.what()};
  } catch (const std::exception& e) {
    return {true, "exception", e.what()};
  }
}

static void expect_exception_parity(const std::string& tag,
                                    const CapturedException& got,
                                    const CapturedException& want) {
  if (got.thrown != want.thrown || got.kind != want.kind ||
      got.message != want.message) {
    ++failures;
    std::printf("FAIL %-32s got %s '%s' want %s '%s'\n", tag.c_str(),
                got.kind.c_str(), got.message.c_str(), want.kind.c_str(),
                want.message.c_str());
  }
}

// Exercise one forward context repeatedly. Executor-bound variants and idata
// are immutable, so a direct registered-kernel call is the only focused way
// to prove that a valid call cannot leave partials/connectivity behind for a
// subsequent empty or inactive call using the same scratch bytes.
static void check_bernoulli_scratch_reset(const std::string& tag,
                                          uint16_t opcode) {
  using namespace stanli;
  const Kernel* k = find_kernel(opcode);
  expect_true(tag + " registered",
              k != nullptr && k->forward != nullptr && k->backward != nullptr);
  if (k == nullptr || k->forward == nullptr || k->backward == nullptr) return;

  int y = 1;
  double theta = opcode == OP_BERNOULLI_LPMF ? 0.4 : 0.3;
  double out = 0.0;
  double scratch[2] = {-77.0, -88.0};
  KernelCtx ctx;
  ctx.in[0] = Desc{&theta, 1};
  ctx.n_in = 1;
  ctx.out = Desc{&out, 1};
  ctx.scratch = scratch;
  ctx.idata = &y;
  ctx.n_idata = 1;

  ctx.variant = 0;
  k->forward(ctx);
  expect_bits(tag + " valid connected", scratch[1], 1.0);

  ctx.variant = 0x02u;  // explicit inactive, full density
  k->forward(ctx);
  expect_bits(tag + " inactive partial", scratch[0], 0.0);
  expect_bits(tag + " inactive connected", scratch[1], 0.0);
  double adj = 0.0;
  ctx.in_adj[0] = Desc{&adj, 1};
  ctx.out_adj = std::numeric_limits<double>::infinity();
  k->backward(ctx);
  expect_bits(tag + " inactive pullback", adj, 0.0);

  ctx.variant = 0;
  k->forward(ctx);
  expect_bits(tag + " refill connected", scratch[1], 1.0);
  ctx.n_idata = 0;
  k->forward(ctx);
  expect_bits(tag + " empty value", out, 0.0);
  expect_bits(tag + " empty partial", scratch[0], 0.0);
  expect_bits(tag + " empty connected", scratch[1], 0.0);
  adj = 0.0;
  k->backward(ctx);
  expect_bits(tag + " empty pullback", adj, 0.0);

  ctx.idata = &y;
  ctx.n_idata = 1;
  ctx.variant = 0x81u;  // active propto
  k->forward(ctx);
  expect_bits(tag + " propto connected", scratch[1], 1.0);
  ctx.variant = 0x80u;  // inactive propto drops the only summand
  k->forward(ctx);
  expect_bits(tag + " dropped value", out, 0.0);
  expect_bits(tag + " dropped partial", scratch[0], 0.0);
  expect_bits(tag + " dropped connected", scratch[1], 0.0);
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

  // ---- native Bernoulli forwards vs the pinned Stan Math checkout --------
  // Default means variant == 0 (all arguments active). Once any variant bit
  // is present, bit 0 explicitly selects activity; bit 7 selects propto.
  // These are all five representable full/propto x default/active/inactive
  // cases for a one-real-argument density.
  {
    struct Dist {
      uint16_t opcode;
      const char* tag;
      std::vector<double> vector_theta;
      double scalar_theta;
    };
    const Dist dists[] = {
        {OP_BERNOULLI_LPMF, "bernoulli native", {0.2, 0.7, 0.4, 0.9}, 0.4},
        {OP_BERNOULLI_LOGIT_LPMF,
         "bernoulli_logit native",
         {-2.0, 0.3, 1.4, -0.7},
         0.3}};
    const std::vector<int> y{1, 0, 1, 0};
    struct VariantCase {
      uint8_t variant;
      const char* tag;
    };
    const VariantCase variants[] = {{0x00u, "default full"},
                                    {0x01u, "active full"},
                                    {0x02u, "inactive full"},
                                    {0x81u, "active propto"},
                                    {0x80u, "inactive propto"}};

    for (const Dist& d : dists) {
      check_bernoulli_run(std::string(d.tag) + " scalar/scalar", d.opcode, 0,
                          {1}, {d.scalar_theta}, true);
      check_bernoulli_run(std::string(d.tag) + " vector/vector", d.opcode, 0, y,
                          d.vector_theta, false);
      check_bernoulli_run(std::string(d.tag) + " vector/broadcast", d.opcode, 0,
                          y, {d.scalar_theta}, false);
      for (const VariantCase& c : variants)
        check_bernoulli_run(std::string(d.tag) + " " + c.tag, d.opcode,
                            c.variant, y, d.vector_theta, false);

      // Reroll emits both vector and broadcast elementwise forms. Exercise
      // explicit active and inactive masks under both template flags.
      check_bernoulli_elt(std::string(d.tag) + " elt vector full", d.opcode,
                          0x01u, y, d.vector_theta);
      check_bernoulli_elt(std::string(d.tag) + " elt broadcast full", d.opcode,
                          0x01u, y, {d.scalar_theta});
      check_bernoulli_elt(std::string(d.tag) + " elt vector propto", d.opcode,
                          0x81u, y, d.vector_theta);
      check_bernoulli_elt(std::string(d.tag) + " elt broadcast propto",
                          d.opcode, 0x81u, y, {d.scalar_theta});
      check_bernoulli_elt(std::string(d.tag) + " elt inactive full", d.opcode,
                          0x02u, y, d.vector_theta);
      check_bernoulli_elt(std::string(d.tag) + " elt inactive propto", d.opcode,
                          0x80u, y, {d.scalar_theta});
    }
  }

  // Bernoulli probability endpoints are valid inputs. Pin the exact values
  // and partials on both outcomes at the endpoint, its adjacent representable
  // interior value, and one ordinary interior point.
  {
    const double p[] = {0.0, std::nextafter(0.0, 1.0), 0.4,
                        std::nextafter(1.0, 0.0), 1.0};
    for (int y = 0; y <= 1; ++y)
      for (size_t i = 0; i < sizeof(p) / sizeof(p[0]); ++i)
        check_bernoulli_run(
            "bernoulli p-grid y" + std::to_string(y) + " p" + std::to_string(i),
            OP_BERNOULLI_LPMF, 0, {y}, {p[i]}, true);
  }

  // Stan Math's logit implementation has deliberately asymmetric strict
  // cutoff comparisons: value uses z > 20 and z < -20, while the partial's
  // middle arm includes z == -20. Test both sides, both exact cutoffs, both
  // outcomes, and accepted infinities.
  {
    const double inf = std::numeric_limits<double>::infinity();
    const double z[] = {
        -inf, std::nextafter(-20.0, -inf), -20.0, std::nextafter(-20.0, inf),
        0.0,  std::nextafter(20.0, -inf),  20.0,  std::nextafter(20.0, inf),
        inf};
    for (int y = 0; y <= 1; ++y) {
      const double sign = 2.0 * y - 1.0;
      for (size_t i = 0; i < sizeof(z) / sizeof(z[0]); ++i)
        check_bernoulli_run("bernoulli_logit z-grid y" + std::to_string(y) +
                                " z" + std::to_string(i),
                            OP_BERNOULLI_LOGIT_LPMF, 0, {y}, {sign * z[i]},
                            true);
    }

    // One summed vector call reaches Eigen's packet exp, reduction, and
    // vector partial assignment. Alternate outcomes while preserving z so
    // every strict cutoff/tail arm appears in that one call.
    std::vector<int> grid_y;
    std::vector<double> grid_theta;
    for (size_t i = 0; i < sizeof(z) / sizeof(z[0]); ++i) {
      const int y = (int)(i & 1u);
      grid_y.push_back(y);
      grid_theta.push_back((2.0 * y - 1.0) * z[i]);
    }
    check_bernoulli_run("bernoulli_logit vector cutoff grid",
                        OP_BERNOULLI_LOGIT_LPMF, 0, grid_y, grid_theta, false);

    // Odd length forces a packet remainder; irregular magnitudes make a
    // scalar-loop implementation diverge bitwise if it does not mirror the
    // pinned vector expression's exp and reduction paths.
    check_bernoulli_run("bernoulli_logit vector irregular",
                        OP_BERNOULLI_LOGIT_LPMF, 0,
                        {1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1},
                        {-37.25, 0.03125, 19.75, -0.875, 20.125, -19.875, 7.3,
                         -2.6, 0.0, 41.0, -13.125},
                        false);

    // A longer deterministic spread catches packet/scalar exp differences
    // away from the named cutoffs as well as reduction-order differences.
    std::vector<int> random_y;
    std::vector<double> random_theta;
    uint64_t state = 0x243f6a8885a308d3ULL;
    for (int i = 0; i < 257; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      random_y.push_back((int)(state & 1u));
      const double unit = static_cast<double>(state >> 11) * 0x1.0p-53;
      random_theta.push_back(80.0 * unit - 40.0);
    }
    check_bernoulli_run("bernoulli_logit vector deterministic spread",
                        OP_BERNOULLI_LOGIT_LPMF, 0, random_y, random_theta,
                        false);
  }

  // Invalid inputs must throw the same exception category and diagnostic as
  // Stan Math, including validation order. The summed kernel binds outcomes
  // as a vector, so the reference does too even for a single invalid lane.
  {
    auto check_bernoulli_exception =
        [&](const std::string& tag, uint16_t opcode, const std::vector<int>& y,
            double theta) {
          const CapturedException got = capture_exception(
              [&] { (void)run_bernoulli_variant(opcode, 0, y, {theta}); });
          const CapturedException want = capture_exception([&] {
            const Eigen::VectorXi y_ref = as_int_vector(y);
            if (opcode == OP_BERNOULLI_LPMF)
              (void)stan::math::bernoulli_lpmf<false>(y_ref, theta);
            else
              (void)stan::math::bernoulli_logit_lpmf<false>(y_ref, theta);
          });
          expect_exception_parity(tag, got, want);
        };
    check_bernoulli_exception("bernoulli invalid y", OP_BERNOULLI_LPMF,
                              {1, 2, 0}, 0.4);
    check_bernoulli_exception("bernoulli probability low", OP_BERNOULLI_LPMF,
                              {1, 0}, -0.1);
    check_bernoulli_exception("bernoulli probability high", OP_BERNOULLI_LPMF,
                              {1, 0}, 1.1);
    check_bernoulli_exception("bernoulli probability nan", OP_BERNOULLI_LPMF,
                              {1, 0}, std::numeric_limits<double>::quiet_NaN());
    check_bernoulli_exception("bernoulli_logit invalid y",
                              OP_BERNOULLI_LOGIT_LPMF, {1, -1, 0}, 0.3);
    check_bernoulli_exception("bernoulli_logit nan", OP_BERNOULLI_LOGIT_LPMF,
                              {1, 0}, std::numeric_limits<double>::quiet_NaN());

    // Empty Bernoulli validates theta before its size-zero return; logit does
    // the size-zero return first. Both orderings are observable and pinned.
    check_bernoulli_exception("bernoulli empty nan", OP_BERNOULLI_LPMF, {},
                              std::numeric_limits<double>::quiet_NaN());
    check_bernoulli_exception("bernoulli_logit empty nan",
                              OP_BERNOULLI_LOGIT_LPMF, {},
                              std::numeric_limits<double>::quiet_NaN());
  }

  // Empty calls return a literal constant, so an infinite downstream
  // adjoint must leave theta at +0 rather than form infinity * 0. Conversely,
  // a valid logit call at +infinity has a connected edge whose local partial
  // is -0, and therefore must produce NaN under the same upstream adjoint.
  {
    const double inf = std::numeric_limits<double>::infinity();
    for (uint16_t opcode : {OP_BERNOULLI_LPMF, OP_BERNOULLI_LOGIT_LPMF}) {
      const auto got = run_bernoulli_scaled(opcode, 0, {}, 0.4, inf);
      Eigen::VectorXi y_ref(0);
      var theta_ref = 0.4;
      var lp_ref =
          opcode == OP_BERNOULLI_LPMF
              ? stan::math::bernoulli_lpmf<false>(y_ref, theta_ref)
              : stan::math::bernoulli_logit_lpmf<false>(y_ref, theta_ref);
      var result_ref = lp_ref * inf;
      result_ref.grad();
      expect_nan(std::string(opcode == OP_BERNOULLI_LPMF ? "bernoulli"
                                                         : "bernoulli_logit") +
                     " empty scaled value",
                 got.value, result_ref.val());
      expect_bits(std::string(opcode == OP_BERNOULLI_LPMF ? "bernoulli"
                                                          : "bernoulli_logit") +
                      " empty scaled grad",
                  got.grad[0], theta_ref.adj());
      stan::math::recover_memory();
    }

    const auto got =
        run_bernoulli_scaled(OP_BERNOULLI_LOGIT_LPMF, 0, {1}, inf, inf);
    Eigen::VectorXi y_ref(1);
    y_ref[0] = 1;
    var theta_ref = inf;
    var lp_ref = stan::math::bernoulli_logit_lpmf<false>(y_ref, theta_ref);
    var result_ref = lp_ref * inf;
    result_ref.grad();
    expect_nan("bernoulli_logit connected value", got.value, result_ref.val());
    expect_nan("bernoulli_logit connected grad", got.grad[0], theta_ref.adj());
    stan::math::recover_memory();
  }

  check_bernoulli_scratch_reset("bernoulli scratch", OP_BERNOULLI_LPMF);
  check_bernoulli_scratch_reset("bernoulli_logit scratch",
                                OP_BERNOULLI_LOGIT_LPMF);

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
    // The elementwise binomials leave the recorder out entirely, so every
    // shape they branch on is pinned against the lanes: both distributions,
    // propto off and on, and a broadcast probability against a vector one.
    std::vector<int> fused_b{N, 3, 0, 7, 1, 2, -1, 20};
    const auto lane_b = [&](int64_t i) {
      return std::vector<int>{-1, counts[i], -1, 20};
    };
    for (uint8_t v : {0x01, 0x81}) {
      const std::string tag = v == 0x01 ? "" : " propto";
      check_elt("elt binomial" + tag, OP_BINOMIAL_LPMF, v, N, {unit}, {true},
                fused_b, lane_b);
      check_elt("elt binomial scalar" + tag, OP_BINOMIAL_LPMF, v, N, {{0.4}},
                {true}, fused_b, lane_b);
      check_elt("elt binomial_logit" + tag, OP_BINOMIAL_LOGIT_LPMF, v, N, {mus},
                {true}, fused_b, lane_b);
      check_elt("elt binomial_logit scalar" + tag, OP_BINOMIAL_LOGIT_LPMF, v, N,
                {{-0.3}}, {true}, fused_b, lane_b);
    }
    // n == 0 and n == N are separate branches of the value and the partial.
    std::vector<int> edge{0, 20, 0, 20, 10};
    check_elt("elt binomial edges", OP_BINOMIAL_LPMF, 0x01, N, {unit}, {true},
              {N, 0, 20, 0, 20, 10, -1, 20},
              [&](int64_t i) { return std::vector<int>{-1, edge[i], -1, 20}; });
  }

  // The two-integer-group cdfs. binomial and beta_binomial carry an
  // outcome group and a trials group, so their idata is the same
  // [len, vals...] pair their lpmfs use, and a len of -1 marks a
  // language-level scalar that stan-math broadcasts. Every combination of
  // the three group forms -- vector, scalar, and the array of one that is
  // a container rather than a scalar -- reaches a different stan-math
  // overload, which is why each one is pinned rather than just the vector
  // case.
  {
    using stan::math::var;
    using VecV = Eigen::Matrix<var, -1, 1>;
    const std::vector<double> th3{0.2, 0.5, 0.75};
    const std::vector<double> a3{2.0, 3.0, 4.0};
    const std::vector<double> b3{1.3, 1.4, 1.5};
    auto vec = [](const std::vector<double>& v) {
      VecV x((Eigen::Index)v.size());
      for (size_t i = 0; i < v.size(); ++i) x((Eigen::Index)i) = v[i];
      return x;
    };
    auto ivec = [](std::vector<int> v) {
      Eigen::VectorXi x((Eigen::Index)v.size());
      for (size_t i = 0; i < v.size(); ++i) x((Eigen::Index)i) = v[i];
      return x;
    };
    // Both groups vectors.
    {
      auto r = testutil::run_one_op(OP_BINOMIAL_LCDF, {th3}, {true},
                                    {3, 1, 2, 3, 3, 5, 7, 9});
      VecV theta = vec(th3);
      var lp =
          stan::math::binomial_lcdf(ivec({1, 2, 3}), ivec({5, 7, 9}), theta);
      lp.grad();
      expect_eq("binomial_lcdf vv value", r.value, lp.val());
      for (int i = 0; i < 3; ++i)
        expect_eq("binomial_lcdf vv d" + std::to_string(i), r.grad[(size_t)i],
                  theta(i).adj());
      stan::math::recover_memory();
    }
    // Scalar outcome against a vector of trials.
    {
      auto r = testutil::run_one_op(OP_BINOMIAL_LCDF, {th3}, {true},
                                    {-1, 3, 3, 5, 7, 9});
      VecV theta = vec(th3);
      var lp = stan::math::binomial_lcdf(3, ivec({5, 7, 9}), theta);
      lp.grad();
      expect_eq("binomial_lcdf sv value", r.value, lp.val());
      for (int i = 0; i < 3; ++i)
        expect_eq("binomial_lcdf sv d" + std::to_string(i), r.grad[(size_t)i],
                  theta(i).adj());
      stan::math::recover_memory();
    }
    // Both groups scalars, broadcast across the real argument's lanes.
    {
      auto r =
          testutil::run_one_op(OP_BINOMIAL_LCDF, {th3}, {true}, {-1, 3, -1, 9});
      VecV theta = vec(th3);
      var lp = stan::math::binomial_lcdf(3, 9, theta);
      lp.grad();
      expect_eq("binomial_lcdf ss value", r.value, lp.val());
      for (int i = 0; i < 3; ++i)
        expect_eq("binomial_lcdf ss d" + std::to_string(i), r.grad[(size_t)i],
                  theta(i).adj());
      stan::math::recover_memory();
    }
    // An array of one is a container: it must not be confused with the
    // scalar form, which is what the -1 length exists to distinguish.
    {
      auto r = testutil::run_one_op(OP_BINOMIAL_LCCDF, {{0.4}}, {true},
                                    {1, 2, 1, 7});
      VecV theta = vec({0.4});
      var lp = stan::math::binomial_lccdf(ivec({2}), ivec({7}), theta);
      lp.grad();
      expect_eq("binomial_lccdf 11 value", r.value, lp.val());
      expect_eq("binomial_lccdf 11 d0", r.grad[0], theta(0).adj());
      stan::math::recover_memory();
    }
    // beta_binomial: the same two groups, then two real arguments.
    {
      auto r = testutil::run_one_op(OP_BETA_BINOMIAL_CDF, {a3, b3},
                                    {true, true}, {3, 1, 2, 3, 3, 5, 7, 9});
      VecV alpha = vec(a3), beta = vec(b3);
      var lp = stan::math::beta_binomial_cdf(ivec({1, 2, 3}), ivec({5, 7, 9}),
                                             alpha, beta);
      lp.grad();
      expect_eq("beta_binomial_cdf vv value", r.value, lp.val());
      for (int i = 0; i < 3; ++i)
        expect_eq("beta_binomial_cdf vv da" + std::to_string(i),
                  r.grad[(size_t)i], alpha(i).adj());
      for (int i = 0; i < 3; ++i)
        expect_eq("beta_binomial_cdf vv db" + std::to_string(i),
                  r.grad[(size_t)(3 + i)], beta(i).adj());
      stan::math::recover_memory();
    }
    {
      auto r = testutil::run_one_op(OP_BETA_BINOMIAL_LCCDF, {a3, b3},
                                    {true, true}, {-1, 3, -1, 9});
      VecV alpha = vec(a3), beta = vec(b3);
      var lp = stan::math::beta_binomial_lccdf(3, 9, alpha, beta);
      lp.grad();
      expect_eq("beta_binomial_lccdf ss value", r.value, lp.val());
      for (int i = 0; i < 3; ++i)
        expect_eq("beta_binomial_lccdf ss da" + std::to_string(i),
                  r.grad[(size_t)i], alpha(i).adj());
      for (int i = 0; i < 3; ++i)
        expect_eq("beta_binomial_lccdf ss db" + std::to_string(i),
                  r.grad[(size_t)(3 + i)], beta(i).adj());
      stan::math::recover_memory();
    }
  }

  // wiener: every argument vectorizes in the language, and a length-1 slot
  // must enter stan-math as a scalar (vectors do not broadcast there).
  {
    using stan::math::var;
    using VecV = Eigen::Matrix<var, -1, 1>;
    std::vector<double> wy{1.45, 1.54}, wa{1.15, 1.24}, wt{0.15, 0.24},
        wb{0.40, 0.49}, wd{0.05, 0.14};
    auto vec = [](const std::vector<double>& v) {
      VecV x(v.size());
      for (size_t i = 0; i < v.size(); ++i) x(i) = v[i];
      return x;
    };
    auto grads = [&](const std::string& tag,
                     const std::vector<std::vector<double>>& vals,
                     const std::vector<var*>& scalars,
                     const std::vector<VecV*>& vectors) {
      // Slot order: any vector slot's elements then the next slot's, matching
      // run_op_sum's parameter concatenation.
      auto r = stanli::testutil::run_op_sum(OP_WIENER_LPDF, 1, vals,
                                            std::vector<bool>(5, true));
      size_t gi = 0;
      for (size_t k = 0; k < 5; ++k) {
        if (vectors[k]) {
          for (int i = 0; i < vectors[k]->size(); ++i)
            expect_eq(tag + " g" + std::to_string(gi), r.grad[gi],
                      (*vectors[k])(i).adj()),
                ++gi;
        } else {
          expect_eq(tag + " g" + std::to_string(gi), r.grad[gi],
                    scalars[k]->adj()),
              ++gi;
        }
      }
      return r;
    };
    {
      VecV y = vec(wy), a = vec(wa), t = vec(wt), b = vec(wb), d = vec(wd);
      var lp = stan::math::wiener_lpdf<false>(y, a, t, b, d);
      lp.grad();
      auto r = grads("wiener all-vector", {wy, wa, wt, wb, wd},
                     {nullptr, nullptr, nullptr, nullptr, nullptr},
                     {&y, &a, &t, &b, &d});
      expect_eq("wiener all-vector lp", r.value, lp.val());
      stan::math::recover_memory();
    }
    {
      VecV y = vec(wy);
      var a = wa[0], t = wt[0], b = wb[0], d = wd[0];
      var lp = stan::math::wiener_lpdf<false>(y, a, t, b, d);
      lp.grad();
      auto r = grads(
          "wiener y-vector", {wy, {wa[0]}, {wt[0]}, {wb[0]}, {wd[0]}},
          {nullptr, &a, &t, &b, &d}, {&y, nullptr, nullptr, nullptr, nullptr});
      expect_eq("wiener y-vector lp", r.value, lp.val());
      stan::math::recover_memory();
    }
    {
      var y = wy[0], a = wa[0], t = wt[0], b = wb[0], d = wd[0];
      var lp = stan::math::wiener_lpdf<false>(y, a, t, b, d);
      lp.grad();
      auto r = grads(
          "wiener scalar", {{wy[0]}, {wa[0]}, {wt[0]}, {wb[0]}, {wd[0]}},
          {&y, &a, &t, &b, &d}, {nullptr, nullptr, nullptr, nullptr, nullptr});
      expect_eq("wiener scalar lp", r.value, lp.val());
      stan::math::recover_memory();
    }
  }

  // The var-tape cdfs: von_mises_{cdf,lcdf,lccdf} and
  // neg_binomial_2_{lcdf,lccdf}. The recorder cannot evaluate these at all
  // -- they do arithmetic on the autodiff scalar rather than going through
  // stan-math's partials propagator -- so the kernel binds every argument
  // as var on a nested tape (matrix_fns.cpp). The reference below binds
  // every argument as var too, so this is same-activity and bitwise: no
  // tolerance, and no evaluation-order divergence to bound.
  {
    using stan::math::var;
    using VecV = Eigen::Matrix<var, -1, 1>;
    auto vec = [](const std::vector<double>& v) {
      VecV x((Eigen::Index)v.size());
      for (size_t i = 0; i < v.size(); ++i) x((Eigen::Index)i) = v[i];
      return x;
    };
    auto ivec = [](std::vector<int> v) {
      Eigen::VectorXi x((Eigen::Index)v.size());
      for (size_t i = 0; i < v.size(); ++i) x((Eigen::Index)i) = v[i];
      return x;
    };
    const std::vector<double> vy{0.30, -0.50}, vmu{0.10, 0.20}, vk{1.50, 2.50};
    // Every argument a vector, then a length-1 slot beside them: that slot
    // has to reach stan-math as a scalar, because its sequence views
    // broadcast a scalar but require a vector to match the others' size.
    struct VonMises {
      uint16_t opcode;
      const char* tag;
      std::function<var(const VecV&, const VecV&, const VecV&)> vvv;
      std::function<var(const VecV&, const var&, const var&)> vss;
      std::function<var(const var&, const var&, const var&)> sss;
    };
    const VonMises kVm[3] = {{OP_VON_MISES_CDF, "von_mises_cdf",
                              [](const VecV& y, const VecV& m, const VecV& k) {
                                return stan::math::von_mises_cdf(y, m, k);
                              },
                              [](const VecV& y, const var& m, const var& k) {
                                return stan::math::von_mises_cdf(y, m, k);
                              },
                              [](const var& y, const var& m, const var& k) {
                                return stan::math::von_mises_cdf(y, m, k);
                              }},
                             {OP_VON_MISES_LCDF, "von_mises_lcdf",
                              [](const VecV& y, const VecV& m, const VecV& k) {
                                return stan::math::von_mises_lcdf(y, m, k);
                              },
                              [](const VecV& y, const var& m, const var& k) {
                                return stan::math::von_mises_lcdf(y, m, k);
                              },
                              [](const var& y, const var& m, const var& k) {
                                return stan::math::von_mises_lcdf(y, m, k);
                              }},
                             {OP_VON_MISES_LCCDF, "von_mises_lccdf",
                              [](const VecV& y, const VecV& m, const VecV& k) {
                                return stan::math::von_mises_lccdf(y, m, k);
                              },
                              [](const VecV& y, const var& m, const var& k) {
                                return stan::math::von_mises_lccdf(y, m, k);
                              },
                              [](const var& y, const var& m, const var& k) {
                                return stan::math::von_mises_lccdf(y, m, k);
                              }}};
    for (const VonMises& c : kVm) {
      {
        auto r = stanli::testutil::run_one_op(c.opcode, {vy, vmu, vk},
                                              {true, true, true});
        VecV y = vec(vy), m = vec(vmu), k = vec(vk);
        var lp = c.vvv(y, m, k);
        lp.grad();
        expect_eq(std::string(c.tag) + " vvv value", r.value, lp.val());
        for (int i = 0; i < 2; ++i) {
          expect_eq(std::string(c.tag) + " vvv dy" + std::to_string(i),
                    r.grad[(size_t)i], y(i).adj());
          expect_eq(std::string(c.tag) + " vvv dmu" + std::to_string(i),
                    r.grad[(size_t)(2 + i)], m(i).adj());
          expect_eq(std::string(c.tag) + " vvv dk" + std::to_string(i),
                    r.grad[(size_t)(4 + i)], k(i).adj());
        }
        stan::math::recover_memory();
      }
      {
        auto r = stanli::testutil::run_one_op(c.opcode, {vy, {vmu[0]}, {vk[0]}},
                                              {true, true, true});
        VecV y = vec(vy);
        var m = vmu[0], k = vk[0];
        var lp = c.vss(y, m, k);
        lp.grad();
        expect_eq(std::string(c.tag) + " vss value", r.value, lp.val());
        for (int i = 0; i < 2; ++i)
          expect_eq(std::string(c.tag) + " vss dy" + std::to_string(i),
                    r.grad[(size_t)i], y(i).adj());
        expect_eq(std::string(c.tag) + " vss dmu", r.grad[2], m.adj());
        expect_eq(std::string(c.tag) + " vss dk", r.grad[3], k.adj());
        stan::math::recover_memory();
      }
      {
        auto r = stanli::testutil::run_one_op(
            c.opcode, {{vy[0]}, {vmu[0]}, {vk[0]}}, {true, true, true});
        var y = vy[0], m = vmu[0], k = vk[0];
        var lp = c.sss(y, m, k);
        lp.grad();
        expect_eq(std::string(c.tag) + " sss value", r.value, lp.val());
        expect_eq(std::string(c.tag) + " sss dy", r.grad[0], y.adj());
        expect_eq(std::string(c.tag) + " sss dmu", r.grad[1], m.adj());
        expect_eq(std::string(c.tag) + " sss dk", r.grad[2], k.adj());
        stan::math::recover_memory();
      }
    }
    // neg_binomial_2: the outcome rides in idata, so the real arguments are
    // the only propagator edges. Both a per-lane outcome and the scalar
    // form the lowering replicates to the lane count.
    const std::vector<double> vmn{2.00, 3.00}, vphi{1.50, 2.50};
    struct Nb2 {
      uint16_t opcode;
      const char* tag;
      std::function<var(const Eigen::VectorXi&, const VecV&, const VecV&)> vv;
      std::function<var(const Eigen::VectorXi&, const var&, const var&)> ss;
    };
    const Nb2 kNb[2] = {
        {OP_NEG_BINOMIAL_2_LCDF, "neg_binomial_2_lcdf",
         [](const Eigen::VectorXi& n, const VecV& m, const VecV& p) {
           return stan::math::neg_binomial_2_lcdf(n, m, p);
         },
         [](const Eigen::VectorXi& n, const var& m, const var& p) {
           return stan::math::neg_binomial_2_lcdf(n, m, p);
         }},
        {OP_NEG_BINOMIAL_2_LCCDF, "neg_binomial_2_lccdf",
         [](const Eigen::VectorXi& n, const VecV& m, const VecV& p) {
           return stan::math::neg_binomial_2_lccdf(n, m, p);
         },
         [](const Eigen::VectorXi& n, const var& m, const var& p) {
           return stan::math::neg_binomial_2_lccdf(n, m, p);
         }}};
    for (const Nb2& c : kNb) {
      {
        auto r = stanli::testutil::run_one_op(c.opcode, {vmn, vphi},
                                              {true, true}, {1, 3});
        VecV m = vec(vmn), p = vec(vphi);
        var lp = c.vv(ivec({1, 3}), m, p);
        lp.grad();
        expect_eq(std::string(c.tag) + " vv value", r.value, lp.val());
        for (int i = 0; i < 2; ++i) {
          expect_eq(std::string(c.tag) + " vv dmu" + std::to_string(i),
                    r.grad[(size_t)i], m(i).adj());
          expect_eq(std::string(c.tag) + " vv dphi" + std::to_string(i),
                    r.grad[(size_t)(2 + i)], p(i).adj());
        }
        stan::math::recover_memory();
      }
      {
        auto r = stanli::testutil::run_one_op(c.opcode, {{vmn[0]}, {vphi[0]}},
                                              {true, true}, {2});
        var m = vmn[0], p = vphi[0];
        var lp = c.ss(ivec({2}), m, p);
        lp.grad();
        expect_eq(std::string(c.tag) + " ss value", r.value, lp.val());
        expect_eq(std::string(c.tag) + " ss dmu", r.grad[0], m.adj());
        expect_eq(std::string(c.tag) + " ss dphi", r.grad[1], p.adj());
        stan::math::recover_memory();
      }
    }
  }

  if (failures == 0) std::printf("test_densities OK\n");
  return failures == 0 ? 0 : 1;
}
