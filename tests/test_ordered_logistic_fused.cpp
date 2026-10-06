// The fused ordered_logistic_lpmf kernel against the Stan Math path it
// replaces, for a shared cutpoint vector: scalar and vector locations,
// cutpoint counts from 1 to 10, both propto settings, edge values, and
// rejections, which must carry the same message. Results must be bitwise
// equal.
#include "../runtime/kernels/density_fused.hpp"

#include <stanli/density_registry.hpp>
#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>

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

constexpr double kSentinel = 12345.0;

struct Outcome {
  bool threw = false;
  std::string message;
  double value = 0;
  std::vector<double> scratch;
};

Outcome run(const std::vector<int>& y, const std::vector<double>& lambda,
            const std::vector<double>& cuts, unsigned variant, bool fused) {
  stanli::dens::set_fused_density(fused);
  std::vector<int> idata(y);
  idata.push_back(stanli::kVectorizedDensityLayoutMarker);
  idata.push_back(static_cast<int>(cuts.size()));
  idata.push_back(-1);
  std::vector<double> lam(lambda), c(cuts);
  Outcome o;
  o.scratch.assign(lam.size() + c.size() + 1, kSentinel);
  double out = kSentinel;
  stanli::KernelCtx ctx;
  ctx.in[0] = {lam.data(), static_cast<int64_t>(lam.size())};
  ctx.in[1] = {c.data(), static_cast<int64_t>(c.size())};
  ctx.n_in = 2;
  ctx.out = {&out, 1};
  ctx.variant = static_cast<uint8_t>(variant);
  ctx.scratch = o.scratch.data();
  ctx.idata = idata.data();
  ctx.n_idata = static_cast<int64_t>(idata.size());
  try {
    stanli::find_kernel(stanli::OP_ORDERED_LOGISTIC_LPMF)->forward(ctx);
  } catch (const std::exception& e) {
    o.threw = true;
    o.message = e.what();
  }
  o.value = out;
  return o;
}

uint64_t ordered_bits(double x) {
  uint64_t b;
  std::memcpy(&b, &x, sizeof b);
  return (b >> 63) ? ~b : (b | (uint64_t{1} << 63));
}

double ulp_distance(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return 0;
  if (std::isnan(a) || std::isnan(b))
    return std::numeric_limits<double>::infinity();
  if (a == b) return 0;
  const uint64_t x = ordered_bits(a), z = ordered_bits(b);
  return static_cast<double>(x > z ? x - z : z - x);
}

int failures = 0;
double worst_overall = 0;

void compare(const Outcome& stan, const Outcome& fused, size_t n, size_t k,
             unsigned variant) {
  auto report = [&](const char* what) {
    ++failures;
    if (failures < 20)
      std::printf("FAIL n=%zu cuts=%zu variant=0x%x: %s\n", n, k, variant,
                  what);
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
  double worst = ulp_distance(stan.value, fused.value);
  for (size_t i = 0; i < stan.scratch.size(); ++i)
    worst = std::max(worst, ulp_distance(stan.scratch[i], fused.scratch[i]));
  worst_overall = std::max(worst_overall, worst);
  if (worst > 0) {
    report("not bitwise equal");
    if (failures <= 3) {
      std::printf("  value stan %.17g fused %.17g\n", stan.value, fused.value);
      for (size_t i = 0; i < stan.scratch.size(); ++i)
        if (ulp_distance(stan.scratch[i], fused.scratch[i]) > 0)
          std::printf("  scratch[%zu] stan %.17g fused %.17g\n", i,
                      stan.scratch[i], fused.scratch[i]);
    }
  }
}

}  // namespace

