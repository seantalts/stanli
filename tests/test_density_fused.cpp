// The fused normal and cauchy kernels against the Stan Math path they can
// replace. Every activity mask, both propto settings, every scalar/vector
// shape, the elementwise variant, and edge values; the two paths must agree
// on rejections (same message) and be within 2 ULP everywhere else.
#include "../runtime/kernels/density_fused.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

struct Outcome {
  bool threw = false;
  std::string message;
  std::vector<double> out;
  std::vector<double> scratch;
};

enum class Dist { normal, cauchy };

constexpr double kSentinel = 12345.0;

int g_offset = 0;

Outcome run(Dist d, const std::vector<double>& y, const std::vector<double>& mu,
            const std::vector<double>& sigma, unsigned variant, int64_t n_out,
            bool fused) {
  stanli::dens::set_fused_density(fused);
  std::vector<double> store(y.size() + mu.size() + sigma.size() + 3);
  double* base = store.data() + g_offset;
  struct View {
    double* data;
    size_t size() const { return n; }
    size_t n;
  };
  const View ys{base, y.size()};
  const View mus{base + y.size(), mu.size()};
  const View ss{base + y.size() + mu.size(), sigma.size()};
  std::copy(y.begin(), y.end(), ys.data);
  std::copy(mu.begin(), mu.end(), mus.data);
  std::copy(sigma.begin(), sigma.end(), ss.data);
  const int64_t total =
      static_cast<int64_t>(ys.size() + mus.size() + ss.size());
  Outcome o;
  o.out.assign(static_cast<size_t>(n_out), kSentinel);
  o.scratch.assign(static_cast<size_t>(std::max<int64_t>(total, 4 * n_out) + 1),
                   kSentinel);
  stanli::KernelCtx ctx;
  ctx.in[0] = {ys.data, static_cast<int64_t>(ys.size())};
  ctx.in[1] = {mus.data, static_cast<int64_t>(mus.size())};
  ctx.in[2] = {ss.data, static_cast<int64_t>(ss.size())};
  ctx.n_in = 3;
  ctx.out = {o.out.data(), n_out};
  ctx.variant = static_cast<uint8_t>(variant);
  ctx.scratch = o.scratch.data();
  try {
    if (d == Dist::normal)
      stanli::dens::normal_lpdf_fwd_gen(ctx);
    else
      stanli::dens::cauchy_lpdf_fwd_gen(ctx);
  } catch (const std::exception& e) {
    o.threw = true;
    o.message = e.what();
  }
  return o;
}

uint64_t ordered(double x) {
  uint64_t b;
  std::memcpy(&b, &x, sizeof b);
  return (b >> 63) ? ~b : (b | (uint64_t{1} << 63));
}

double ulp_distance(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return 0;
  if (std::isnan(a) || std::isnan(b))
    return std::numeric_limits<double>::infinity();
  if (a == b) return 0;
  const uint64_t x = ordered(a), y = ordered(b);
  return static_cast<double>(x > y ? x - y : y - x);
}

int failures = 0;
double worst_by_mask[2][8];
double worst_overall = 0;

void compare(const char* name, Dist d, unsigned mask, unsigned variant,
             const Outcome& stan, const Outcome& fused,
             const std::vector<double>& y, const std::vector<double>& mu,
             const std::vector<double>& sigma) {
  auto report = [&](const char* what) {
    ++failures;
    if (failures < 20)
      std::printf("FAIL %s mask=%u variant=0x%x |y|=%zu |mu|=%zu |s|=%zu: %s\n",
                  name, mask, variant, y.size(), mu.size(), sigma.size(), what);
  };
  if (stan.threw != fused.threw) {
    report("one path threw");
    return;
  }
  if (stan.threw) {
    if (stan.message != fused.message) {
      report("messages differ");
      if (failures < 20)
        std::printf("  stan:  %s\n  fused: %s\n", stan.message.c_str(),
                    fused.message.c_str());
    }
    return;
  }
  double worst = 0;
  for (size_t i = 0; i < stan.out.size(); ++i)
    worst = std::max(worst, ulp_distance(stan.out[i], fused.out[i]));
  for (size_t i = 0; i < stan.scratch.size(); ++i)
    worst = std::max(worst, ulp_distance(stan.scratch[i], fused.scratch[i]));
  double& slot = worst_by_mask[d == Dist::normal ? 0 : 1][mask];
  slot = std::max(slot, worst);
  worst_overall = std::max(worst_overall, worst);
  if (worst > 2) {
    report("more than 2 ULP");
    if (failures <= 3) {
      for (size_t i = 0; i < stan.out.size(); ++i)
        std::printf("  out[%zu] stan %.17g fused %.17g\n", i, stan.out[i],
                    fused.out[i]);
      for (size_t i = 0; i < stan.scratch.size(); ++i)
        if (ulp_distance(stan.scratch[i], fused.scratch[i]) > 2)
          std::printf("  scratch[%zu] stan %.17g fused %.17g\n", i,
                      stan.scratch[i], fused.scratch[i]);
    }
  }
}

