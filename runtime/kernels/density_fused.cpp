#include "density_fused.hpp"

#include <stan/math/prim/err.hpp>
#include <stan/math/prim/fun/Eigen.hpp>
#include <stan/math/prim/fun/constants.hpp>
#include <stan/math/prim/fun/log1p.hpp>
#include <stan/math/prim/fun/sum.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
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

void normal_lpdf_fused(KernelCtx& ctx) { fused_lpdf<NormalOp>(ctx); }
void cauchy_lpdf_fused(KernelCtx& ctx) { fused_lpdf<CauchyOp>(ctx); }

}  // namespace dens
}  // namespace stanli
