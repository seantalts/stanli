// Kernels the fast-mode observation collapse emits (runtime/src/collapse.cpp).
#include <stanli/collapse.hpp>
#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>

#include <stan/math/prim/err.hpp>
#include <stan/math/prim/fun/constants.hpp>

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

}  // namespace

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
}

}  // namespace stanli
