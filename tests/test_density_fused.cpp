// The fused normal, cauchy, lognormal, beta and gamma kernels against the
// Stan Math path they can replace. Every activity mask, both propto
// settings, every scalar/vector shape, the elementwise variant, and edge
// values; the two paths must agree on rejections (same message) and be
// within the ULP limit of the density (0 for lognormal, beta and gamma).
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

enum class Dist { normal, cauchy, lognormal, beta, gamma };
constexpr int kDists = 5;

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
  ctx.in[0] = {ys.n == 0 ? nullptr : ys.data, static_cast<int64_t>(ys.size())};
  ctx.in[1] = {mus.n == 0 ? nullptr : mus.data,
               static_cast<int64_t>(mus.size())};
  ctx.in[2] = {ss.n == 0 ? nullptr : ss.data, static_cast<int64_t>(ss.size())};
  ctx.n_in = 3;
  ctx.out = {o.out.data(), n_out};
  ctx.variant = static_cast<uint8_t>(variant);
  ctx.scratch = o.scratch.data();
  try {
    switch (d) {
      case Dist::normal:
        stanli::dens::normal_lpdf_fwd_gen(ctx);
        break;
      case Dist::cauchy:
        stanli::dens::cauchy_lpdf_fwd_gen(ctx);
        break;
      case Dist::lognormal:
        stanli::dens::lognormal_lpdf_fwd_gen(ctx);
        break;
      case Dist::beta:
        stanli::dens::beta_lpdf_fwd_gen(ctx);
        break;
      case Dist::gamma:
        stanli::dens::gamma_lpdf_fwd_gen(ctx);
        break;
    }
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
double worst_by_mask[kDists][8];
double worst_overall = 0;
const double kMaxUlp[kDists] = {2, 2, 0, 0, 0};

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
  double& slot = worst_by_mask[static_cast<int>(d)][mask];
  slot = std::max(slot, worst);
  worst_overall = std::max(worst_overall, worst);
  const double limit = kMaxUlp[static_cast<int>(d)];
  if (worst > limit) {
    report("more ULP than allowed");
    if (failures <= 3) {
      for (size_t i = 0; i < stan.out.size(); ++i)
        std::printf("  out[%zu] stan %.17g fused %.17g\n", i, stan.out[i],
                    fused.out[i]);
      for (size_t i = 0; i < stan.scratch.size(); ++i)
        if (ulp_distance(stan.scratch[i], fused.scratch[i]) > limit)
          std::printf("  scratch[%zu] stan %.17g fused %.17g\n", i,
                      stan.scratch[i], fused.scratch[i]);
    }
  }
}

enum class Kind { real, positive, unit, nonneg };

