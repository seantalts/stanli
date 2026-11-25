// The integer-outcome densities: the hand-written lpmfs, the ones
// generated from STANLI_INT_DENSITY_LIST, the binomials with their two
// int groups (lpmf and cdf alike), and beta_binomial.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"
#include <stanli/density_layout.hpp>

namespace stanli {
namespace dens {

STANLI_LPMF_FWD(poisson_log_recorded_fwd, poisson_log_lpmf, 1)
STANLI_LPMF_FWD(bernoulli_logit_recorded_fwd, bernoulli_logit_lpmf, 1)
STANLI_LPMF_FWD(poisson_fwd, poisson_lpmf, 1)
#undef STANLI_LPMF_FWD

// Stan Math's neg_binomial_2_lpmf with three changes where its formula
// loses bits for large n:
//  - for mu > phi, n * log(mu) - n * log(mu + phi) becomes
//    -n * log1p(phi / mu); at n = 2000, mu = 5000, phi = 1 the old form is
//    about 1000 ULP off;
//  - the mu partial n / mu - (n + phi) / (mu + phi) becomes
//    (n - mu) / mu * phi / (mu + phi), which does not cancel;
//  - binomial_coefficient_log(n + phi - 1, n) becomes
//    -lbeta(phi, n + 1) - log(n + phi). The old form gets phi back as
//    (n + phi - 1) - n + 1, which rounds it to the ULP of n.
// When propto drops the location term the remaining -n * log(mu + phi)
// stays as Stan Math has it.
template <bool propto, typename T_n, typename T_location, typename T_precision>
stan::return_type_t<T_location, T_precision> neg_binomial_2_lpmf(
    const T_n& n, const T_location& mu, const T_precision& phi) {
  using stan::math::as_array_or_scalar;
  using stan::math::as_column_vector_or_scalar;
  using stan::math::lbeta;
  using stan::math::digamma;
  using stan::math::include_summand;
  using stan::math::log1p;
  using stan::math::select;
  using stan::math::sum;
  using stan::math::value_of;
  using std::log;
  using T_partials_return =
      stan::partials_return_t<T_n, T_location, T_precision>;
  static constexpr const char* function = "neg_binomial_2_lpmf";
  stan::math::check_consistent_sizes(function, "Failures variable", n,
                                     "Location parameter", mu,
                                     "Precision parameter", phi);
  stan::ref_type_t<T_n> n_ref = n;
  stan::ref_type_t<T_location> mu_ref = mu;
  stan::ref_type_t<T_precision> phi_ref = phi;
  stan::math::check_nonnegative(function, "Failures variable", n_ref);
  stan::math::check_positive_finite(function, "Location parameter", mu_ref);
  stan::math::check_positive_finite(function, "Precision parameter", phi_ref);
  if (stan::math::size_zero(n, mu, phi)) return 0.0;
  constexpr bool include_precision =
      include_summand<propto, T_precision>::value;
  constexpr bool include_location = include_summand<propto, T_location>::value;
  if constexpr (!include_precision && !include_location) return 0.0;

  T_partials_return logp(0.0);
  auto ops_partials = stan::math::make_partials_propagator(mu_ref, phi_ref);
  auto n_vec = as_array_or_scalar(as_column_vector_or_scalar(n_ref));
  auto mu_vec = as_array_or_scalar(as_column_vector_or_scalar(mu_ref));
  auto phi_vec = as_array_or_scalar(as_column_vector_or_scalar(phi_ref));
  decltype(auto) mu_val = value_of(mu_vec);
  decltype(auto) phi_val = value_of(phi_vec);
  auto mu_plus_phi = mu_val + phi_val;
  auto log_mu_plus_phi = log(mu_plus_phi);
  auto n_plus_phi = value_of(n_vec) + phi_val;
  auto phi_term = -phi_val * log1p(mu_val / phi_val);
  auto choose_term = -lbeta(phi_val, value_of(n_vec) + 1.0) - log(n_plus_phi);
  if constexpr (include_location) {
    auto n_term = value_of(n_vec) * select(mu_val > phi_val,
                                           -log1p(phi_val / mu_val),
                                           log(mu_val) - log_mu_plus_phi);
    if constexpr (include_precision) {
      logp += sum(choose_term + n_term + phi_term);
    } else {
      logp += sum(n_term + phi_term);
    }
  } else {
    logp += sum(choose_term + phi_term - value_of(n_vec) * log_mu_plus_phi);
  }
  if constexpr (stan::is_autodiff_v<T_location>) {
    stan::math::partials<0>(ops_partials) =
        (value_of(n_vec) - mu_val) / mu_val * (phi_val / mu_plus_phi);
  }
  if constexpr (stan::is_autodiff_v<T_precision>) {
    auto log_term = select(mu_val < phi_val, log1p(-mu_val / mu_plus_phi),
                           log(phi_val) - log_mu_plus_phi);
    stan::math::partials<1>(ops_partials) =
        (mu_val - value_of(n_vec)) / mu_plus_phi + log_term -
        digamma(phi_val) + digamma(n_plus_phi);
  }
  return ops_partials.build(logp);
}

void neg_binomial_2_fwd(KernelCtx& ctx) {
  if (density_mode(ctx).elementwise) {
    density_fwd_elt<2, 3>(
        ctx,
        [&](int64_t n, const auto&... a) {
          return neg_binomial_2_lpmf<true>(ctx.idata[n], a...);
        },
        [&](int64_t n, const auto&... a) {
          return neg_binomial_2_lpmf<false>(ctx.idata[n], a...);
        });
    return;
  }
  Eigen::Map<const Eigen::VectorXi> y(ctx.idata,
                                      static_cast<Eigen::Index>(ctx.n_idata));
  density_fwd_v<2, 3>(
      ctx, [&](const auto&... a) { return neg_binomial_2_lpmf<true>(y, a...); },
      [&](const auto&... a) { return neg_binomial_2_lpmf<false>(y, a...); });
}

namespace {

// The generic density recorder is valuable for distributions with several
// arguments and complicated partials. Bernoulli's only real argument has a
// closed-form partial, though, and these kernels commonly appear hundreds of
// times in a graph. Keep Stan Math responsible for Bernoulli values and all
// scalar calls; the hot vector-logit branch below mirrors its body and packet
// reduction exactly. Both write the one partial column directly into
// density_bwd<1>'s existing scratch layout.
inline bool bernoulli_arg_active(const KernelCtx& ctx) {
  return density_mode(ctx).is_active(0);
}

inline bool bernoulli_drop_propto(const KernelCtx& ctx) {
  constexpr bool has_propto = (3 & STANLI_DENSITY_PROPTO) != 0;
  return has_propto && density_mode(ctx).propto && !bernoulli_arg_active(ctx);
}

template <bool Logit>
double bernoulli_value(int y, double theta, bool drop) {
  if constexpr (Logit) {
    return drop ? stan::math::bernoulli_logit_lpmf<true>(y, theta)
                : stan::math::bernoulli_logit_lpmf<false>(y, theta);
  } else {
    return drop ? stan::math::bernoulli_lpmf<true>(y, theta)
                : stan::math::bernoulli_lpmf<false>(y, theta);
  }
}

template <bool Logit>
double bernoulli_value(const Eigen::Map<const Eigen::VectorXi>& y,
                       const Desc& theta, bool drop) {
  if (theta.len == 1) {
    if constexpr (Logit) {
      return drop ? stan::math::bernoulli_logit_lpmf<true>(y, theta.data[0])
                  : stan::math::bernoulli_logit_lpmf<false>(y, theta.data[0]);
    } else {
      return drop ? stan::math::bernoulli_lpmf<true>(y, theta.data[0])
                  : stan::math::bernoulli_lpmf<false>(y, theta.data[0]);
    }
  }
  const Eigen::Map<const Eigen::VectorXd> theta_vec(theta.data, theta.len);
  if constexpr (Logit) {
    return drop ? stan::math::bernoulli_logit_lpmf<true>(y, theta_vec)
                : stan::math::bernoulli_logit_lpmf<false>(y, theta_vec);
  } else {
    return drop ? stan::math::bernoulli_lpmf<true>(y, theta_vec)
                : stan::math::bernoulli_lpmf<false>(y, theta_vec);
  }
}

inline double bernoulli_partial(int y, double theta) {
  return y == 1 ? stan::math::inv(theta) : stan::math::inv(theta - 1.0);
}

// This deliberately mirrors the strict inequalities and even the high-tail
// partial in the pinned Stan Math implementation. In particular, infinities
// are accepted (only NaN is rejected), and z == +/-20 takes the middle arm.
inline double bernoulli_logit_partial(int y, double theta) {
  const double sign = 2.0 * y - 1.0;
  const double z = sign * theta;
  const double exp_m_z = std::exp(-z);
  if (z > 20.0) return sign * exp_m_z;
  if (z >= -20.0) return sign * exp_m_z / (exp_m_z + 1.0);
  return sign;
}

// Stan Math's bernoulli_logit_lpmf body over arrays: f receives the
// per-element terms and then the partials, as Eigen expressions of
// z = sign * theta and exp(-z). The expressions hold temporaries, so they
// are only valid inside f.
template <typename S, typename Z, typename E, typename F>
void with_bernoulli_logit_terms(const S& signs, const Z& z, const E& exp_m_z,
                                F&& f) {
  constexpr double cutoff = 20.0;
  f((z > cutoff)
        .select(-exp_m_z, (z < -cutoff).select(z, -stan::math::log1p(exp_m_z))),
    (z > cutoff)
        .select(signs * exp_m_z,
                (z >= -cutoff)
                    .select(stan::math::promote_scalar<double>(signs * exp_m_z /
                                                               (exp_m_z + 1)),
                            stan::math::promote_scalar<double>(signs))));
}

// The vector shapes need Eigen's packet exp and reduction order to stay
// bitwise with Stan Math. Summed is one lp over a vector theta. Its one
// partial column is exactly the size of z, so that scratch holds z first and
// the finished partials after, which removes both the z allocation and the
// recorder edge. The elementwise form leaves the reduction off: out[i] is
// element i's lp, and theta may be one value for every element. Calling Stan
// Math once per element costs about four times as much, and a partitioned
// width-W lane presents thousands of elements at once.
template <bool Summed>
void bernoulli_logit_array_fwd(KernelCtx& ctx) {
  static constexpr const char* function = "bernoulli_logit_lpmf";
  const Desc& theta = ctx.in[0];
  const Eigen::Index n =
      static_cast<Eigen::Index>(Summed ? ctx.n_idata : ctx.out.len);
  const Eigen::Map<const Eigen::VectorXi> y(ctx.idata, n);
  Eigen::ArrayXd broadcast;
  if constexpr (Summed) {
    const Eigen::Map<const Eigen::VectorXd> theta_vec(theta.data, theta.len);
    stan::math::check_consistent_sizes(function, "Random variable", y,
                                       "Probability parameter", theta_vec);
    std::fill_n(ctx.scratch, static_cast<size_t>(theta.len + 1), 0.0);
    if (stan::math::size_zero(y, theta_vec)) {
      ctx.out.data[0] = 0.0;
      return;
    }
  } else {
    std::fill_n(ctx.scratch, static_cast<size_t>(2 * n), 0.0);
  }
  stan::math::check_bounded(function, "n", y, 0, 1);
  if (!Summed && theta.len == 1) broadcast.setConstant(n, theta.data[0]);
  const Eigen::Map<const Eigen::ArrayXd> theta_val(
      !Summed && theta.len == 1 ? broadcast.data() : theta.data, n);
  stan::math::check_not_nan(function, "Logit transformed probability parameter",
                            theta_val);
  if (bernoulli_drop_propto(ctx)) {
    Eigen::Map<Eigen::ArrayXd>(ctx.out.data, Summed ? 1 : n).setZero();
    return;
  }

  const auto& n_col = stan::math::as_column_vector_or_scalar(y);
  const auto& n_double = stan::math::value_of_rec(n_col);
  const auto signs = 2 * stan::math::as_array_or_scalar(n_double) - 1;
  const bool active = bernoulli_arg_active(ctx);
  if constexpr (Summed) {
    Eigen::Map<Eigen::ArrayXd> z(ctx.scratch, n);
    z = signs * theta_val;
    const Eigen::ArrayXd exp_m_z = stan::math::exp(-z);
    with_bernoulli_logit_terms(signs, z, exp_m_z,
                               [&](const auto& terms, const auto& partials) {
                                 ctx.out.data[0] = stan::math::sum(terms);
                                 if (active)
                                   z = partials;
                                 else
                                   z.setZero();
                               });
    if (active) ctx.scratch[n] = 1.0;
  } else {
    const Eigen::ArrayXd z = signs * theta_val;
    const Eigen::ArrayXd exp_m_z = stan::math::exp(-z);
    with_bernoulli_logit_terms(
        signs, z, exp_m_z, [&](const auto& terms, const auto& partials) {
          Eigen::Map<Eigen::ArrayXd>(ctx.out.data, n) = terms;
          if (!active) return;
          Eigen::Map<Eigen::ArrayXd>(ctx.scratch, n) = partials;
          Eigen::Map<Eigen::ArrayXd>(ctx.scratch + n, n).setConstant(1.0);
        });
  }
}

// Stan Math adds the terms of a vector theta in one running sum. Add them
// pairwise instead: a merged loop of scalar calls presents thousands at once.
// The terms are stored in `terms` (theta.len values) first: adding stored
// values pairwise is faster than forming each term inside the recursion.
double bernoulli_vector_value(const Eigen::Map<const Eigen::VectorXi>& y,
                              const Desc& theta, bool drop, double* terms) {
  static constexpr const char* function = "bernoulli_lpmf";
  const Eigen::Map<const Eigen::VectorXd> theta_vec(theta.data, theta.len);
  stan::math::check_consistent_sizes(function, "Random variable", y,
                                     "Probability parameter", theta_vec);
  stan::math::check_bounded(function, "n", y, 0, 1);
  stan::math::check_bounded(function, "Probability parameter", theta_vec, 0.0,
                            1.0);
  if (drop || y.size() == 0) return 0.0;
  for (int64_t i = 0; i < theta.len; ++i)
    terms[i] =
        y[i] == 1 ? std::log(theta.data[i]) : stan::math::log1m(theta.data[i]);
  return pairwise_sum(terms, theta.len);
}

template <bool Logit>
void bernoulli_native_fwd(KernelCtx& ctx) {
  const bool active = bernoulli_arg_active(ctx);
  const bool drop = bernoulli_drop_propto(ctx);
  const Desc& theta = ctx.in[0];

  if (density_mode(ctx).elementwise) {
    const int64_t n = ctx.out.len;
    std::fill_n(ctx.scratch, static_cast<size_t>(2 * n), 0.0);
    for (int64_t i = 0; i < n; ++i) {
      const double theta_i = theta.data[theta.len == 1 ? 0 : i];
      ctx.out.data[i] = bernoulli_value<Logit>(ctx.idata[i], theta_i, drop);
      if (active) {
        ctx.scratch[i] = Logit ? bernoulli_logit_partial(ctx.idata[i], theta_i)
                               : bernoulli_partial(ctx.idata[i], theta_i);
        ctx.scratch[n + i] = 1.0;
      }
    }
    return;
  }

  const Eigen::Map<const Eigen::VectorXi> y(
      ctx.idata, static_cast<Eigen::Index>(ctx.n_idata));
  ctx.out.data[0] = !Logit && theta.len > kPairwiseBlock
                        ? bernoulli_vector_value(y, theta, drop, ctx.scratch)
                        : bernoulli_value<Logit>(y, theta, drop);
  std::fill_n(ctx.scratch, static_cast<size_t>(theta.len + 1), 0.0);

  // Both functions return a literal zero before constructing their partials
  // edge for an empty input. Preserve that disconnected topology: multiplying
  // an empty density by an infinite downstream adjoint must not form inf * 0.
  if (!active || drop || ctx.n_idata == 0 || theta.len == 0) return;
  ctx.scratch[theta.len] = 1.0;

  if (theta.len == 1) {
    if constexpr (Logit) {
      double partial = 0.0;
      for (int64_t i = 0; i < ctx.n_idata; ++i)
        partial += bernoulli_logit_partial(ctx.idata[i], theta.data[0]);
      ctx.scratch[0] = partial;
    } else {
      int64_t successes = 0;
      for (int64_t i = 0; i < ctx.n_idata; ++i) successes += ctx.idata[i];
      if (successes == ctx.n_idata) {
        ctx.scratch[0] = static_cast<double>(ctx.n_idata) / theta.data[0];
      } else if (successes == 0) {
        ctx.scratch[0] =
            static_cast<double>(ctx.n_idata) / (theta.data[0] - 1.0);
      } else {
        ctx.scratch[0] =
            static_cast<double>(successes) * stan::math::inv(theta.data[0]);
        ctx.scratch[0] += static_cast<double>(ctx.n_idata - successes) *
                          stan::math::inv(theta.data[0] - 1.0);
      }
    }
    return;
  }

  for (int64_t i = 0; i < theta.len; ++i)
    ctx.scratch[i] = Logit
                         ? bernoulli_logit_partial(ctx.idata[i], theta.data[i])
                         : bernoulli_partial(ctx.idata[i], theta.data[i]);
}

}  // namespace

// Stan Math's vector poisson_log_lpmf forms sum(n * alpha) - sum(exp(alpha)),
// two sums that can cancel to a much smaller lp. With a vector of log rates,
// form each observation's term first and add the terms pairwise. The checks
// and the partials are Stan Math's.
void poisson_log_fwd(KernelCtx& ctx) {
  const Desc& alpha = ctx.in[0];
  const DensityMode mode = density_mode(ctx);
  if (mode.elementwise || alpha.len <= kPairwiseBlock) {
    poisson_log_recorded_fwd(ctx);
    return;
  }
  static constexpr const char* function = "poisson_log_lpmf";
  const Eigen::Map<const Eigen::VectorXi> n(
      ctx.idata, static_cast<Eigen::Index>(ctx.n_idata));
  const Eigen::Map<const Eigen::ArrayXd> alpha_val(alpha.data, alpha.len);
  stan::math::check_consistent_sizes(function, "Random variable", n,
                                     "Log rate parameter", alpha_val);
  stan::math::check_nonnegative(function, "Random variable", n);
  stan::math::check_not_nan(function, "Log rate parameter", alpha_val);
  std::fill_n(ctx.scratch, static_cast<size_t>(alpha.len + 1), 0.0);
  ctx.out.data[0] = 0.0;
  const bool active = mode.is_active(0);
  if (alpha.len == 0 || (mode.propto && !active)) return;
  for (int64_t i = 0; i < alpha.len; ++i) {
    if (alpha.data[i] == stan::math::INFTY ||
        (alpha.data[i] == stan::math::NEGATIVE_INFTY && n[i] != 0)) {
      ctx.out.data[0] = stan::math::LOG_ZERO;
      return;
    }
  }
  const Eigen::ArrayXd exp_alpha = stan::math::exp(alpha_val);
  for (int64_t i = 0; i < alpha.len; ++i) {
    double term = n[i] * alpha.data[i];
    term -= exp_alpha[i];
    if (!mode.propto) term -= stan::math::lgamma(n[i] + 1.0);
    ctx.scratch[i] = term;
  }
  ctx.out.data[0] = pairwise_sum(ctx.scratch, alpha.len);
  if (!active) {
    std::fill_n(ctx.scratch, static_cast<size_t>(alpha.len), 0.0);
    return;
  }
  Eigen::Map<Eigen::ArrayXd>(ctx.scratch, alpha.len) =
      n.array().cast<double>() - exp_alpha;
  ctx.scratch[alpha.len] = 1.0;
}

void bernoulli_fwd(KernelCtx& ctx) { bernoulli_native_fwd<false>(ctx); }
void bernoulli_logit_fwd(KernelCtx& ctx) {
  // A scalar theta against vector y contracts its lane partials with Eigen's
  // reduction order. Keep the recorder for that uncommon shape; the native
  // path covers scalar/scalar, vector/vector, and every rerolled elementwise
  // call without changing the contraction by a few ULPs.
  if (!density_mode(ctx).elementwise && ctx.in[0].len == 1 && ctx.n_idata > 1) {
    bernoulli_logit_recorded_fwd(ctx);
    return;
  }
  if (!density_mode(ctx).elementwise && ctx.in[0].len > 1) {
    bernoulli_logit_array_fwd<true>(ctx);
    return;
  }
  if (density_mode(ctx).elementwise && ctx.out.len > 1) {
    bernoulli_logit_array_fwd<false>(ctx);
    return;
  }
  bernoulli_native_fwd<true>(ctx);
}

// The same shape, one line per distribution (STANLI_INT_DENSITY_LIST).
#define STANLI_DEFINE_INT_DENSITY(code, fn, nreal, tier) \
  STANLI_LPMF_FWD_T(fn##_fwd_gen, fn, nreal, tier)
STANLI_INT_DENSITY_LIST(STANLI_DEFINE_INT_DENSITY)
#undef STANLI_DEFINE_INT_DENSITY
#undef STANLI_LPMF_FWD_T

// Binomials carry two integer groups, outcomes then trials (IntGroups in
// density_layout.hpp). A language int scalar broadcasts; stan-math would
// reject a size-1 vector against a longer group.
[[gnu::always_inline]] inline IntGroups binomial_groups(const KernelCtx& ctx) {
  return int_groups(ctx.idata, ctx.n_idata, 2);
}
template <typename F>
void with_int_group(const IntGroup& g, F&& f) {
  if (g.scalar)
    f(g.values[0]);
  else
    f(Eigen::Map<const Eigen::VectorXi>(g.values, g.count));
}
// The elementwise variant with the per-element recorder call left off: this
// walks stan-math's own branches for element i's value and its one partial,
// so each element is what the scalar op it replaces computed, without a
// tape. Survey_model presents 2,370 of them in one op, and at that width
// two things the summed form does once per call have to stay once per call:
// the argument checks, and the logs of a broadcast probability.
template <bool Logit>
void binomial_elt_vector_fwd(KernelCtx& ctx) {
  static constexpr const char* function =
      Logit ? "binomial_logit_lpmf" : "binomial_lpmf";
  const IntGroups groups = binomial_groups(ctx);
  const IntGroup& g1 = groups.group[0];
  const IntGroup& g2 = groups.group[1];
  const Desc& theta = ctx.in[0];
  const int64_t len = ctx.out.len;
  const DensityMode mode = density_mode(ctx);
  const bool active = mode.is_active(0);
  const bool propto = mode.propto;
  std::fill_n(ctx.scratch, static_cast<size_t>(2 * len), 0.0);
  with_int_group(g1, [&](const auto& n) {
    with_int_group(g2, [&](const auto& trials) {
      stan::math::check_bounded(function, "Successes variable", n, 0, trials);
      stan::math::check_nonnegative(function, "Population size parameter",
                                    trials);
    });
  });
  const Eigen::Map<const Eigen::VectorXd> theta_vec(theta.data, theta.len);
  if constexpr (Logit) {
    stan::math::check_finite(function, "Probability parameter", theta_vec);
  } else {
    stan::math::check_bounded(function, "Probability parameter", theta_vec, 0.0,
                              1.0);
  }
  if (propto && !active) {
    std::fill_n(ctx.out.data, static_cast<size_t>(len), 0.0);
    return;
  }
  const bool one_theta = theta.len == 1;
  double la = 0.0, lb = 0.0;
  const auto logs = [&](double t) {
    if constexpr (Logit) {
      la = stan::math::log_inv_logit(t);
      lb = stan::math::log1m_inv_logit(t);
    } else {
      la = std::log(t);
      lb = stan::math::log1m(t);
    }
  };
  if (one_theta) logs(theta.data[0]);
  for (int64_t i = 0; i < len; ++i) {
    const int n = g1.at(i);
    const int trials = g2.at(i);
    const double t = theta.data[one_theta ? 0 : i];
    if (!one_theta) logs(t);
    double lp = 0.0, partial = 0.0;
    if constexpr (Logit) {
      lp = n * la + (trials - n) * lb;
      if (!propto) lp += stan::math::binomial_coefficient_log(trials, n);
      partial = n - trials * std::exp(la);
    } else {
      if (!propto) lp += stan::math::binomial_coefficient_log(trials, n);
      if (trials != 0) {
        if (n == 0) {
          lp += trials * lb;
          partial = -(trials / (1.0 - t));
        } else if (n == trials) {
          lp += n * la;
          partial = n / t;
        } else {
          lp += n * la + (trials - n) * lb;
          partial = n / t - (trials - n) / (1.0 - t);
        }
      }
    }
    ctx.out.data[i] = lp;
    if (!active) continue;
    ctx.scratch[i] = partial;
    ctx.scratch[len + i] = 1.0;
  }
}

#define STANLI_BINOMIAL_FWD(fname, dist, logit)                        \
  void fname(KernelCtx& ctx) {                                         \
    if (density_mode(ctx).elementwise) {                               \
      if (ctx.out.len > 1) {                                           \
        binomial_elt_vector_fwd<logit>(ctx);                           \
        return;                                                        \
      }                                                                \
      const IntGroups groups = binomial_groups(ctx);                   \
      const IntGroup& g1 = groups.group[0];                            \
      const IntGroup& g2 = groups.group[1];                            \
      density_fwd_elt<1, 3>(                                           \
          ctx,                                                         \
          [&](int64_t n, const auto& theta) {                          \
            return stan::math::dist<true>(g1.at(n), g2.at(n), theta);  \
          },                                                           \
          [&](int64_t n, const auto& theta) {                          \
            return stan::math::dist<false>(g1.at(n), g2.at(n), theta); \
          });                                                          \
      return;                                                          \
    }                                                                  \
    const IntGroups groups = binomial_groups(ctx);                     \
    with_int_group(groups.group[0], [&](const auto& n) {               \
      with_int_group(groups.group[1], [&](const auto& N) {             \
        density_fwd_v<1, 3>(                                           \
            ctx,                                                       \
            [&](const auto& theta) {                                   \
              return stan::math::dist<true>(n, N, theta);              \
            },                                                         \
            [&](const auto& theta) {                                   \
              return stan::math::dist<false>(n, N, theta);             \
            });                                                        \
      });                                                              \
    });                                                                \
  }

STANLI_BINOMIAL_FWD(binomial_fwd, binomial_lpmf, false)
STANLI_BINOMIAL_FWD(binomial_logit_fwd, binomial_logit_lpmf, true)
#undef STANLI_BINOMIAL_FWD

// beta_binomial(n | N, alpha, beta): two integer groups, then two real
// arguments that behave like any other density's. Its integer groups are
// the binomial's.
void beta_binomial_fwd(KernelCtx& ctx) {
  if (density_mode(ctx).elementwise) {
    // One value per element, as the binomials above: partition and the
    // observation collapse both ask for it.
    const IntGroups groups = binomial_groups(ctx);
    const IntGroup& g1 = groups.group[0];
    const IntGroup& g2 = groups.group[1];
    density_fwd_elt<2, 2>(
        ctx,
        [&](int64_t n, const auto&... a) {
          return stan::math::beta_binomial_lpmf<true>(g1.at(n), g2.at(n), a...);
        },
        [&](int64_t n, const auto&... a) {
          return stan::math::beta_binomial_lpmf<false>(g1.at(n), g2.at(n),
                                                       a...);
        });
    return;
  }
  const IntGroups groups = binomial_groups(ctx);
  with_int_group(groups.group[0], [&](const auto& n) {
    with_int_group(groups.group[1], [&](const auto& N) {
      density_fwd_sum<2, 2, 0>(
          ctx,
          [&](const auto&... a) {
            return stan::math::beta_binomial_lpmf<true>(n, N, a...);
          },
          [&](const auto&... a) {
            return stan::math::beta_binomial_lpmf<false>(n, N, a...);
          });
    });
  });
}

// The binomials' cumulative-distribution functions, which have the same
// two integer groups and so live beside the unpacker rather than with the
// other cdfs. No propto form and no elementwise one, exactly as the
// one-group int cdfs in densities_cdf_b.cpp: cdf_fwd is what says so.
#define STANLI_DEFINE_TWO_INT_CDF_FWD(code, fn, nreal)     \
  void fn##_fwd_gen(KernelCtx& ctx) {                      \
    const IntGroups groups = binomial_groups(ctx);         \
    with_int_group(groups.group[0], [&](const auto& n) {   \
      with_int_group(groups.group[1], [&](const auto& N) { \
        cdf_fwd<nreal>(ctx, [&](const auto&... a) {        \
          return stan::math::fn(n, N, a...);               \
        });                                                \
      });                                                  \
    });                                                    \
  }
STANLI_TWO_INT_CDF_LIST(STANLI_DEFINE_TWO_INT_CDF_FWD)
#undef STANLI_DEFINE_TWO_INT_CDF_FWD

}  // namespace dens
}  // namespace stanli
