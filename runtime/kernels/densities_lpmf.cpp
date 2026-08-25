// The integer-outcome densities: the hand-written lpmfs, the ones
// generated from STANLI_INT_DENSITY_LIST, the binomials with their two
// int groups (lpmf and cdf alike), beta_binomial, and the ordered pair.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

STANLI_LPMF_FWD(poisson_log_fwd, poisson_log_lpmf, 1)
STANLI_LPMF_FWD(bernoulli_logit_recorded_fwd, bernoulli_logit_lpmf, 1)
STANLI_LPMF_FWD(poisson_fwd, poisson_lpmf, 1)
STANLI_LPMF_FWD(neg_binomial_2_fwd, neg_binomial_2_lpmf, 2)
#undef STANLI_LPMF_FWD

namespace {

// The generic density recorder is valuable for distributions with several
// arguments and complicated partials. Bernoulli's only real argument has a
// closed-form partial, though, and these kernels commonly appear hundreds of
// times in a graph. Keep Stan Math responsible for Bernoulli values and all
// scalar calls; the hot vector-logit branch below mirrors its body and packet
// reduction exactly. Both write the one partial column directly into
// density_bwd<1>'s existing scratch layout.
inline bool bernoulli_arg_active(const KernelCtx& ctx) {
  return ctx.variant == 0 || (ctx.variant & 0x01u) != 0;
}

inline bool bernoulli_drop_propto(const KernelCtx& ctx) {
  constexpr bool has_propto = (density_tier(3) & STANLI_DENSITY_PROPTO) != 0;
  return has_propto && (ctx.variant & 0x80u) != 0 && !bernoulli_arg_active(ctx);
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
  if (z > 20.0) return -exp_m_z;
  if (z >= -20.0) return sign * exp_m_z / (exp_m_z + 1.0);
  return sign;
}

// The hot summed/vector shape needs Eigen's packet exp and reduction order to
// stay bitwise with Stan Math. Its one partial column is also exactly the size
// of ntheta, so use that existing scratch as the temporary and then overwrite
// it with the finished partials. Compared with the recorder path this removes
// both the ntheta allocation and the recorder edge/partial allocation.
void bernoulli_logit_vector_fwd(KernelCtx& ctx) {
  static constexpr const char* function = "bernoulli_logit_lpmf";
  static constexpr double cutoff = 20.0;
  const Desc& theta = ctx.in[0];
  const Eigen::Map<const Eigen::VectorXi> y(
      ctx.idata, static_cast<Eigen::Index>(ctx.n_idata));
  const Eigen::Map<const Eigen::VectorXd> theta_vec(theta.data, theta.len);

  stan::math::check_consistent_sizes(function, "Random variable", y,
                                     "Probability parameter", theta_vec);
  std::fill_n(ctx.scratch, static_cast<size_t>(theta.len + 1), 0.0);
  if (stan::math::size_zero(y, theta_vec)) {
    ctx.out.data[0] = 0.0;
    return;
  }
  stan::math::check_bounded(function, "n", y, 0, 1);
  decltype(auto) theta_val = stan::math::to_ref(
      stan::math::as_value_column_array_or_scalar(theta_vec));
  stan::math::check_not_nan(function, "Logit transformed probability parameter",
                            theta_val);
  if (bernoulli_drop_propto(ctx)) {
    ctx.out.data[0] = 0.0;
    return;
  }

  const auto& n_col = stan::math::as_column_vector_or_scalar(y);
  const auto& n_double = stan::math::value_of_rec(n_col);
  const auto signs = 2 * stan::math::as_array_or_scalar(n_double) - 1;
  Eigen::Map<Eigen::ArrayXd> ntheta(ctx.scratch, theta.len);
  ntheta = signs * theta_val;
  Eigen::ArrayXd exp_m_ntheta = stan::math::exp(-ntheta);
  ctx.out.data[0] = stan::math::sum(
      (ntheta > cutoff)
          .select(-exp_m_ntheta,
                  (ntheta < -cutoff)
                      .select(ntheta, -stan::math::log1p(exp_m_ntheta))));

  if (!bernoulli_arg_active(ctx)) {
    ntheta.setZero();
    return;
  }
  ntheta =
      (ntheta > cutoff)
          .select(-exp_m_ntheta,
                  (ntheta >= -cutoff)
                      .select(stan::math::promote_scalar<double>(
                                  signs * exp_m_ntheta / (exp_m_ntheta + 1)),
                              stan::math::promote_scalar<double>(signs)));
  ctx.scratch[theta.len] = 1.0;
}

// The same expression with the reduction left off: out[i] is element i's lp.
// The per-element arm of bernoulli_native_fwd calls Stan Math once per
// element, which costs about four times as much per element as this does,
// and a partitioned width-W lane presents thousands of them at once.
void bernoulli_logit_elt_vector_fwd(KernelCtx& ctx) {
  static constexpr const char* function = "bernoulli_logit_lpmf";
  static constexpr double cutoff = 20.0;
  const Desc& theta = ctx.in[0];
  const Eigen::Index n = static_cast<Eigen::Index>(ctx.out.len);
  const Eigen::Map<const Eigen::VectorXi> y(ctx.idata, n);
  std::fill_n(ctx.scratch, static_cast<size_t>(2 * n), 0.0);
  stan::math::check_bounded(function, "n", y, 0, 1);
  Eigen::ArrayXd theta_val(n);
  if (theta.len == 1)
    theta_val.setConstant(theta.data[0]);
  else
    theta_val = Eigen::Map<const Eigen::ArrayXd>(theta.data, n);
  stan::math::check_not_nan(function, "Logit transformed probability parameter",
                            theta_val);
  Eigen::Map<Eigen::ArrayXd> out(ctx.out.data, n);
  if (bernoulli_drop_propto(ctx)) {
    out.setZero();
    return;
  }

  const auto& n_col = stan::math::as_column_vector_or_scalar(y);
  const auto& n_double = stan::math::value_of_rec(n_col);
  const Eigen::ArrayXd signs = 2 * stan::math::as_array_or_scalar(n_double) - 1;
  const Eigen::ArrayXd ntheta = signs * theta_val;
  const Eigen::ArrayXd exp_m_ntheta = stan::math::exp(-ntheta);
  out = (ntheta > cutoff)
            .select(-exp_m_ntheta,
                    (ntheta < -cutoff)
                        .select(ntheta, -stan::math::log1p(exp_m_ntheta)));
  if (!bernoulli_arg_active(ctx)) return;
  Eigen::Map<Eigen::ArrayXd>(ctx.scratch, n) =
      (ntheta > cutoff)
          .select(-exp_m_ntheta,
                  (ntheta >= -cutoff)
                      .select(stan::math::promote_scalar<double>(
                                  signs * exp_m_ntheta / (exp_m_ntheta + 1)),
                              stan::math::promote_scalar<double>(signs)));
  Eigen::Map<Eigen::ArrayXd>(ctx.scratch + n, n).setConstant(1.0);
}

template <bool Logit>
void bernoulli_native_fwd(KernelCtx& ctx) {
  const bool active = bernoulli_arg_active(ctx);
  const bool drop = bernoulli_drop_propto(ctx);
  const Desc& theta = ctx.in[0];

  if ((ctx.variant & 0x40u) != 0) {
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
  ctx.out.data[0] = bernoulli_value<Logit>(y, theta, drop);
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

void bernoulli_fwd(KernelCtx& ctx) { bernoulli_native_fwd<false>(ctx); }
void bernoulli_logit_fwd(KernelCtx& ctx) {
  // A scalar theta against vector y contracts its lane partials with Eigen's
  // reduction order. Keep the recorder for that uncommon shape; the native
  // path covers scalar/scalar, vector/vector, and every rerolled elementwise
  // call without changing the contraction by a few ULPs.
  if ((ctx.variant & 0x40u) == 0 && ctx.in[0].len == 1 && ctx.n_idata > 1) {
    bernoulli_logit_recorded_fwd(ctx);
    return;
  }
  if ((ctx.variant & 0x40u) == 0 && ctx.in[0].len > 1) {
    bernoulli_logit_vector_fwd(ctx);
    return;
  }
  if ((ctx.variant & 0x40u) != 0 && ctx.out.len > 1) {
    bernoulli_logit_elt_vector_fwd(ctx);
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

// Binomials carry two int groups; idata = [len_n, n..., len_N, N...].
// A length of -1 marks a language-level int scalar (stan-math broadcasts
// scalars; a size-1 vector would be a size error against a longer group).
template <typename F>
void with_int_group(const int* p, F&& f) {
  const int len = static_cast<int>(p[0]);
  if (len == -1)
    f(p[1], p + 2);
  else
    f(Eigen::Map<const Eigen::VectorXi>(p + 1, len), p + 1 + len);
}
// Element n of a group: the scalar for all n, or the vector's n-th entry.
int int_group_elem(const int* p, int64_t n) {
  return p[0] == -1 ? p[1] : p[1 + n];
}
const int* int_group_next(const int* p) {
  return p + (p[0] == -1 ? 2 : 1 + p[0]);
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
  const int* g1 = ctx.idata;
  const int* g2 = int_group_next(g1);
  const Desc& theta = ctx.in[0];
  const int64_t len = ctx.out.len;
  const bool active = (ctx.variant & 0x01u) != 0;
  const bool propto = (ctx.variant & 0x80u) != 0;
  std::fill_n(ctx.scratch, static_cast<size_t>(2 * len), 0.0);
  with_int_group(g1, [&](const auto& n, const int*) {
    with_int_group(g2, [&](const auto& trials, const int*) {
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
    const int n = int_group_elem(g1, i);
    const int trials = int_group_elem(g2, i);
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

#define STANLI_BINOMIAL_FWD(fname, dist, logit)                           \
  void fname(KernelCtx& ctx) {                                            \
    if (ctx.variant & 0x40u) {                                            \
      if (ctx.out.len > 1) {                                              \
        binomial_elt_vector_fwd<logit>(ctx);                              \
        return;                                                           \
      }                                                                   \
      const int* g1 = ctx.idata;                                          \
      const int* g2 = int_group_next(g1);                                 \
      density_fwd_elt<1, 3>(                                              \
          ctx,                                                            \
          [&](int64_t n, const auto& theta) {                             \
            return stan::math::dist<true>(int_group_elem(g1, n),          \
                                          int_group_elem(g2, n), theta);  \
          },                                                              \
          [&](int64_t n, const auto& theta) {                             \
            return stan::math::dist<false>(int_group_elem(g1, n),         \
                                           int_group_elem(g2, n), theta); \
          });                                                             \
      return;                                                             \
    }                                                                     \
    with_int_group(ctx.idata, [&](const auto& n, const int* rest) {       \
      with_int_group(rest, [&](const auto& N, const int*) {               \
        density_fwd_v<1, 3>(                                              \
            ctx,                                                          \
            [&](const auto& theta) {                                      \
              return stan::math::dist<true>(n, N, theta);                 \
            },                                                            \
            [&](const auto& theta) {                                      \
              return stan::math::dist<false>(n, N, theta);                \
            });                                                           \
      });                                                                 \
    });                                                                   \
  }

STANLI_BINOMIAL_FWD(binomial_fwd, binomial_lpmf, false)
STANLI_BINOMIAL_FWD(binomial_logit_fwd, binomial_logit_lpmf, true)
#undef STANLI_BINOMIAL_FWD

// beta_binomial(n | N, alpha, beta): two integer groups, then two real
// arguments that behave like any other density's. with_int_group unpacks
// the [len, vals...] pairs the lowering wrote, exactly as binomial does.
void beta_binomial_fwd(KernelCtx& ctx) {
  with_int_group(ctx.idata, [&](const auto& n, const int* rest) {
    with_int_group(rest, [&](const auto& N, const int*) {
      density_fwd_sum<2, density_tier(2), 0>(
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
#define STANLI_DEFINE_TWO_INT_CDF_FWD(code, fn, nreal, tier)               \
  void fn##_fwd_gen(KernelCtx& ctx) {                                      \
    with_int_group(ctx.idata, [&](const auto& n, const int* rest) {        \
      with_int_group(rest, [&](const auto& N, const int*) {                \
        cdf_fwd<nreal, density_tier(tier) & STANLI_DENSITY_FULL_MASKS>(    \
            ctx,                                                           \
            [&](const auto&... a) { return stan::math::fn(n, N, a...); }); \
      });                                                                  \
    });                                                                    \
  }
STANLI_TWO_INT_CDF_LIST(STANLI_DEFINE_TWO_INT_CDF_FWD)
#undef STANLI_DEFINE_TWO_INT_CDF_FWD

STANLI_ORDERED_DENSITY_LIST(STANLI_DEFINE_ORDERED_FWD)

}  // namespace dens
}  // namespace stanli