double draw(std::mt19937_64& rng, Kind kind) {
  std::normal_distribution<double> nd(0, 2);
  std::uniform_real_distribution<double> u(0, 1);
  if (kind == Kind::unit) {
    const double r = u(rng);
    if (r < 0.05) return 1e-300;
    if (r < 0.10) return 1.0 - 1e-16;
    if (r < 0.15) return 1e-8;
    if (r < 0.20) return 0.5;
    return u(rng) * 0.998 + 0.001;
  }
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
  if (kind == Kind::positive || kind == Kind::nonneg)
    return std::abs(v) + (v == 0 ? 1.0 : 0.0);
  return (u(rng) < 0.5) ? v : -v;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

struct Spec {
  const char* name;
  Dist dist;
  Kind kind[3];
  std::vector<double> bad[3];
  std::vector<double> edge[3];
  bool equal_y_mu;
};

const Spec kSpecs[] = {
    {"normal_lpdf",
     Dist::normal,
     {Kind::real, Kind::real, Kind::positive},
     {{kNaN}, {kInf, kNaN}, {0.0, -1.5, kNaN, kInf}},
     {{}, {}, {}},
     true},
    {"cauchy_lpdf",
     Dist::cauchy,
     {Kind::real, Kind::real, Kind::positive},
     {{kNaN}, {kInf, kNaN}, {0.0, -1.5, kNaN, kInf}},
     {{}, {}, {}},
     true},
    {"lognormal_lpdf",
     Dist::lognormal,
     {Kind::nonneg, Kind::real, Kind::positive},
     {{kNaN, -1.5, -1e-300, -kInf},
      {kInf, kNaN, -kInf},
      {0.0, -1.5, kNaN, kInf}},
     {{0.0, -0.0, 1.0, kInf, 1e-300}, {}, {}},
     false},
    {"beta_lpdf",
     Dist::beta,
     {Kind::unit, Kind::positive, Kind::positive},
     {{kNaN, -0.1, 1.5, kInf},
      {0.0, -1.5, kNaN, kInf},
      {0.0, -1.5, kNaN, kInf}},
     {{0.0, 1.0, 1e-300}, {1.0, 1e-300}, {1.0, 1e-300}},
     false},
    {"gamma_lpdf",
     Dist::gamma,
     {Kind::positive, Kind::positive, Kind::positive},
     {{kNaN, 0.0, -1.5, kInf},
      {0.0, -1.5, kNaN, kInf},
      {0.0, -1.5, kNaN, kInf}},
     {{1e-300}, {1.0, 1e-300}, {1.0, 1e-300}},
     false},
};

const double kSpecial[] = {kNaN, kInf,   -kInf, 0.0, -0.0,
                           -1.5, 1e-300, 1.0,   1.5, 1e300};

void one_case(const Spec& spec, const std::vector<double> (&args)[3],
              unsigned variant, int64_t n_out, long& cases, long& rejected) {
  const Outcome s =
      run(spec.dist, args[0], args[1], args[2], variant, n_out, false);
  const Outcome f =
      run(spec.dist, args[0], args[1], args[2], variant, n_out, true);
  ++cases;
  if (s.threw) ++rejected;
  compare(spec.name, spec.dist, variant & 7u, variant, s, f, args[0], args[1],
          args[2]);
}

// A special value (NaN, infinities, zeros, negatives, extremes) at the first,
// middle and last index of each argument in turn, in every shape, size,
// activity mask, propto setting and both output forms. Whatever the Stan Math
// path accepts or rejects, and with which message, the fused path must too.
void systematic_single(const Spec& spec, std::mt19937_64& rng, long& cases,
                       long& rejected) {
  const int sizes[] = {2, 3, 7, 8, 9, 16, 17, 33, 129};
  const unsigned masks[] = {7u, 5u, 0u};
  for (int k = 0; k < 3; ++k) {
    for (double special : kSpecial) {
      for (int shape = 1; shape < 8; ++shape) {
        const bool vec[3] = {(shape & 1) != 0, (shape & 2) != 0,
                             (shape & 4) != 0};
        for (int n : sizes) {
          const size_t N = static_cast<size_t>(n);
          const size_t positions[] = {0, N / 2, N - 1};
          for (size_t pos : positions) {
            if (!vec[k] && pos != 0) continue;
            for (int off = 0; off < 2; ++off) {
              g_offset = off;
              std::vector<double> args[3];
              for (int j = 0; j < 3; ++j) {
                args[j].resize(vec[j] ? N : 1);
                for (auto& x : args[j]) x = draw(rng, spec.kind[j]);
              }
              args[k][pos] = special;
              for (unsigned mask : masks)
                for (int propto = 0; propto < 2; ++propto)
                  for (int elt = 0; elt < 2; ++elt) {
                    unsigned variant = mask | (propto ? 0x80u : 0u);
                    int64_t n_out = 1;
                    if (elt) {
                      variant |= 0x40u;
                      n_out = static_cast<int64_t>(N);
                    }
                    one_case(spec, args, variant, n_out, cases, rejected);
                  }
            }
          }
        }
      }
    }
  }
  std::vector<double> args[3];
  for (int k = 0; k < 3; ++k) {
    for (double special : kSpecial) {
      for (int j = 0; j < 3; ++j) args[j].assign(1, draw(rng, spec.kind[j]));
      args[k][0] = special;
      for (unsigned mask : masks)
        for (int propto = 0; propto < 2; ++propto)
          one_case(spec, args, mask | (propto ? 0x80u : 0u), 1, cases,
                   rejected);
    }
  }
}

// Several bad values at once, in different arguments, so that the order of
// the checks decides which message comes out.
void systematic_multiple(const Spec& spec, std::mt19937_64& rng, long& cases,
                         long& rejected) {
  std::uniform_real_distribution<double> u(0, 1);
  const int sizes[] = {2, 5, 8, 9, 17, 33, 129};
  for (int rep = 0; rep < 20000; ++rep) {
    g_offset = rep & 1;
    const int shape = 1 + static_cast<int>(u(rng) * 7) % 7;
    const bool vec[3] = {(shape & 1) != 0, (shape & 2) != 0, (shape & 4) != 0};
    const size_t N =
        static_cast<size_t>(sizes[static_cast<size_t>(u(rng) * 7) % 7]);
    std::vector<double> args[3];
    for (int j = 0; j < 3; ++j) {
      args[j].resize(vec[j] ? N : 1);
      for (auto& x : args[j]) x = draw(rng, spec.kind[j]);
      if (u(rng) < 0.6)
        args[j][static_cast<size_t>(u(rng) * args[j].size())] =
            kSpecial[static_cast<size_t>(u(rng) * 10) % 10];
      if (u(rng) < 0.2)
        args[j][static_cast<size_t>(u(rng) * args[j].size())] =
            kSpecial[static_cast<size_t>(u(rng) * 10) % 10];
    }
    unsigned variant =
        static_cast<unsigned>(u(rng) * 8) % 8 | (u(rng) < 0.5 ? 0x80u : 0u);
    int64_t n_out = 1;
    if (u(rng) < 0.3) {
      variant |= 0x40u;
      n_out = static_cast<int64_t>(N);
    }
    one_case(spec, args, variant, n_out, cases, rejected);
  }
}

// Sizes of zero, one and several in every combination, clean and with a
// special value in one argument.
void systematic_sizes(const Spec& spec, std::mt19937_64& rng, long& cases,
                      long& rejected) {
  std::uniform_real_distribution<double> u(0, 1);
  const size_t sizes[] = {0, 1, 2, 5};
  for (size_t a : sizes)
    for (size_t b : sizes)
      for (size_t c : sizes) {
        const size_t lens[3] = {a, b, c};
        for (int bad = -1; bad < 3; ++bad)
          for (unsigned variant : {0u, 7u, 0x87u, 0x80u, 0x85u, 0x02u}) {
            std::vector<double> args[3];
            for (int j = 0; j < 3; ++j) {
              args[j].resize(lens[j]);
              for (auto& x : args[j]) x = draw(rng, spec.kind[j]);
            }
            if (bad >= 0 && !args[bad].empty())
              args[bad][static_cast<size_t>(u(rng) * args[bad].size())] =
                  kSpecial[static_cast<size_t>(u(rng) * 10) % 10];
            one_case(spec, args, variant, 1, cases, rejected);
          }
      }
}

}  // namespace