double draw(std::mt19937_64& rng, bool positive) {
  std::normal_distribution<double> nd(0, 2);
  std::uniform_real_distribution<double> u(0, 1);
  const double r = u(rng);
  double v;
  if (r < 0.04)
    v = 1e200;
  else if (r < 0.08)
    v = 1e-200;
  else if (r < 0.12)
    v = 1e-8;
  else if (r < 0.16)
    v = 3e7;
  else
    v = nd(rng);
  if (positive) return std::abs(v) + (v == 0 ? 1.0 : 0.0);
  return (u(rng) < 0.5) ? v : -v;
}

}  // namespace

int main() {
  std::mt19937_64 rng(20251004);
  std::uniform_real_distribution<double> u(0, 1);
  const int sizes[] = {1, 2, 3, 5, 8, 17, 64, 600, 1000};
  const char* names[] = {"normal_lpdf", "cauchy_lpdf"};
  long cases = 0, rejected = 0;

  for (int di = 0; di < 2; ++di) {
    const Dist d = di == 0 ? Dist::normal : Dist::cauchy;
    for (unsigned mask = 0; mask < 8; ++mask) {
      for (int propto = 0; propto < 2; ++propto) {
        for (int shape = 0; shape < 8; ++shape) {
          for (int n : sizes) {
            for (int rep = 0; rep < 12; ++rep) {
              g_offset = rep & 1;
              for (int elt = 0; elt < 2; ++elt) {
                const bool yv = shape & 1, mv = shape & 2, sv = shape & 4;
                const size_t N = static_cast<size_t>(n);
                if ((yv || mv || sv) == false && N != 1) continue;
                if (n == 1 && (yv || mv || sv)) continue;
                std::vector<double> y(yv ? N : 1), mu(mv ? N : 1),
                    sigma(sv ? N : 1);
                for (auto& x : y) x = draw(rng, false);
                for (auto& x : mu) x = draw(rng, false);
                for (auto& x : sigma) x = draw(rng, true);
                if (u(rng) < 0.15) y[0] = mu[0];
                const double bad = u(rng);
                if (bad < 0.04)
                  y[0] = std::numeric_limits<double>::quiet_NaN();
                else if (bad < 0.07)
                  mu[0] = std::numeric_limits<double>::infinity();
                else if (bad < 0.10)
                  mu[0] = std::numeric_limits<double>::quiet_NaN();
                else if (bad < 0.13)
                  sigma[0] = 0.0;
                else if (bad < 0.16)
                  sigma[0] = -1.5;
                else if (bad < 0.19)
                  sigma[0] = std::numeric_limits<double>::quiet_NaN();
                else if (bad < 0.21)
                  sigma[0] = std::numeric_limits<double>::infinity();
                unsigned variant = mask | (propto ? 0x80u : 0u);
                int64_t n_out = 1;
                if (elt) {
                  if (N == 1) continue;
                  variant |= 0x40u;
                  n_out = static_cast<int64_t>(N);
                }
                const Outcome s = run(d, y, mu, sigma, variant, n_out, false);
                const Outcome f = run(d, y, mu, sigma, variant, n_out, true);
                ++cases;
                if (s.threw) ++rejected;
                compare(names[di], d, mask, variant, s, f, y, mu, sigma);
              }
            }
          }
        }
      }
    }
    const size_t lens[][3] = {{3, 5, 1}, {1, 4, 2}, {2, 2, 3}, {0, 3, 3},
                              {0, 1, 1}, {1, 1, 0}, {0, 0, 0}};
    for (const auto& l : lens) {
      std::vector<double> y(l[0], 0.5), mu(l[1], 0.1), sigma(l[2], 1.3);
      for (unsigned variant : {0u, 7u, 0x87u, 0x80u, 0x85u}) {
        const Outcome s = run(d, y, mu, sigma, variant, 1, false);
        const Outcome f = run(d, y, mu, sigma, variant, 1, true);
        ++cases;
        if (s.threw) ++rejected;
        compare(names[di], d, variant & 7u, variant, s, f, y, mu, sigma);
      }
    }
  }

  for (int di = 0; di < 2; ++di) {
    std::printf("%s max ULP by mask:", names[di]);
    for (unsigned m = 0; m < 8; ++m) std::printf(" %g", worst_by_mask[di][m]);
    std::printf("\n");
  }
  std::printf("%ld cases, %ld rejected, max ULP %g\n", cases, rejected,
              worst_overall);

  stanli::dens::set_fused_density(true);
  {
    std::vector<double> y{0.5, -1.0, 2.0}, mu{0.25}, sigma{1.5};
    const size_t before = stanli::dens::fused_density_calls();
    run(Dist::normal, y, mu, sigma, 0x87, 1, true);
    run(Dist::cauchy, y, mu, sigma, 0x87, 1, true);
    if (stanli::dens::fused_density_calls() != before + 2) {
      ++failures;
      std::printf("FAIL fused path not taken\n");
    }
    stanli::dens::set_fused_density(false);
    const size_t mid = stanli::dens::fused_density_calls();
    run(Dist::normal, y, mu, sigma, 0x87, 1, false);
    if (stanli::dens::fused_density_calls() != mid) {
      ++failures;
      std::printf("FAIL oracle switch does not select the Stan Math path\n");
    }
  }

  if (failures == 0) std::printf("ok\n");
  return failures == 0 ? 0 : 1;
}
