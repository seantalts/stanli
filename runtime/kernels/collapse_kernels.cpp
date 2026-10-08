// Kernels the fast-mode observation collapse emits (runtime/src/collapse.cpp).
#include <stanli/collapse.hpp>
#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>

#include "../src/collapse_linear.hpp"

#include <stan/math/prim/err.hpp>
#include <stan/math/prim/fun/binomial_coefficient_log.hpp>
#include <stan/math/prim/fun/constants.hpp>
#include <stan/math/prim/fun/digamma.hpp>
#include <stan/math/prim/fun/inv_logit.hpp>
#include <stan/math/prim/fun/lgamma.hpp>
#include <stan/math/prim/fun/log1m.hpp>
#include <stan/math/prim/fun/log1m_inv_logit.hpp>
#include <stan/math/prim/fun/log_inv_logit.hpp>

#include <algorithm>
#include <cmath>

namespace stanli {
namespace {

// Stan's own argument checks, so a rejected point is rejected the same way.
// A length-one argument is checked as a scalar and reported without an index.
template <typename Check>
void check_arg(const Desc& arg, Check&& check) {
  if (arg.len == 1)
    check(arg.data[0]);
  else
    check(Eigen::Map<const Eigen::ArrayXd>(arg.data, arg.len));
}

// See collapse.hpp for the layout. Partials land in scratch: the location's
// first, then the scale's, each one per group or summed for a shared value.
void normal_grouped_fwd(KernelCtx& ctx) {
  const int64_t groups = ctx.in[0].len / kGroupedStatColumns;
  const double* count = ctx.in[0].data;
  const double* centre = count + groups;
  const double* offset = centre + groups;
  const double* squares = offset + groups;
  const Desc& mu = ctx.in[1];
  const Desc& sigma = ctx.in[2];
  const bool lognormal = (ctx.variant & kGroupedLognormal) != 0;
  const bool propto = (ctx.variant & kGroupedPropto) != 0;
  const bool scale_active = (ctx.variant & kGroupedScaleActive) != 0;

  const char* function = lognormal ? "lognormal_lpdf" : "normal_lpdf";
  check_arg(mu, [&](const auto& v) {
    stan::math::check_finite(function, "Location parameter", v);
  });
  check_arg(sigma, [&](const auto& v) {
    if (lognormal)
      stan::math::check_positive_finite(function, "Scale parameter", v);
    else
      stan::math::check_positive(function, "Scale parameter", v);
  });

  double* d_mu = ctx.scratch;
  double* d_sigma = ctx.scratch + mu.len;
  std::fill_n(ctx.scratch, mu.len + sigma.len, 0.0);
  double quadratic = 0, log_scale = 0, total = 0;
  for (int64_t g = 0; g < groups; ++g) {
    const double s = sigma.data[sigma.len == 1 ? 0 : g];
    const double inv = 1.0 / s, inv2 = inv * inv;
    const double r = centre[g] - mu.data[mu.len == 1 ? 0 : g];
    const double q = squares[g] + 2.0 * r * offset[g] + count[g] * r * r;
    quadratic += q * inv2;
    total += count[g];
    if (sigma.len != 1) log_scale += count[g] * std::log(s);
    d_mu[mu.len == 1 ? 0 : g] += (count[g] * r + offset[g]) * inv2;
    d_sigma[sigma.len == 1 ? 0 : g] += q * inv2 * inv - count[g] * inv;
  }
  if (sigma.len == 1) log_scale = total * std::log(sigma.data[0]);

  double lp = -0.5 * quadratic;
  if (!propto || scale_active) lp -= log_scale;
  // Stan's lognormal keeps this constant under propto; its normal does not.
  if (!propto || lognormal) lp += total * stan::math::NEG_LOG_SQRT_TWO_PI;
  ctx.out.data[0] = lp + ctx.in[3].data[0];
}

void normal_grouped_bwd(KernelCtx& ctx) {
  const int64_t n_mu = ctx.in[1].len, n_sigma = ctx.in[2].len;
  if (ctx.in_adj[1].data != nullptr)
    for (int64_t k = 0; k < n_mu; ++k)
      ctx.in_adj[1].data[k] += ctx.out_adj * ctx.scratch[k];
  if (ctx.in_adj[2].data != nullptr)
    for (int64_t k = 0; k < n_sigma; ++k)
      ctx.in_adj[2].data[k] += ctx.out_adj * ctx.scratch[n_mu + k];
}

int64_t normal_grouped_scratch(const Op& op, const Slot* slots) {
  return slots[op.in[1]].len + slots[op.in[2]].len;
}

// See collapse.hpp for the layout. Scratch holds theta's partials, then the
// scale's, then u and R u as work space.
void linear_gaussian_fwd(KernelCtx& ctx) {
  const Desc& theta = ctx.in[1];
  const Desc& sigma = ctx.in[2];
  const int64_t p = theta.len;
  const double* data = ctx.in[0].data;
  const double base = data[0], total = data[1];
  const double* centre = data + kLinearGaussianHeader;
  const double* linear = centre + p;
  const double* r = linear + p;
  const bool lognormal = (ctx.variant & kGroupedLognormal) != 0;
  const bool propto = (ctx.variant & kGroupedPropto) != 0;
  const bool scale_active = (ctx.variant & kGroupedScaleActive) != 0;

  // The locations themselves are never formed. They are finite when theta
  // is, short of overflow, which then shows as a non-finite result.
  if ((ctx.variant & kGroupedGlm) != 0) {
    // normal_id_glm checks its scale first. theta holds the intercept with
    // the weights, so a bad intercept is reported as a weight.
    const char* function = "normal_id_glm_lpdf";
    stan::math::check_positive_finite(function, "Scale vector", sigma.data[0]);
    check_arg(theta, [&](const auto& v) {
      stan::math::check_finite(function, "Weight vector", v);
    });
  } else {
    const char* function = lognormal ? "lognormal_lpdf" : "normal_lpdf";
    check_arg(theta, [&](const auto& v) {
      stan::math::check_finite(function, "Location parameter", v);
    });
    if (lognormal)
      stan::math::check_positive_finite(function, "Scale parameter",
                                        sigma.data[0]);
    else
      stan::math::check_positive(function, "Scale parameter", sigma.data[0]);
  }

  double* d_theta = ctx.scratch;
  double* d_sigma = ctx.scratch + p;
  double* u = d_sigma + 1;
  double* ru = u + p;
  double q = base;
  for (int64_t j = 0; j < p; ++j) {
    u[j] = theta.data[j] - centre[j];
    q -= 2.0 * u[j] * linear[j];
    d_theta[j] = 0.0;
  }
  // One pass over R: row i gives (R u)[i]; its entries then carry that
  // value into R' (R u).
  const double* row = r;
  for (int64_t i = 0; i < p; ++i) {
    double acc = 0;
    for (int64_t j = i; j < p; ++j) acc += row[j - i] * u[j];
    ru[i] = acc;
    q += acc * acc;
    for (int64_t j = i; j < p; ++j) d_theta[j] += row[j - i] * acc;
    row += p - i;
  }
  const double s = sigma.data[0];
  const double inv = 1.0 / s, inv2 = inv * inv;
  for (int64_t j = 0; j < p; ++j) d_theta[j] = (linear[j] - d_theta[j]) * inv2;
  *d_sigma = q * inv2 * inv - total * inv;

  double lp = -0.5 * q * inv2;
  if (!propto || scale_active) lp -= total * std::log(s);
  // Stan's lognormal keeps this constant under propto; its normal does not.
  if (!propto || lognormal) lp += total * stan::math::NEG_LOG_SQRT_TWO_PI;
  ctx.out.data[0] = lp + ctx.in[3].data[0];
}

void linear_gaussian_bwd(KernelCtx& ctx) {
  const int64_t p = ctx.in[1].len;
  if (ctx.in_adj[1].data != nullptr)
    for (int64_t j = 0; j < p; ++j)
      ctx.in_adj[1].data[j] += ctx.out_adj * ctx.scratch[j];
  if (ctx.in_adj[2].data != nullptr)
    ctx.in_adj[2].data[0] += ctx.out_adj * ctx.scratch[p];
}

int64_t linear_gaussian_scratch(const Op& op, const Slot* slots) {
  return 3 * slots[op.in[1]].len + 1;
}

// Stan's argument checks for a family, under the density's own name.
const char* glm_name(int family) {
  return family == kFamilyBernoulliLogit ? "bernoulli_logit_glm_lpmf"
         : family == kFamilyPoissonLog   ? "poisson_log_glm_lpmf"
                                         : "binomial_logit_glm_lpmf";
}

// A GLM checks its weights, intercept and linear predictor for finiteness
// only once its result is not finite. Here only the predictor is in hand,
// so a bad weight is reported as a bad predictor.
void check_glm_predictor(int family, const Desc& a) {
  check_arg(a, [&](const auto& v) {
    stan::math::check_finite(glm_name(family),
                             "Matrix of independent variables", v);
  });
}

void check_family(int family, const Desc& a, const Desc* b, bool glm) {
  using namespace stan::math;
  if (glm) {
    // The Poisson and binomial GLMs are not finite wherever the predictor
    // is not; the Bernoulli one is checked on its value, below.
    if (family != kFamilyBernoulliLogit) check_glm_predictor(family, a);
    return;
  }
  const auto positive_finite = [](const char* f, const char* what,
                                  const Desc& d) {
    check_arg(d, [&](const auto& v) { check_positive_finite(f, what, v); });
  };
  switch (family) {
    case kFamilyExponential:
      positive_finite("exponential_lpdf", "Inverse scale parameter", a);
      break;
    case kFamilyGamma:
      positive_finite("gamma_lpdf", "Shape parameter", a);
      positive_finite("gamma_lpdf", "Inverse scale parameter", *b);
      break;
    case kFamilyInvGamma:
      positive_finite("inv_gamma_lpdf", "Shape parameter", a);
      positive_finite("inv_gamma_lpdf", "Scale parameter", *b);
      break;
    case kFamilyBeta:
      positive_finite("beta_lpdf", "First shape parameter", a);
      positive_finite("beta_lpdf", "Second shape parameter", *b);
      break;
    case kFamilyPoisson:
      check_arg(a, [](const auto& v) {
        check_nonnegative("poisson_lpmf", "Rate parameter", v);
      });
      break;
    case kFamilyPoissonLog:
      check_arg(a, [](const auto& v) {
        check_not_nan("poisson_log_lpmf", "Log rate parameter", v);
      });
      break;
    case kFamilyBernoulli:
      check_arg(a, [](const auto& v) {
        check_bounded("bernoulli_lpmf", "Probability parameter", v, 0.0, 1.0);
      });
      break;
    case kFamilyBernoulliLogit:
      check_arg(a, [](const auto& v) {
        check_not_nan("bernoulli_logit_lpmf",
                      "Logit transformed probability parameter", v);
      });
      break;
    case kFamilyBinomial:
      check_arg(a, [](const auto& v) {
        check_bounded("binomial_lpmf", "Probability parameter", v, 0.0, 1.0);
      });
      break;
    default:  // kFamilyBinomialLogit
      check_arg(a, [](const auto& v) {
        check_finite("binomial_logit_lpmf", "Probability parameter", v);
      });
      break;
  }
}

// Bernoulli-logit at theta for both outcomes at once, from one exp and one
// log1p: the log probabilities of a success and of a failure, and the
// partials of each. Stan's cutoffs at |theta| = 20 are kept; inside them
// log1p(exp(theta)) is taken as log1p(exp(-theta)) + theta, which is the
// same number up to rounding.
struct Logit {
  double success, failure, d_success, d_failure;
};
Logit logit_at(double theta) {
  const double e = std::exp(-theta);  // also carries a NaN through
  if (theta > 20.0) return {-e, -theta, e, -1.0};
  if (theta < -20.0) {
    const double r = std::exp(theta);
    return {theta, -r, 1.0, -r};
  }
  const double l = std::log1p(e);
  const double p = 1.0 / (1.0 + e);
  return {-l, -(l + theta), e * p, -p};
}

// See collapse.hpp for the layout. Each family's value is Stan's density
// summed over a group, with the group's observations replaced by their
// sums; a term is kept under propto exactly when Stan keeps it. Partials
// land in scratch, the first parameter's and then the second's.
void family_grouped_fwd(KernelCtx& ctx) {
  using stan::math::digamma;
  using stan::math::lgamma;
  const int family = ctx.idata[0];
  const int64_t groups = ctx.in[0].len / kFamilyStatColumns;
  const double* count = ctx.in[0].data;
  const double* t1 = count + groups;
  const double* t2 = t1 + groups;
  const Desc& a = ctx.in[2];
  const Desc* b = ctx.n_in > 3 ? &ctx.in[3] : nullptr;
  const bool propto = (ctx.variant & kFamilyPropto) != 0;
  const bool keep_a = !propto || (ctx.variant & kFamilyFirstActive) != 0;
  const bool keep_b = !propto || (ctx.variant & kFamilySecondActive) != 0;
  const bool glm = (ctx.variant & kFamilyGlm) != 0;
  check_family(family, a, b, glm);

  const int64_t n_a = a.len, n_b = b ? b->len : 0;
  double* d_a = ctx.scratch;
  double* d_b = ctx.scratch + n_a;
  std::fill_n(ctx.scratch, n_a + n_b, 0.0);
  double lp = 0;
  bool impossible = false;  // Stan's LOG_ZERO: the whole density is zero
  for (int64_t g = 0; g < groups && !impossible; ++g) {
    const double n = count[g], s1 = t1[g], s2 = t2[g];
    const double x = a.data[n_a == 1 ? 0 : g];
    const double y = b ? b->data[n_b == 1 ? 0 : g] : 0.0;
    double da = 0, db = 0;
    switch (family) {
      case kFamilyExponential:
        lp += n * std::log(x) - x * s1;
        da = n / x - s1;
        break;
      case kFamilyGamma: {
        const double log_beta = std::log(y);
        if (keep_a) lp += -n * lgamma(x) + (x - 1.0) * s1;
        if (keep_a || keep_b) lp += n * x * log_beta;
        if (keep_b) lp -= y * s2;
        da = n * log_beta + s1 - n * digamma(x);
        db = n * x / y - s2;
        break;
      }
      case kFamilyInvGamma: {
        const double log_beta = std::log(y);
        if (keep_a) lp += -n * lgamma(x) - (x + 1.0) * s1;
        if (keep_a || keep_b) lp += n * x * log_beta;
        if (keep_b) lp -= y * s2;
        da = n * log_beta - n * digamma(x) - s1;
        db = n * x / y - s2;
        break;
      }
      case kFamilyBeta: {
        if (keep_a) lp += -n * lgamma(x) + (x - 1.0) * s1;
        if (keep_b) lp += -n * lgamma(y) + (y - 1.0) * s2;
        lp += n * lgamma(x + y);
        const double both = n * digamma(x + y);
        da = s1 + both - n * digamma(x);
        db = s2 + both - n * digamma(y);
        break;
      }
      case kFamilyPoisson:
        if (std::isinf(x) || (x == 0 && s1 != 0)) {
          impossible = true;
          break;
        }
        if (s1 != 0) lp += s1 * std::log(x);
        lp -= n * x;
        da = (s1 != 0 ? s1 / x : 0.0) - n;
        break;
      case kFamilyPoissonLog: {
        if (x == stan::math::INFTY ||
            (x == stan::math::NEGATIVE_INFTY && s1 != 0)) {
          impossible = true;
          break;
        }
        const double rate = std::exp(x);
        if (s1 != 0) lp += s1 * x;
        lp -= n * rate;
        da = s1 - n * rate;
        break;
      }
      case kFamilyBernoulli:
      case kFamilyBinomial:
        // s1 successes, s2 failures; a count of zero contributes nothing,
        // also when its log is infinite.
        if (s1 != 0) {
          lp += s1 * std::log(x);
          da += s1 / x;
        }
        if (s2 != 0) {
          lp += s2 * stan::math::log1m(x);
          da += s2 / (x - 1.0);
        }
        break;
      case kFamilyBernoulliLogit:
      default: {  // and kFamilyBinomialLogit
        // A count of zero contributes nothing, also at an infinite theta.
        const Logit at = logit_at(x);
        if (s1 != 0) {
          lp += s1 * at.success;
          da += s1 * at.d_success;
        }
        if (s2 != 0) {
          lp += s2 * at.failure;
          da += s2 * at.d_failure;
        }
        break;
      }
    }
    d_a[n_a == 1 ? 0 : g] += da;
    if (b) d_b[n_b == 1 ? 0 : g] += db;
  }
  if (glm && !std::isfinite(lp)) check_glm_predictor(family, a);
  if (impossible) {
    std::fill_n(ctx.scratch, n_a + n_b, 0.0);
    ctx.out.data[0] = stan::math::LOG_ZERO;
    return;
  }
  ctx.out.data[0] = lp + ctx.in[1].data[0];
}

void family_grouped_bwd(KernelCtx& ctx) {
  const int64_t n_a = ctx.in[2].len;
  if (ctx.in_adj[2].data != nullptr)
    for (int64_t k = 0; k < n_a; ++k)
      ctx.in_adj[2].data[k] += ctx.out_adj * ctx.scratch[k];
  if (ctx.n_in > 3 && ctx.in_adj[3].data != nullptr)
    for (int64_t k = 0; k < ctx.in[3].len; ++k)
      ctx.in_adj[3].data[k] += ctx.out_adj * ctx.scratch[n_a + k];
}

int64_t family_grouped_scratch(const Op& op, const Slot* slots) {
  return slots[op.in[2]].len + (op.n_in > 3 ? slots[op.in[3]].len : 0);
}

}  // namespace

int family_of_opcode(uint16_t opcode) {
  switch (opcode) {
    case OP_EXPONENTIAL_LPDF:
      return kFamilyExponential;
    case OP_GAMMA_LPDF:
      return kFamilyGamma;
    case OP_INV_GAMMA_LPDF:
      return kFamilyInvGamma;
    case OP_BETA_LPDF:
      return kFamilyBeta;
    case OP_POISSON_LPMF:
      return kFamilyPoisson;
    case OP_POISSON_LOG_LPMF:
      return kFamilyPoissonLog;
    case OP_BERNOULLI_LPMF:
      return kFamilyBernoulli;
    case OP_BERNOULLI_LOGIT_LPMF:
      return kFamilyBernoulliLogit;
    case OP_BINOMIAL_LPMF:
      return kFamilyBinomial;
    case OP_BINOMIAL_LOGIT_LPMF:
      return kFamilyBinomialLogit;
    default:
      return -1;
  }
}

int family_parameters(int family) {
  return family == kFamilyGamma || family == kFamilyInvGamma ||
                 family == kFamilyBeta
             ? 2
             : 1;
}

bool family_observation(int family, double y, double second, double t[3]) {
  t[0] = t[1] = t[2] = 0;
  if (!std::isfinite(y)) return false;
  switch (family) {
    case kFamilyExponential:
      if (y < 0) return false;
      t[0] = y;
      return true;
    case kFamilyGamma:
      if (y <= 0) return false;
      t[0] = std::log(y);
      t[1] = y;
      return true;
    case kFamilyInvGamma:
      if (y <= 0) return false;
      t[0] = std::log(y);
      t[1] = 1.0 / y;
      return true;
    case kFamilyBeta:
      if (y <= 0 || y >= 1) return false;
      t[0] = std::log(y);
      t[1] = stan::math::log1m(y);
      return true;
    case kFamilyPoisson:
    case kFamilyPoissonLog:
      if (y < 0) return false;
      t[0] = y;
      t[2] = -stan::math::lgamma(y + 1.0);
      return true;
    case kFamilyBernoulli:
    case kFamilyBernoulliLogit:
      if (y != 0 && y != 1) return false;
      t[0] = y;
      t[1] = 1.0 - y;
      return true;
    default:  // the binomials: y successes of `second` trials
      if (y < 0 || second < y || !std::isfinite(second)) return false;
      t[0] = y;
      t[1] = second - y;
      t[2] = stan::math::binomial_coefficient_log(second, y);
      return true;
  }
}

void register_collapse_kernels() {
  Kernel grouped{normal_grouped_fwd, normal_grouped_bwd,
                 normal_grouped_scratch};
  grouped.primal_reads = backward_reads_none;
  grouped.derivative_mechanism = "closed form";
  register_kernel(OP_NORMAL_GROUPED_LPDF, grouped);
  Kernel linear{linear_gaussian_fwd, linear_gaussian_bwd,
                linear_gaussian_scratch};
  linear.primal_reads = backward_reads_none;
  linear.derivative_mechanism = "closed form";
  register_kernel(OP_LINEAR_GAUSSIAN_LPDF, linear);
  Kernel family{family_grouped_fwd, family_grouped_bwd, family_grouped_scratch};
  family.primal_reads = backward_reads_none;
  family.derivative_mechanism = "closed form";
  register_kernel(OP_FAMILY_GROUPED_LPDF, family);
}

}  // namespace stanli