int main() {
  std::mt19937_64 rng(20251004);
  std::uniform_real_distribution<double> u(0, 1);
  const int sizes[] = {1, 2, 3, 5, 8, 17, 64, 128, 129, 600, 1000};
  long cases = 0, rejected = 0;

  for (const Spec& spec : kSpecs) {
    const Dist d = spec.dist;
    for (unsigned mask = 0; mask < 8; ++mask) {
      for (int propto = 0; propto < 2; ++propto) {
        for (int shape = 0; shape < 8; ++shape) {
          for (int n : sizes) {
            for (int rep = 0; rep < 12; ++rep) {
              g_offset = rep & 1;
              for (int elt = 0; elt < 2; ++elt) {
                const bool vec[3] = {(shape & 1) != 0, (shape & 2) != 0,
                                     (shape & 4) != 0};
                const size_t N = static_cast<size_t>(n);
                if (!(vec[0] || vec[1] || vec[2]) && N != 1) continue;
                if (n == 1 && (vec[0] || vec[1] || vec[2])) continue;
                std::vector<double> args[3];
                for (int k = 0; k < 3; ++k) {
                  args[k].resize(vec[k] ? N : 1);
                  for (auto& x : args[k]) x = draw(rng, spec.kind[k]);
                  for (const auto& e : spec.edge[k])
                    if (u(rng) < 0.04)
                      args[k][static_cast<size_t>(u(rng) * args[k].size())] = e;
                }
                if (spec.equal_y_mu && u(rng) < 0.15) args[0][0] = args[1][0];
                if (u(rng) < 0.21) {
                  const int k = static_cast<int>(u(rng) * 3) % 3;
                  const auto& b = spec.bad[k];
                  args[k][static_cast<size_t>(u(rng) * args[k].size())] =
                      b[static_cast<size_t>(u(rng) * b.size()) % b.size()];
                }
                unsigned variant = mask | (propto ? 0x80u : 0u);
                int64_t n_out = 1;
                if (elt) {
                  if (N == 1) continue;
                  variant |= 0x40u;
                  n_out = static_cast<int64_t>(N);
                }
                const Outcome s =
                    run(d, args[0], args[1], args[2], variant, n_out, false);
                const Outcome f =
                    run(d, args[0], args[1], args[2], variant, n_out, true);
                ++cases;
                if (s.threw) ++rejected;
                compare(spec.name, d, mask, variant, s, f, args[0], args[1],
                        args[2]);
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
        compare(spec.name, d, variant & 7u, variant, s, f, y, mu, sigma);
      }
    }
    systematic_single(spec, rng, cases, rejected);
    systematic_multiple(spec, rng, cases, rejected);
    systematic_sizes(spec, rng, cases, rejected);
  }

  for (const Spec& spec : kSpecs) {
    std::printf("%s max ULP by mask:", spec.name);
    for (unsigned m = 0; m < 8; ++m)
      std::printf(" %g", worst_by_mask[static_cast<int>(spec.dist)][m]);
    std::printf("\n");
  }
  std::printf("%ld cases, %ld rejected, max ULP %g\n", cases, rejected,
              worst_overall);

  stanli::dens::set_fused_density(true);
  {
    std::vector<double> y{0.5, 0.25, 0.75}, mu{0.25}, sigma{1.5};
    for (const Spec& spec : kSpecs) {
      const size_t before = stanli::dens::fused_density_calls();
      run(spec.dist, y, mu, sigma, 0x87, 1, true);
      if (stanli::dens::fused_density_calls() != before + 1) {
        ++failures;
        std::printf("FAIL %s: fused path not taken\n", spec.name);
      }
      stanli::dens::set_fused_density(false);
      const size_t mid = stanli::dens::fused_density_calls();
      run(spec.dist, y, mu, sigma, 0x87, 1, false);
      if (stanli::dens::fused_density_calls() != mid) {
        ++failures;
        std::printf(
            "FAIL %s: oracle switch does not select the Stan Math path\n",
            spec.name);
      }
      stanli::dens::set_fused_density(true);
    }
  }

  if (failures == 0) std::printf("ok\n");
  return failures == 0 ? 0 : 1;
}
