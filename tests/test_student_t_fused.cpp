// The fused student_t_lpdf kernel against the Stan Math path it replaces.
// Every activity mask (0 to 15), both propto settings, every scalar or
// vector shape of the four arguments, the elementwise variant, edge values
// and rejections, which must carry the same message. Results must be bitwise
// equal.
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

constexpr double kSentinel = 12345.0;
int g_offset = 0;

Outcome run(const std::vector<double>& y, const std::vector<double>& nu,
            const std::vector<double>& mu, const std::vector<double>& sigma,
            unsigned variant, int64_t n_out, bool fused) {
  stanli::dens::set_fused_density(fused);
  const std::vector<double>* args[4] = {&y, &nu, &mu, &sigma};
  size_t total = 0;
  for (auto* a : args) total += a->size();
  std::vector<double> store(total + 3);
  double* base = store.data() + g_offset;
  Outcome o;
  stanli::KernelCtx ctx;
  size_t off = 0;
  for (int k = 0; k < 4; ++k) {
    std::copy(args[k]->begin(), args[k]->end(), base + off);
    ctx.in[k] = {args[k]->empty() ? nullptr : base + off,
                 static_cast<int64_t>(args[k]->size())};
    off += args[k]->size();
  }
  o.out.assign(static_cast<size_t>(n_out), kSentinel);
  o.scratch.assign(
      static_cast<size_t>(
          std::max<int64_t>(static_cast<int64_t>(total), 5 * n_out) + 1),
      kSentinel);
  ctx.n_in = 4;
  ctx.out = {o.out.data(), n_out};
  ctx.variant = static_cast<uint8_t>(variant);
  ctx.scratch = o.scratch.data();
  try {
    stanli::dens::student_t_lpdf_fwd_gen(ctx);
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
  const uint64_t x = ordered(a), z = ordered(b);
  return static_cast<double>(x > z ? x - z : z - x);
}

int failures = 0;
double worst_by_mask[16];
double worst_overall = 0;

void compare(unsigned mask, unsigned variant, const Outcome& stan,
             const Outcome& fused, size_t shape, size_t n) {
  auto report = [&](const char* what) {
    ++failures;
    if (failures < 20)
      std::printf("FAIL mask=%u variant=0x%x shape=%zu n=%zu: %s\n", mask,
                  variant, shape, n, what);
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
  worst_by_mask[mask] = std::max(worst_by_mask[mask], worst);
  worst_overall = std::max(worst_overall, worst);
  if (worst > 0) {
    report("not bitwise equal");
    if (failures <= 3) {
      for (size_t i = 0; i < stan.out.size(); ++i)
        std::printf("  out[%zu] stan %.17g fused %.17g\n", i, stan.out[i],
                    fused.out[i]);
      for (size_t i = 0; i < stan.scratch.size(); ++i)
        if (ulp_distance(stan.scratch[i], fused.scratch[i]) > 0)
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
    v = 1e100;
  else if (r < 0.08)
    v = 1e-100;
  else if (r < 0.12)
    v = 1e-8;
  else if (r < 0.16)
    v = 3e7;
  else
    v = nd(rng);
  if (positive) return std::abs(v) + (v == 0 ? 1.0 : 0.0);
  return (u(rng) < 0.5) ? v : -v;
}

const double kSpecial[] = {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           0.0,
                           -0.0,
                           -1.5,
                           1e-300,
                           1.0,
                           1.5,
                           1e300};
const bool kPositive[4] = {false, true, false, true};

void one_case(const std::vector<double> (&a)[4], unsigned variant,
              int64_t n_out, long& cases, long& rejected, size_t shape,
              size_t n) {
  const Outcome s = run(a[0], a[1], a[2], a[3], variant, n_out, false);
  const Outcome f = run(a[0], a[1], a[2], a[3], variant, n_out, true);
  ++cases;
  if (s.threw) ++rejected;
  compare(variant & 15u, variant, s, f, shape, n);
}

// A special value at the first, middle and last index of each argument in
// turn, in every shape, size, mask, propto setting and output form.
void systematic_single(std::mt19937_64& rng, long& cases, long& rejected) {
  const int sizes[] = {2, 3, 7, 8, 9, 16, 17, 33, 129};
  const unsigned masks[] = {15u, 9u, 0u};
  for (int k = 0; k < 4; ++k)
    for (double special : kSpecial)
      for (int shape = 1; shape < 16; ++shape) {
        const bool vec[4] = {(shape & 1) != 0, (shape & 2) != 0,
                             (shape & 4) != 0, (shape & 8) != 0};
        for (int n : sizes) {
          const size_t N = static_cast<size_t>(n);
          const size_t positions[] = {0, N / 2, N - 1};
          for (size_t pos : positions) {
            if (!vec[k] && pos != 0) continue;
            for (int off = 0; off < 2; ++off) {
              g_offset = off;
              std::vector<double> a[4];
              for (int j = 0; j < 4; ++j) {
                a[j].resize(vec[j] ? N : 1);
                for (auto& x : a[j]) x = draw(rng, kPositive[j]);
              }
              a[k][pos] = special;
              for (unsigned mask : masks)
                for (int propto = 0; propto < 2; ++propto)
                  for (int elt = 0; elt < 2; ++elt) {
                    unsigned variant = mask | (propto ? 0x80u : 0u);
                    int64_t n_out = 1;
                    if (elt) {
                      variant |= 0x40u;
                      n_out = static_cast<int64_t>(N);
                    }
                    one_case(a, variant, n_out, cases, rejected,
                             static_cast<size_t>(shape), N);
                  }
            }
          }
        }
      }
  std::vector<double> a[4];
  for (int k = 0; k < 4; ++k)
    for (double special : kSpecial) {
      for (int j = 0; j < 4; ++j) a[j].assign(1, draw(rng, kPositive[j]));
      a[k][0] = special;
      for (unsigned mask : masks)
        for (int propto = 0; propto < 2; ++propto)
          one_case(a, mask | (propto ? 0x80u : 0u), 1, cases, rejected, 0, 1);
    }
}

// Several bad values at once, in different arguments, so that the order of
// the checks decides which message comes out.
void systematic_multiple(std::mt19937_64& rng, long& cases, long& rejected) {
  std::uniform_real_distribution<double> u(0, 1);
  const int sizes[] = {2, 5, 8, 9, 17, 33, 129};
  for (int rep = 0; rep < 40000; ++rep) {
    g_offset = rep & 1;
    const int shape = 1 + static_cast<int>(u(rng) * 15) % 15;
    const bool vec[4] = {(shape & 1) != 0, (shape & 2) != 0, (shape & 4) != 0,
                         (shape & 8) != 0};
    const size_t N =
        static_cast<size_t>(sizes[static_cast<size_t>(u(rng) * 7) % 7]);
    std::vector<double> a[4];
    for (int j = 0; j < 4; ++j) {
      a[j].resize(vec[j] ? N : 1);
      for (auto& x : a[j]) x = draw(rng, kPositive[j]);
      for (int t = 0; t < 2; ++t)
        if (u(rng) < (t == 0 ? 0.5 : 0.2))
          a[j][static_cast<size_t>(u(rng) * a[j].size())] =
              kSpecial[static_cast<size_t>(u(rng) * 10) % 10];
    }
    unsigned variant =
        static_cast<unsigned>(u(rng) * 16) % 16 | (u(rng) < 0.5 ? 0x80u : 0u);
    int64_t n_out = 1;
    if (u(rng) < 0.3) {
      variant |= 0x40u;
      n_out = static_cast<int64_t>(N);
    }
    one_case(a, variant, n_out, cases, rejected, static_cast<size_t>(shape), N);
  }
}

// Sizes of zero, one and several in every combination, clean and with a
// special value in one argument.
void systematic_sizes(std::mt19937_64& rng, long& cases, long& rejected) {
  std::uniform_real_distribution<double> u(0, 1);
  const size_t sizes[] = {0, 1, 2, 5};
  for (size_t l0 : sizes)
    for (size_t l1 : sizes)
      for (size_t l2 : sizes)
        for (size_t l3 : sizes) {
          const size_t lens[4] = {l0, l1, l2, l3};
          for (int bad = -1; bad < 4; ++bad)
            for (unsigned variant : {0u, 15u, 0x8fu, 0x80u, 0x85u, 0x04u}) {
              std::vector<double> a[4];
              for (int j = 0; j < 4; ++j) {
                a[j].resize(lens[j]);
                for (auto& x : a[j]) x = draw(rng, kPositive[j]);
              }
              if (bad >= 0 && !a[bad].empty())
                a[bad][static_cast<size_t>(u(rng) * a[bad].size())] =
                    kSpecial[static_cast<size_t>(u(rng) * 10) % 10];
              one_case(a, variant, 1, cases, rejected, 98, 0);
            }
        }
}

}  // namespace

int main() {
  std::mt19937_64 rng(20251005);
  std::uniform_real_distribution<double> u(0, 1);
  const int sizes[] = {1, 2, 3, 5, 8, 17, 64, 600, 1000};
  long cases = 0, rejected = 0;
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  for (unsigned mask = 0; mask < 16; ++mask) {
    for (int propto = 0; propto < 2; ++propto) {
      for (int shape = 0; shape < 16; ++shape) {
        for (int n : sizes) {
          for (int rep = 0; rep < 8; ++rep) {
            g_offset = rep & 1;
            for (int elt = 0; elt < 2; ++elt) {
              const bool vec[4] = {(shape & 1) != 0, (shape & 2) != 0,
                                   (shape & 4) != 0, (shape & 8) != 0};
              const size_t N = static_cast<size_t>(n);
              const bool any_vec = vec[0] || vec[1] || vec[2] || vec[3];
              if (!any_vec && N != 1) continue;
              if (n == 1 && any_vec) continue;
              std::vector<double> a[4];
              for (int k = 0; k < 4; ++k) a[k].resize(vec[k] ? N : 1);
              for (auto& x : a[0]) x = draw(rng, false);
              for (auto& x : a[1]) x = draw(rng, true);
              for (auto& x : a[2]) x = draw(rng, false);
              for (auto& x : a[3]) x = draw(rng, true);
              if (u(rng) < 0.15) a[0][0] = a[2][0];
              if (u(rng) < 0.1) a[1][0] = 1e-3;
              if (u(rng) < 0.1) a[1][0] = 1e8;
              const double bad = u(rng);
              if (bad < 0.03)
                a[0][0] = nan;
              else if (bad < 0.05)
                a[1][0] = 0.0;
              else if (bad < 0.07)
                a[1][0] = -2.0;
              else if (bad < 0.09)
                a[1][0] = inf;
              else if (bad < 0.11)
                a[1][0] = nan;
              else if (bad < 0.13)
                a[2][0] = inf;
              else if (bad < 0.15)
                a[2][0] = nan;
              else if (bad < 0.17)
                a[3][0] = 0.0;
              else if (bad < 0.19)
                a[3][0] = -1.5;
              else if (bad < 0.21)
                a[3][0] = inf;
              else if (bad < 0.23)
                a[3][0] = nan;
              unsigned variant = mask | (propto ? 0x80u : 0u);
              int64_t n_out = 1;
              if (elt) {
                if (N == 1) continue;
                variant |= 0x40u;
                n_out = static_cast<int64_t>(N);
              }
              const Outcome s =
                  run(a[0], a[1], a[2], a[3], variant, n_out, false);
              const Outcome f =
                  run(a[0], a[1], a[2], a[3], variant, n_out, true);
              ++cases;
              if (s.threw) ++rejected;
              compare(mask, variant, s, f, static_cast<size_t>(shape), N);
            }
          }
        }
      }
    }
  }
  const size_t lens[][4] = {{3, 1, 5, 1}, {1, 4, 2, 1}, {2, 2, 3, 3},
                            {0, 1, 3, 3}, {0, 1, 1, 1}, {1, 1, 0, 1},
                            {1, 0, 1, 1}, {1, 1, 1, 0}, {0, 0, 0, 0}};
  for (const auto& l : lens) {
    std::vector<double> y(l[0], 0.5), nu(l[1], 3.5), mu(l[2], 0.1),
        sigma(l[3], 1.3);
    for (unsigned variant : {0u, 15u, 0x8fu, 0x80u, 0x85u}) {
      const Outcome s = run(y, nu, mu, sigma, variant, 1, false);
      const Outcome f = run(y, nu, mu, sigma, variant, 1, true);
      ++cases;
      if (s.threw) ++rejected;
      compare(variant & 15u, variant, s, f, 99, 0);
    }
  }

  systematic_single(rng, cases, rejected);
  systematic_multiple(rng, cases, rejected);
  systematic_sizes(rng, cases, rejected);

  std::printf("max ULP by mask:");
  for (unsigned m = 0; m < 16; ++m) std::printf(" %g", worst_by_mask[m]);
  std::printf("\n%ld cases, %ld rejected, max ULP %g\n", cases, rejected,
              worst_overall);

  stanli::dens::set_fused_density(true);
  {
    std::vector<double> y{0.5, -1.0, 2.0}, nu{4.0}, mu{0.25}, sigma{1.5};
    const size_t before = stanli::dens::fused_density_calls();
    run(y, nu, mu, sigma, 0x8f, 1, true);
    if (stanli::dens::fused_density_calls() != before + 1) {
      ++failures;
      std::printf("FAIL fused path not taken\n");
    }
    const size_t mid = stanli::dens::fused_density_calls();
    run(y, nu, mu, sigma, 0x8f, 1, false);
    if (stanli::dens::fused_density_calls() != mid) {
      ++failures;
      std::printf("FAIL oracle switch does not select the Stan Math path\n");
    }
  }

  if (failures == 0) std::printf("ok\n");
  return failures == 0 ? 0 : 1;
}
