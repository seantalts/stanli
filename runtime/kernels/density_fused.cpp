#include "density_fused.hpp"

#include <stan/math/prim/err.hpp>
#include <stan/math/prim/fun/Eigen.hpp>
#include <stan/math/prim/fun/constants.hpp>
#include <stan/math/prim/fun/exp.hpp>
#include <stan/math/prim/fun/log1m_exp.hpp>
#include <stan/math/prim/fun/digamma.hpp>
#include <stan/math/prim/fun/lgamma.hpp>
#include <stan/math/prim/fun/log.hpp>
#include <stan/math/prim/fun/log1p.hpp>
#include <stan/math/prim/fun/square.hpp>
#include <stan/math/prim/fun/sum.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace stanli {
namespace dens {
namespace {

std::atomic<int> g_enabled{-1};
thread_local std::size_t g_calls = 0;

using VecView = Eigen::Map<const Eigen::Array<double, -1, 1>>;
using OutView = Eigen::Map<Eigen::Array<double, -1, 1>>;

struct Out {
  double* buf[3];
  int64_t len[3];
  double* connected;
  double* value;
};

template <typename F>
void with_view(const Desc& d, F&& f) {
  if (d.len == 1)
    f(d.data[0]);
  else
    f(VecView(d.data, d.len));
}

template <typename T>
constexpr bool is_scalar_v = std::is_arithmetic_v<T>;

template <typename T>
std::size_t size_of(const T& x) {
  if constexpr (is_scalar_v<T>)
    return 1;
  else
    return static_cast<std::size_t>(x.size());
}

template <typename T>
bool any_nan(const T& x) {
  if constexpr (is_scalar_v<T>)
    return std::isnan(x);
  else
    return x.isNaN().any();
}

template <typename T>
bool all_finite(const T& x) {
  if constexpr (is_scalar_v<T>)
    return std::isfinite(x);
  else
    return x.isFinite().all();
}

template <typename T>
bool all_positive(const T& x) {
  if constexpr (is_scalar_v<T>)
    return x > 0;
  else
    return (x > 0.0).all();
}

template <typename T>
bool all_positive_finite(const T& x) {
  return all_positive(x) && all_finite(x);
}

template <typename T>
bool all_nonnegative(const T& x) {
  if constexpr (is_scalar_v<T>)
    return x >= 0;
  else
    return (x >= 0.0).all();
}

template <typename T>
bool any_zero(const T& x) {
  if constexpr (is_scalar_v<T>)
    return x == 0;
  else
    return (x == 0.0).any();
}

template <typename T>
auto inv_of(const T& x) {
  if constexpr (is_scalar_v<T>)
    return 1.0 / x;
  else
    return x.inverse();
}

template <typename T>
auto log_of(const T& x) {
  if constexpr (is_scalar_v<T>)
    return std::log(x);
  else
    return x.log();
}

template <typename T>
auto square_of(const T& x) {
  if constexpr (is_scalar_v<T>)
    return x * x;
  else
    return x.square();
}

template <typename Arg, typename E>
void put(double* buf, int64_t len, const E& e) {
  if constexpr (is_scalar_v<Arg>)
    buf[0] = stan::math::sum(e);
  else
    OutView(buf, len) = e;
}

void zero_result(const Out& o) {
  *o.value = 0.0;
  for (int k = 0; k < 3; ++k)
    if (o.buf[k] != nullptr)
      std::fill_n(o.buf[k], static_cast<std::size_t>(o.len[k]), 0.0);
  *o.connected = 0.0;
}

void finish(const Out& o, double logp, unsigned mask) {
  if (mask == 0) {
    zero_result(o);
    *o.value = logp;
    return;
  }
  *o.value = logp;
  *o.connected = 1.0;
}

void log_zero_result(const Out& o, unsigned mask) {
  if (mask == 0) {
    zero_result(o);
    *o.value = stan::math::LOG_ZERO;
    return;
  }
  for (int k = 0; k < 3; ++k)
    if (((mask >> k) & 1u) != 0)
      std::fill_n(o.buf[k], static_cast<std::size_t>(o.len[k]), 0.0);
  *o.value = stan::math::LOG_ZERO;
  *o.connected = 1.0;
}

template <typename Y, typename M, typename S>
bool size_zero_any(const Y& y, const M& mu, const S& sigma) {
  return size_of(y) == 0 || size_of(mu) == 0 || size_of(sigma) == 0;
}

template <typename Y, typename M, typename S>
std::size_t max_size_of(const Y& y, const M& mu, const S& sigma) {
  return std::max({size_of(y), size_of(mu), size_of(sigma)});
}

template <typename T>
void note_size(const T& x, bool& have, std::size_t& first, bool& match) {
  if constexpr (!is_scalar_v<T>) {
    const std::size_t s = size_of(x);
    if (!have) {
      have = true;
      first = s;
    } else if (s != first) {
      match = false;
    }
  }
}

template <typename Y, typename M, typename S>
void check_sizes(const char* function, const Y& y, const M& mu,
                 const S& sigma) {
  bool have = false, match = true;
  std::size_t first = 0;
  note_size(y, have, first, match);
  note_size(mu, have, first, match);
  note_size(sigma, have, first, match);
  if (!match)
    stan::math::check_consistent_sizes(function, "Random variable", y,
                                       "Location parameter", mu,
                                       "Scale parameter", sigma);
}

template <typename Y, typename M, typename S>
void normal_summed(const Y& y, const M& mu, const S& sigma, unsigned mask,
                   bool propto, const Out& o) {
  static constexpr const char* function = "normal_lpdf";
  check_sizes(function, y, mu, sigma);
  if (any_nan(y) || !all_finite(mu) || !all_positive(sigma)) {
    stan::math::check_not_nan(function, "Random variable", y);
    stan::math::check_finite(function, "Location parameter", mu);
    stan::math::check_positive(function, "Scale parameter", sigma);
  }
  const bool ya = (mask & 1u) != 0, ma = (mask & 2u) != 0,
             sa = (mask & 4u) != 0;
  if (size_zero_any(y, mu, sigma) || (propto && mask == 0)) {
    zero_result(o);
    return;
  }
  const std::size_t N = max_size_of(y, mu, sigma);
  const auto inv_sigma = inv_of(sigma);
  const auto y_scaled = (y - mu) * inv_sigma;
  const auto y_scaled_sq = y_scaled * y_scaled;
  double logp = -0.5 * stan::math::sum(y_scaled_sq);
  if (!propto) logp += stan::math::NEG_LOG_SQRT_TWO_PI * N;
  if (!propto || sa)
    logp -= stan::math::sum(log_of(sigma)) * N / size_of(sigma);
  if (mask != 0) {
    const auto scaled_diff = inv_sigma * y_scaled;
    if (ya) put<Y>(o.buf[0], o.len[0], -scaled_diff);
    if (sa) put<S>(o.buf[2], o.len[2], inv_sigma * y_scaled_sq - inv_sigma);
    if (ma) put<M>(o.buf[1], o.len[1], scaled_diff);
  }
  finish(o, logp, mask);
}

template <typename Y, typename M, typename S>
void cauchy_summed(const Y& y, const M& mu, const S& sigma, unsigned mask,
                   bool propto, const Out& o) {
  static constexpr const char* function = "cauchy_lpdf";
  check_sizes(function, y, mu, sigma);
  if (size_zero_any(y, mu, sigma) || (propto && mask == 0)) {
    zero_result(o);
    return;
  }
  if (any_nan(y) || !all_finite(mu) || !all_positive_finite(sigma)) {
    stan::math::check_not_nan(function, "Random variable", y);
    stan::math::check_finite(function, "Location parameter", mu);
    stan::math::check_positive_finite(function, "Scale parameter", sigma);
  }
  const bool ya = (mask & 1u) != 0, ma = (mask & 2u) != 0,
             sa = (mask & 4u) != 0;
  const std::size_t N = max_size_of(y, mu, sigma);
  double logp = 0.0;
  const auto inv_sigma = inv_of(sigma);
  const auto y_minus_mu = y - mu;
  logp -= stan::math::sum(stan::math::log1p(square_of(y_minus_mu * inv_sigma)));
  if (!propto) logp -= N * stan::math::LOG_PI;
  if (!propto || sa)
    logp -= stan::math::sum(log_of(sigma)) * N / size_of(sigma);
  if (mask != 0) {
    const auto sigma_squared = square_of(sigma);
    const auto y_minus_mu_squared = square_of(y_minus_mu);
    if (ya || ma) {
      const auto emit = [&](const auto& mu_deriv) {
        if (ya) {
          if constexpr (is_scalar_v<Y>)
            o.buf[0][0] = -stan::math::sum(mu_deriv);
          else
            OutView(o.buf[0], o.len[0]) = -mu_deriv;
        }
        if (ma) put<M>(o.buf[1], o.len[1], mu_deriv);
      };
      const auto expr = 2 * y_minus_mu / (sigma_squared + y_minus_mu_squared);
      if constexpr (is_scalar_v<Y> && is_scalar_v<M> && is_scalar_v<S>) {
        emit(expr);
      } else if (ya && ma) {
        constexpr Eigen::Index kStack = 512;
        if (static_cast<Eigen::Index>(N) <= kStack) {
          alignas(64) double buf[kStack];
          Eigen::Map<Eigen::ArrayXd, Eigen::Aligned64> a(
              buf, static_cast<Eigen::Index>(N));
          a = expr;
          emit(a);
        } else {
          const Eigen::ArrayXd a = expr;
          emit(a);
        }
      } else {
        emit(expr);
      }
    }
    if (sa)
      put<S>(o.buf[2], o.len[2],
             (y_minus_mu_squared - sigma_squared) * inv_sigma /
                 (sigma_squared + y_minus_mu_squared));
  }
  finish(o, logp, mask);
}

template <typename E>
auto materialize(const E& e, double* buf, Eigen::Index n) {
  if constexpr (is_scalar_v<E>) {
    return e;
  } else {
    Eigen::Map<Eigen::ArrayXd, Eigen::Aligned64> a(buf, n);
    a = e;
    return a;
  }
}

template <typename E, typename F>
void with_ref(bool materialized, const E& e, double* buf, Eigen::Index n,
              F&& f) {
  if (materialized)
    f(materialize(e, buf, n));
  else
    f(e);
}

template <typename Y, typename M, typename S>
void lognormal_summed(const Y& y, const M& mu, const S& sigma, unsigned mask,
                      bool propto, const Out& o, double* work,
                      Eigen::Index stride) {
  static constexpr const char* function = "lognormal_lpdf";
  check_sizes(function, y, mu, sigma);
  if (!all_nonnegative(y) || !all_finite(mu) || !all_positive_finite(sigma)) {
    stan::math::check_nonnegative(function, "Random variable", y);
    stan::math::check_finite(function, "Location parameter", mu);
    stan::math::check_positive_finite(function, "Scale parameter", sigma);
  }
  if (size_zero_any(y, mu, sigma) || (propto && mask == 0)) {
    zero_result(o);
    return;
  }
  const bool ya = (mask & 1u) != 0, ma = (mask & 2u) != 0,
             sa = (mask & 4u) != 0;
  if (any_zero(y)) {
    log_zero_result(o, mask);
    return;
  }
  const std::size_t N = max_size_of(y, mu, sigma);
  const Eigen::Index n = static_cast<Eigen::Index>(N);
  const auto inv_sigma = inv_of(sigma);
  const auto inv_sigma_sq = square_of(inv_sigma);
  with_ref(!propto || ya, log_of(y), work, n, [&](const auto& log_y) {
    const auto logy_m_mu = materialize(log_y - mu, work + stride, n);
    double logp = N * stan::math::NEG_LOG_SQRT_TWO_PI -
                  0.5 * stan::math::sum(square_of(logy_m_mu) * inv_sigma_sq);
    if (!propto || sa)
      logp -= stan::math::sum(log_of(sigma)) * N / size_of(sigma);
    if (!propto || ya) logp -= stan::math::sum(log_y) * N / size_of(y);
    if (mask != 0) {
      const int active =
          static_cast<int>(ya) + static_cast<int>(ma) + static_cast<int>(sa);
      with_ref(active >= 2, logy_m_mu * inv_sigma_sq, work + 2 * stride, n,
               [&](const auto& logy_m_mu_div_sigma) {
                 if (ya)
                   put<Y>(o.buf[0], o.len[0], -(1 + logy_m_mu_div_sigma) / y);
                 if (ma) put<M>(o.buf[1], o.len[1], logy_m_mu_div_sigma);
                 if (sa)
                   put<S>(o.buf[2], o.len[2],
                          (logy_m_mu_div_sigma * logy_m_mu - 1) * inv_sigma);
               });
    }
    finish(o, logp, mask);
  });
}

struct OrderedOut {
  double* lambda_buf;
  int64_t lambda_len;
  double* cut_buf;
  int64_t cut_len;
  double* connected;
  double* value;
};

void zero_ordered(const OrderedOut& o) {
  *o.value = 0.0;
  std::fill_n(o.lambda_buf, static_cast<std::size_t>(o.lambda_len), 0.0);
  std::fill_n(o.cut_buf, static_cast<std::size_t>(o.cut_len), 0.0);
  *o.connected = 0.0;
}

template <typename Lambda>
void ordered_logistic_summed(const Lambda& lambda_val, const int* y,
                             Eigen::Index N, int K, const double* cut,
                             double* work, const OrderedOut& o) {
  using Arr = Eigen::Map<Eigen::ArrayXd, Eigen::Aligned64>;
  const Eigen::Index S = (N + 7) & ~Eigen::Index{7};
  Arr cuts_y1(work, N), cuts_y2(work + S, N), cut1(work + 2 * S, N),
      cut2(work + 3 * S, N), exp_m_abs_cut1(work + 4 * S, N),
      exp_m_abs_cut2(work + 5 * S, N), exp_cuts_diff(work + 6 * S, N),
      inv_logit_neg_cut2(work + 7 * S, N), inv_logit_neg_cut1(work + 8 * S, N),
      d1(work + 9 * S, N), d2(work + 10 * S, N);
  for (Eigen::Index i = 0; i < N; ++i) {
    const int c = y[i];
    cuts_y1(i) = c != K ? cut[c - 1] : INFINITY;
    cuts_y2(i) = c != 1 ? cut[c - 2] : -INFINITY;
  }
  cut2 = lambda_val - cuts_y2;
  cut1 = lambda_val - cuts_y1;
  auto m_log_1p_exp_cut1 =
      (cut1 > 0.0).select(-cut1, 0) - (-cut1.abs()).exp().log1p();
  auto m_log_1p_exp_m_cut2 =
      (cut2 <= 0.0).select(cut2, 0) - (-cut2.abs()).exp().log1p();
  Eigen::Map<const Eigen::Matrix<int, Eigen::Dynamic, 1>> y_vec(y, N);
  auto log1m_exp_cuts_diff = stan::math::log1m_exp(cut1 - cut2);
  const double logp =
      y_vec.cwiseEqual(1)
          .select(m_log_1p_exp_cut1,
                  y_vec.cwiseEqual(K).select(m_log_1p_exp_m_cut2,
                                             m_log_1p_exp_m_cut2 +
                                                 log1m_exp_cuts_diff +
                                                 m_log_1p_exp_cut1))
          .sum();
  exp_m_abs_cut1 = (-cut1.abs()).exp();
  exp_m_abs_cut2 = (-cut2.abs()).exp();
  exp_cuts_diff = stan::math::exp(cuts_y2 - cuts_y1);
  inv_logit_neg_cut2 = (cut2 > 0).select(exp_m_abs_cut2 / (1 + exp_m_abs_cut2),
                                         1 / (1 + exp_m_abs_cut2));
  inv_logit_neg_cut1 = (cut1 > 0).select(exp_m_abs_cut1 / (1 + exp_m_abs_cut1),
                                         1 / (1 + exp_m_abs_cut1));
  d1 = inv_logit_neg_cut2 - exp_cuts_diff / (exp_cuts_diff - 1);
  d2 = 1 / (1 - exp_cuts_diff) - inv_logit_neg_cut1;
  if (o.lambda_len == 1)
    o.lambda_buf[0] = d1.coeff(0) - d2.coeff(0);
  else
    OutView(o.lambda_buf, o.lambda_len) = d1 - d2;
  std::fill_n(o.cut_buf, static_cast<std::size_t>(o.cut_len), 0.0);
  for (Eigen::Index i = 0; i < N; ++i) {
    const int c = y[i];
    if (c != K) o.cut_buf[c - 1] += d2.coeff(i);
    if (c != 1) o.cut_buf[c - 2] -= d1.coeff(i);
  }
  *o.value = logp;
  *o.connected = 1.0;
}

struct Out4 {
  double* buf[4];
  int64_t len[4];
  double* connected;
  double* value;
};

void zero_result4(const Out4& o) {
  *o.value = 0.0;
  for (int k = 0; k < 4; ++k)
    std::fill_n(o.buf[k], static_cast<std::size_t>(o.len[k]), 0.0);
  *o.connected = 0.0;
}

template <typename Y, typename N, typename M, typename S>
void check_sizes4(const char* function, const Y& y, const N& nu, const M& mu,
                  const S& sigma) {
  bool have = false, match = true;
  std::size_t first = 0;
  note_size(y, have, first, match);
  note_size(nu, have, first, match);
  note_size(mu, have, first, match);
  note_size(sigma, have, first, match);
  if (!match)
    stan::math::check_consistent_sizes(
        function, "Random variable", y, "Degrees of freedom parameter", nu,
        "Location parameter", mu, "Scale parameter", sigma);
}

template <typename Y, typename Nu, typename M, typename S>
void student_t_summed(const Y& y, const Nu& nu, const M& mu, const S& sigma,
                      unsigned mask, bool propto, const Out4& o, double* work,
                      Eigen::Index stride) {
  static constexpr const char* function = "student_t_lpdf";
  check_sizes4(function, y, nu, mu, sigma);
  if (any_nan(y) || !all_positive_finite(nu) || !all_finite(mu) ||
      !all_positive_finite(sigma)) {
    stan::math::check_not_nan(function, "Random variable", y);
    stan::math::check_positive_finite(function, "Degrees of freedom parameter",
                                      nu);
    stan::math::check_finite(function, "Location parameter", mu);
    stan::math::check_positive_finite(function, "Scale parameter", sigma);
  }
  if (size_of(y) == 0 || size_of(nu) == 0 || size_of(mu) == 0 ||
      size_of(sigma) == 0 || (propto && mask == 0)) {
    zero_result4(o);
    return;
  }
  const bool ya = (mask & 1u) != 0, na = (mask & 2u) != 0,
             ma = (mask & 4u) != 0, sa = (mask & 8u) != 0;
  const std::size_t N =
      std::max({size_of(y), size_of(nu), size_of(mu), size_of(sigma)});
  const Eigen::Index n = static_cast<Eigen::Index>(N);
  const auto half_nu = 0.5 * nu;
  const auto square_y_scaled = stan::math::square((y - mu) / sigma);
  const auto sqn = materialize(square_y_scaled / nu, work, n);
  // Stan keeps log1p(sqn) lazy unless nu is active, and a lazy expression
  // sums in a different order than an array.
  double logp;
  decltype(materialize(stan::math::log1p(sqn), work, n)) log1p_val = [&] {
    if constexpr (is_scalar_v<decltype(sqn)>)
      return na ? stan::math::log1p(sqn) : 0.0;
    else
      return Eigen::Map<Eigen::ArrayXd, Eigen::Aligned64>(work + stride, n);
  }();
  if (na) {
    if constexpr (!is_scalar_v<decltype(sqn)>)
      log1p_val = stan::math::log1p(sqn);
    logp = -stan::math::sum((half_nu + 0.5) * log1p_val);
  } else {
    logp = -stan::math::sum((half_nu + 0.5) * stan::math::log1p(sqn));
  }
  if (!propto) logp -= stan::math::LOG_SQRT_PI * N;
  if (!propto || na) {
    logp += (stan::math::sum(stan::math::lgamma(half_nu + 0.5)) -
             stan::math::sum(stan::math::lgamma(half_nu)) -
             0.5 * stan::math::sum(stan::math::log(nu))) *
            N / size_of(nu);
  }
  if (!propto || sa)
    logp -= stan::math::sum(stan::math::log(sigma)) * N / size_of(sigma);
  if (mask != 0) {
    if (ya || ma) {
      const auto square_sigma = stan::math::square(sigma);
      const auto emit = [&](const auto& deriv_y_mu) {
        if (ya) put<Y>(o.buf[0], o.len[0], -deriv_y_mu);
        if (ma) put<M>(o.buf[2], o.len[2], deriv_y_mu);
      };
      const auto expr = (nu + 1) * (y - mu) / ((1 + sqn) * square_sigma * nu);
      if (ya && ma)
        emit(materialize(expr, work + 2 * stride, n));
      else
        emit(expr);
    }
    if (na || sa) {
      const auto emit = [&](const auto& rep_deriv) {
        if (na) {
          put<Nu>(o.buf[1], o.len[1],
                  0.5 * (stan::math::digamma(half_nu + 0.5) -
                         stan::math::digamma(half_nu) - log1p_val +
                         rep_deriv / nu));
        }
        if (sa) put<S>(o.buf[3], o.len[3], rep_deriv / sigma);
      };
      const auto expr = (nu + 1) * sqn / (1 + sqn) - 1;
      if (na && sa)
        emit(materialize(expr, work + 3 * stride, n));
      else
        emit(expr);
    }
    *o.value = logp;
    *o.connected = 1.0;
    return;
  }
  zero_result4(o);
  *o.value = logp;
}

template <typename Y, typename Nu, typename M, typename S>
void student_t_dispatch(const Y& y, const Nu& nu, const M& mu, const S& sigma,
                        unsigned mask, bool propto, const Out4& o,
                        std::size_t n) {
  constexpr Eigen::Index kStack = 128;
  const Eigen::Index stride =
      (static_cast<Eigen::Index>(n) + 7) & ~Eigen::Index{7};
  const auto run = [&](double* work) {
    student_t_summed(y, nu, mu, sigma, mask, propto, o, work, stride);
  };
  if (n <= static_cast<std::size_t>(kStack)) {
    alignas(64) double work[4 * kStack];
    run(work);
  } else {
    std::vector<double> heap(static_cast<std::size_t>(4 * stride + 8));
    double* work = heap.data();
    while (reinterpret_cast<std::uintptr_t>(work) % 64 != 0) ++work;
    run(work);
  }
}

void student_t_lpdf_run(KernelCtx& ctx) {
  ++g_calls;
  const unsigned variant = ctx.variant;
#ifdef STANLI_LITE_LP
  const bool propto = false;
#else
  const bool propto = (variant & 0x80u) != 0;
#endif
  if (variant & 0x40u) {
    const unsigned mask = variant & 0x3fu;
    const int64_t N = ctx.out.len;
    for (int64_t n = 0; n < N; ++n) {
      Out4 o;
      for (int k = 0; k < 4; ++k) {
        o.buf[k] = ctx.scratch + static_cast<int64_t>(k) * N + n;
        o.len[k] = 1;
      }
      o.connected = ctx.scratch + 4 * N + n;
      o.value = ctx.out.data + n;
      student_t_dispatch(ctx.in[0].data[ctx.in[0].len == 1 ? 0 : n],
                         ctx.in[1].data[ctx.in[1].len == 1 ? 0 : n],
                         ctx.in[2].data[ctx.in[2].len == 1 ? 0 : n],
                         ctx.in[3].data[ctx.in[3].len == 1 ? 0 : n], mask,
                         propto, o, 1);
    }
    return;
  }
  const unsigned mask = variant == 0 ? 15u : (variant & 0x3fu);
  Out4 o;
  int64_t off = 0;
  std::size_t n = 1;
  for (int k = 0; k < 4; ++k) {
    o.buf[k] = ctx.scratch + off;
    o.len[k] = ctx.in[k].len;
    off += ctx.in[k].len;
    n = std::max<std::size_t>(n, static_cast<std::size_t>(ctx.in[k].len));
  }
  o.connected = ctx.scratch + off;
  o.value = ctx.out.data;
  with_view(ctx.in[0], [&](const auto& y) {
    with_view(ctx.in[1], [&](const auto& nu) {
      with_view(ctx.in[2], [&](const auto& mu) {
        with_view(ctx.in[3], [&](const auto& sigma) {
          student_t_dispatch(y, nu, mu, sigma, mask, propto, o, n);
        });
      });
    });
  });
}

struct NormalOp {
  template <typename Y, typename M, typename S>
  static void run(const Y& y, const M& mu, const S& sigma, unsigned mask,
                  bool propto, const Out& o) {
    normal_summed(y, mu, sigma, mask, propto, o);
  }
};

struct CauchyOp {
  template <typename Y, typename M, typename S>
  static void run(const Y& y, const M& mu, const S& sigma, unsigned mask,
                  bool propto, const Out& o) {
    cauchy_summed(y, mu, sigma, mask, propto, o);
  }
};

template <int Arrays, typename F>
void with_work(std::size_t n, F&& f) {
  constexpr Eigen::Index kStack = 128;
  const Eigen::Index stride =
      (static_cast<Eigen::Index>(n) + 7) & ~Eigen::Index{7};
  if (n <= static_cast<std::size_t>(kStack)) {
    alignas(64) double work[Arrays * kStack];
    f(work, stride);
  } else {
    std::vector<double> heap(static_cast<std::size_t>(Arrays * stride + 8));
    double* work = heap.data();
    while (reinterpret_cast<std::uintptr_t>(work) % 64 != 0) ++work;
    f(work, stride);
  }
}

struct LognormalOp {
  template <typename Y, typename M, typename S>
  static void run(const Y& y, const M& mu, const S& sigma, unsigned mask,
                  bool propto, const Out& o) {
    with_work<3>(
        max_size_of(y, mu, sigma), [&](double* work, Eigen::Index stride) {
          lognormal_summed(y, mu, sigma, mask, propto, o, work, stride);
        });
  }
};

template <typename Op>
void fused_lpdf(KernelCtx& ctx) {
  ++g_calls;
  const unsigned variant = ctx.variant;
#ifdef STANLI_LITE_LP
  const bool propto = false;
#else
  const bool propto = (variant & 0x80u) != 0;
#endif
  if (variant & 0x40u) {
    const unsigned mask = variant & 0x3fu;
    const int64_t N = ctx.out.len;
    for (int64_t n = 0; n < N; ++n) {
      Out o;
      for (int k = 0; k < 3; ++k) {
        o.buf[k] = ctx.scratch + static_cast<int64_t>(k) * N + n;
        o.len[k] = 1;
      }
      o.connected = ctx.scratch + 3 * N + n;
      o.value = ctx.out.data + n;
      Op::run(ctx.in[0].data[ctx.in[0].len == 1 ? 0 : n],
              ctx.in[1].data[ctx.in[1].len == 1 ? 0 : n],
              ctx.in[2].data[ctx.in[2].len == 1 ? 0 : n], mask, propto, o);
    }
    return;
  }
  const unsigned mask = variant == 0 ? 7u : (variant & 0x3fu);
  Out o;
  int64_t off = 0;
  for (int k = 0; k < 3; ++k) {
    o.buf[k] = ctx.scratch + off;
    o.len[k] = ctx.in[k].len;
    off += ctx.in[k].len;
  }
  o.connected = ctx.scratch + off;
  o.value = ctx.out.data;
  with_view(ctx.in[0], [&](const auto& y) {
    with_view(ctx.in[1], [&](const auto& mu) {
      with_view(ctx.in[2], [&](const auto& sigma) {
        Op::run(y, mu, sigma, mask, propto, o);
      });
    });
  });
}

}  // namespace

#ifdef STANLI_FUSED_ONLY
namespace {
struct OracleSwitchNotice {
  OracleSwitchNotice() {
    const char* e = std::getenv("STANLI_NO_FUSED_DENSITY");
    if (e != nullptr && e[0] != '\0' && e[0] != '0')
      std::fputs(
          "stanli: STANLI_NO_FUSED_DENSITY is ignored: this build does not "
          "contain Stan Math's normal, cauchy, student_t and ordered_logistic "
          "kernels (CMake option STANLI_STAN_DENSITY_ORACLE=OFF)\n",
          stderr);
  }
} g_oracle_switch_notice;
}  // namespace
#endif

bool fused_density_enabled() {
  int v = g_enabled.load(std::memory_order_relaxed);
  if (v < 0) {
    const char* e = std::getenv("STANLI_NO_FUSED_DENSITY");
    v = !(e != nullptr && e[0] != '\0' && e[0] != '0');
    g_enabled.store(v, std::memory_order_relaxed);
  }
  return v != 0;
}

void set_fused_density(bool on) {
  g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
}

std::size_t fused_density_calls() { return g_calls; }

void ordered_logistic_lpmf_fused(KernelCtx& ctx) {
  ++g_calls;
  static constexpr const char* function = "ordered_logistic";
  const int64_t n = ctx.n_idata - 3;
  const int* y = ctx.idata;
  const int64_t L = ctx.in[0].len, C = ctx.in[1].len;
  const double* lambda = ctx.in[0].data;
  const double* cut = ctx.in[1].data;
  OrderedOut o{ctx.scratch,         L,           ctx.scratch + L, C,
               ctx.scratch + L + C, ctx.out.data};
  const bool scalar_lambda = L == 1;
  const Eigen::Map<const Eigen::ArrayXd> lambda_vec(lambda, L);
  const auto yvec = [&] { return std::vector<int>(y, y + n); };
  const auto cut_vec = [&] {
    return Eigen::Map<const Eigen::VectorXd>(cut, C);
  };
  const auto check_lambda = [&] {
    if (scalar_lambda)
      stan::math::check_finite(function, "Location parameter", lambda[0]);
    else
      stan::math::check_finite(function, "Location parameter", lambda_vec);
  };
  if (!scalar_lambda && n != L)
    stan::math::check_consistent_sizes(function, "Integers", yvec(),
                                       "Locations", lambda_vec);
  const bool lambda_ok =
      scalar_lambda ? std::isfinite(lambda[0]) : lambda_vec.isFinite().all();
  if (!lambda_ok) check_lambda();
  if (L == 0) {
    zero_ordered(o);
    return;
  }
  const int K = static_cast<int>(C) + 1;
  bool y_ok = true;
  for (int64_t i = 0; i < n; ++i) y_ok = y_ok && y[i] >= 1 && y[i] <= K;
  if (!y_ok)
    stan::math::check_bounded(function, "Random variable", yvec(), 1, K);
  bool cuts_ok = true;
  for (int64_t i = 1; i < C; ++i) cuts_ok = cuts_ok && cut[i] > cut[i - 1];
  if (C >= 1) cuts_ok = cuts_ok && std::isfinite(cut[0]);
  if (C >= 2) cuts_ok = cuts_ok && std::isfinite(cut[C - 1]);
  if (!cuts_ok) {
    stan::math::check_ordered(function, "Cut-points", cut_vec());
    if (K > 1) {
      if (K > 2)
        stan::math::check_finite(function, "Final cut-point", cut[K - 2]);
      stan::math::check_finite(function, "First cut-point", cut[0]);
    }
  }
  if (n != (scalar_lambda ? 1 : L))
    stan::math::check_size_match(function, "Integers", n, "Locations", L);
  const Eigen::Index N = scalar_lambda ? 1 : static_cast<Eigen::Index>(L);
  constexpr Eigen::Index kStack = 128;
  const Eigen::Index stride = (N + 7) & ~Eigen::Index{7};
  const auto run = [&](const auto& lambda_val) {
    if (N <= kStack) {
      alignas(64) double work[11 * kStack];
      ordered_logistic_summed(lambda_val, y, N, K, cut, work, o);
    } else {
      std::vector<double> heap(static_cast<std::size_t>(11 * stride + 8));
      double* work = heap.data();
      while (reinterpret_cast<std::uintptr_t>(work) % 64 != 0) ++work;
      ordered_logistic_summed(lambda_val, y, N, K, cut, work, o);
    }
  };
  if (scalar_lambda)
    run(lambda[0]);
  else
    run(lambda_vec);
}

void normal_lpdf_fused(KernelCtx& ctx) { fused_lpdf<NormalOp>(ctx); }
void cauchy_lpdf_fused(KernelCtx& ctx) { fused_lpdf<CauchyOp>(ctx); }
void student_t_lpdf_fused(KernelCtx& ctx) { student_t_lpdf_run(ctx); }
void lognormal_lpdf_fused(KernelCtx& ctx) { fused_lpdf<LognormalOp>(ctx); }

}  // namespace dens
}  // namespace stanli