int main() {
  std::mt19937_64 rng(20251005);
  std::uniform_real_distribution<double> u(0, 1);
  std::normal_distribution<double> nd(0, 2);
  const int cut_counts[] = {0, 1, 2, 3, 4, 6, 10};
  const int sizes[] = {1, 2, 3, 5, 8, 17, 64, 129, 600};
  long cases = 0, rejected = 0;

  for (int kc : cut_counts) {
    for (int n : sizes) {
      for (int rep = 0; rep < 40; ++rep) {
        const bool scalar_lambda = n == 1;
        std::vector<double> cuts(static_cast<size_t>(kc));
        double spread = u(rng) < 0.2 ? 1e-9 : (u(rng) < 0.2 ? 1e4 : 1.0);
        double c = nd(rng);
        for (auto& x : cuts) {
          c += spread * (0.01 + u(rng));
          x = c;
        }
        std::vector<double> lambda(scalar_lambda ? 1 : static_cast<size_t>(n));
        const double lscale = u(rng) < 0.15 ? 1e3 : 3.0;
        for (auto& x : lambda) x = lscale * nd(rng);
        if (u(rng) < 0.15 && !cuts.empty())
          lambda[0] = cuts[static_cast<size_t>(rep) % cuts.size()];
        std::vector<int> y(static_cast<size_t>(n));
        for (auto& v : y)
          v = 1 + static_cast<int>(u(rng) * (kc + 1)) % (kc + 1);
        const double bad = u(rng);
        if (bad < 0.03)
          y[0] = 0;
        else if (bad < 0.06)
          y[0] = kc + 2;
        else if (bad < 0.08)
          y[0] = -3;
        else if (bad < 0.11 && kc >= 2)
          std::swap(cuts[0], cuts[1]);
        else if (bad < 0.13 && kc >= 2)
          cuts[1] = cuts[0];
        else if (bad < 0.15 && !cuts.empty())
          cuts[0] = std::numeric_limits<double>::quiet_NaN();
        else if (bad < 0.17 && !cuts.empty())
          cuts[0] = -std::numeric_limits<double>::infinity();
        else if (bad < 0.19 && !cuts.empty())
          cuts.back() = std::numeric_limits<double>::infinity();
        else if (bad < 0.21)
          lambda[0] = std::numeric_limits<double>::infinity();
        else if (bad < 0.23)
          lambda[0] = std::numeric_limits<double>::quiet_NaN();
        else if (bad < 0.25 && n > 2)
          lambda.pop_back();
        for (unsigned variant : {0u, 0x80u, 0x87u}) {
          const Outcome s = run(y, lambda, cuts, variant, false);
          const Outcome f = run(y, lambda, cuts, variant, true);
          ++cases;
          if (s.threw) ++rejected;
          compare(s, f, static_cast<size_t>(n), cuts.size(), variant);
        }
      }
    }
  }

  const std::vector<std::vector<double>> cut_sets{{}, {0.5}, {-1, 2}};
  for (const auto& cs : cut_sets) {
    for (unsigned variant : {0u, 0x80u}) {
      const Outcome s = run({}, {}, cs, variant, false);
      const Outcome f = run({}, {}, cs, variant, true);
      ++cases;
      if (s.threw) ++rejected;
      compare(s, f, 0, cs.size(), variant);
    }
  }
  std::printf("%ld cases, %ld rejected, max ULP %g\n", cases, rejected,
              worst_overall);

  stanli::dens::set_fused_density(true);
  {
    std::vector<int> y{2, 1, 3};
    std::vector<double> lambda{0.5, -1.0, 2.0}, cuts{-0.5, 0.75};
    const size_t before = stanli::dens::fused_density_calls();
    run(y, lambda, cuts, 0x87, true);
    if (stanli::dens::fused_density_calls() != before + 1) {
      ++failures;
      std::printf("FAIL fused path not taken\n");
    }
    const size_t mid = stanli::dens::fused_density_calls();
    run(y, lambda, cuts, 0x87, false);
    if (stanli::dens::fused_density_calls() != mid) {
      ++failures;
      std::printf("FAIL oracle switch does not select the Stan Math path\n");
    }
  }

  if (failures == 0) std::printf("ok\n");
  return failures == 0 ? 0 : 1;
}
