// The fused normal, cauchy, student_t, lognormal, beta, gamma and
// ordered_logistic (shared cutpoints) kernels against the Stan Math path they
// can replace. Every activity mask, both propto settings, every scalar/vector
// shape, the elementwise variant, and edge values; the two paths must agree
// on rejections (same message) and be within the ULP limit of the density
// (2 for normal and cauchy, 0 for the rest).
#include "../runtime/kernels/density_fused.hpp"

#include "expect.hpp"

#include <stanli/density_registry.hpp>
#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

int g_offset = 0;

using Forward = void (*)(stanli::KernelCtx&);

Outcome run(Forward forward, const std::vector<double>* const* args, int arity,
            unsigned variant, int64_t n_out, bool fused) {
  stanli::dens::set_fused_density(fused);
  size_t total = 0;
  for (int k = 0; k < arity; ++k) total += args[k]->size();
  std::vector<double> store(total + 3);
  double* base = store.data() + g_offset;
  Outcome o;
  stanli::KernelCtx ctx;
  size_t off = 0;
  for (int k = 0; k < arity; ++k) {
    std::copy(args[k]->begin(), args[k]->end(), base + off);
    ctx.in[k] = {args[k]->empty() ? nullptr : base + off,
                 static_cast<int64_t>(args[k]->size())};
    off += args[k]->size();
  }
  o.out.assign(static_cast<size_t>(n_out), kSentinel);
  o.scratch.assign(
      static_cast<size_t>(
          std::max<int64_t>(static_cast<int64_t>(total), (arity + 1) * n_out) +
          1),
      kSentinel);
  ctx.n_in = arity;
  ctx.out = {o.out.data(), n_out};
  ctx.variant = static_cast<uint8_t>(variant);
  ctx.scratch = o.scratch.data();
  try {
    forward(ctx);
  } catch (const std::exception& e) {
    o.threw = true;
    o.message = e.what();
  }
  return o;
}

double distance(double a, double b) {
  return both_nan(a, b) ? 0 : static_cast<double>(ulp_distance(a, b));
}

struct Tally {
  long cases = 0, rejected = 0;
  double worst = 0;
  double worst_by_mask[16] = {};
};

// `describe` prints the case after "FAIL <name> ".
template <class Describe>
void compare(const char* name, Tally& tally, unsigned mask, const Outcome& stan,
             const Outcome& fused, double limit, Describe describe) {
  ++tally.cases;
  if (stan.threw) ++tally.rejected;
  auto report = [&](const char* what) {
    ++failures;
    if (failures < 20) {
      std::printf("FAIL %s ", name);
      describe();
      std::printf(": %s\n", what);
    }
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
    worst = std::max(worst, distance(stan.out[i], fused.out[i]));
  for (size_t i = 0; i < stan.scratch.size(); ++i)
    worst = std::max(worst, distance(stan.scratch[i], fused.scratch[i]));
  tally.worst_by_mask[mask] = std::max(tally.worst_by_mask[mask], worst);
  tally.worst = std::max(tally.worst, worst);
  if (worst > limit) {
    report("more ULP than allowed");
    if (failures <= 3) {
      for (size_t i = 0; i < stan.out.size(); ++i)
        std::printf("  out[%zu] stan %.17g fused %.17g\n", i, stan.out[i],
                    fused.out[i]);
      for (size_t i = 0; i < stan.scratch.size(); ++i)
        if (distance(stan.scratch[i], fused.scratch[i]) > limit)
          std::printf("  scratch[%zu] stan %.17g fused %.17g\n", i,
                      stan.scratch[i], fused.scratch[i]);
    }
  }
}

enum class Kind { real, positive, unit, nonneg };

double draw(std::mt19937_64& rng, Kind kind, double big, double tiny) {
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
    v = big;
  else if (r < 0.08)
    v = tiny;
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

const double kSpecial[] = {kNaN, kInf,   -kInf, 0.0, -0.0,
                           -1.5, 1e-300, 1.0,   1.5, 1e300};

struct Spec;
using Args = std::vector<double>[4];
using Perturb = void (*)(const Spec&, std::mt19937_64&, Args&);

// How a density's arguments are drawn and how many cases each stage runs.
struct Plan {
  int arity;
  double big, tiny;  // magnitudes of the extreme draws
  std::vector<int> sizes;
  int reps;
  std::vector<std::vector<size_t>> lens;
  std::vector<double> fill;
  std::vector<unsigned> fixed_variants;
  std::vector<unsigned> single_masks;
  int multiple_reps;
  double multiple_first_p;
  std::vector<unsigned> size_variants;
};

struct Spec {
  const char* name;
  Forward forward;
  const Plan* plan;
  uint64_t seed;  // 0 continues the previous row's stream
  double max_ulp;
  Kind kind[4];
  std::vector<double> edge[4];
  int copy_into_y;  // argument whose first value y[0] takes 15% of the time
  Perturb perturb;
  std::vector<double> bad[4];
};

void listed_bad_value(const Spec& spec, std::mt19937_64& rng, Args& args) {
  std::uniform_real_distribution<double> u(0, 1);
  if (u(rng) < 0.21) {
    const int k = static_cast<int>(u(rng) * 3) % 3;
    const auto& b = spec.bad[k];
    args[k][static_cast<size_t>(u(rng) * args[k].size())] =
        b[static_cast<size_t>(u(rng) * b.size()) % b.size()];
  }
}

void student_t_bad_value(const Spec&, std::mt19937_64& rng, Args& a) {
  std::uniform_real_distribution<double> u(0, 1);
  if (u(rng) < 0.1) a[1][0] = 1e-3;
  if (u(rng) < 0.1) a[1][0] = 1e8;
  const double bad = u(rng);
  if (bad < 0.03)
    a[0][0] = kNaN;
  else if (bad < 0.05)
    a[1][0] = 0.0;
  else if (bad < 0.07)
    a[1][0] = -2.0;
  else if (bad < 0.09)
    a[1][0] = kInf;
  else if (bad < 0.11)
    a[1][0] = kNaN;
  else if (bad < 0.13)
    a[2][0] = kInf;
  else if (bad < 0.15)
    a[2][0] = kNaN;
  else if (bad < 0.17)
    a[3][0] = 0.0;
  else if (bad < 0.19)
    a[3][0] = -1.5;
  else if (bad < 0.21)
    a[3][0] = kInf;
  else if (bad < 0.23)
    a[3][0] = kNaN;
}

const Plan kThreeArgs{3,
                      1e200,
                      1e-200,
                      {1, 2, 3, 5, 8, 17, 64, 128, 129, 600, 1000},
                      12,
                      {{3, 5, 1},
                       {1, 4, 2},
                       {2, 2, 3},
                       {0, 3, 3},
                       {0, 1, 1},
                       {1, 1, 0},
                       {0, 0, 0}},
                      {0.5, 0.1, 1.3},
                      {0u, 7u, 0x87u, 0x80u, 0x85u},
                      {7u, 5u, 0u},
                      20000,
                      0.6,
                      {0u, 7u, 0x87u, 0x80u, 0x85u, 0x02u}};

const Plan kFourArgs{4,
                     1e100,
                     1e-100,
                     {1, 2, 3, 5, 8, 17, 64, 600, 1000},
                     8,
                     {{3, 1, 5, 1},
                      {1, 4, 2, 1},
                      {2, 2, 3, 3},
                      {0, 1, 3, 3},
                      {0, 1, 1, 1},
                      {1, 1, 0, 1},
                      {1, 0, 1, 1},
                      {1, 1, 1, 0},
                      {0, 0, 0, 0}},
                     {0.5, 3.5, 0.1, 1.3},
                     {0u, 15u, 0x8fu, 0x80u, 0x85u},
                     {15u, 9u, 0u},
                     40000,
                     0.5,
                     {0u, 15u, 0x8fu, 0x80u, 0x85u, 0x04u}};

using stanli::dens::beta_lpdf_fwd_gen;
using stanli::dens::cauchy_lpdf_fwd_gen;
using stanli::dens::gamma_lpdf_fwd_gen;
using stanli::dens::lognormal_lpdf_fwd_gen;
using stanli::dens::normal_lpdf_fwd_gen;
using stanli::dens::student_t_lpdf_fwd_gen;

const Spec kSpecs[] = {
    {"normal_lpdf",
     normal_lpdf_fwd_gen,
     &kThreeArgs,
     20251004,
     2,
     {Kind::real, Kind::real, Kind::positive},
     {},
     1,
     listed_bad_value,
     {{kNaN}, {kInf, kNaN}, {0.0, -1.5, kNaN, kInf}}},
    {"cauchy_lpdf",
     cauchy_lpdf_fwd_gen,
     &kThreeArgs,
     0,
     2,
     {Kind::real, Kind::real, Kind::positive},
     {},
     1,
     listed_bad_value,
     {{kNaN}, {kInf, kNaN}, {0.0, -1.5, kNaN, kInf}}},
    {"lognormal_lpdf",
     lognormal_lpdf_fwd_gen,
     &kThreeArgs,
     0,
     0,
     {Kind::nonneg, Kind::real, Kind::positive},
     {{0.0, -0.0, 1.0, kInf, 1e-300}},
     -1,
     listed_bad_value,
     {{kNaN, -1.5, -1e-300, -kInf},
      {kInf, kNaN, -kInf},
      {0.0, -1.5, kNaN, kInf}}},
    {"beta_lpdf",
     beta_lpdf_fwd_gen,
     &kThreeArgs,
     0,
     0,
     {Kind::unit, Kind::positive, Kind::positive},
     {{0.0, 1.0, 1e-300}, {1.0, 1e-300}, {1.0, 1e-300}},
     -1,
     listed_bad_value,
     {{kNaN, -0.1, 1.5, kInf},
      {0.0, -1.5, kNaN, kInf},
      {0.0, -1.5, kNaN, kInf}}},
    {"gamma_lpdf",
     gamma_lpdf_fwd_gen,
     &kThreeArgs,
     0,
     0,
     {Kind::positive, Kind::positive, Kind::positive},
     {{1e-300}, {1.0, 1e-300}, {1.0, 1e-300}},
     -1,
     listed_bad_value,
     {{kNaN, 0.0, -1.5, kInf},
      {0.0, -1.5, kNaN, kInf},
      {0.0, -1.5, kNaN, kInf}}},
    {"student_t_lpdf",
     student_t_lpdf_fwd_gen,
     &kFourArgs,
     20251005,
     0,
     {Kind::real, Kind::positive, Kind::real, Kind::positive},
     {},
     2,
     student_t_bad_value,
     {}},
};

struct Runner {
  const Spec& spec;
  std::mt19937_64& rng;
  Tally& tally;
  int arity() const { return spec.plan->arity; }
  double draw_arg(int k) {
    return draw(rng, spec.kind[k], spec.plan->big, spec.plan->tiny);
  }

  void one_case(const Args& args, unsigned variant, int64_t n_out) {
    const std::vector<double>* a[4] = {&args[0], &args[1], &args[2], &args[3]};
    const Outcome s = run(spec.forward, a, arity(), variant, n_out, false);
    const Outcome f = run(spec.forward, a, arity(), variant, n_out, true);
    compare(spec.name, tally, variant & ((1u << arity()) - 1), s, f,
            spec.max_ulp, [&] {
              std::printf("variant=0x%x sizes", variant);
              for (int k = 0; k < arity(); ++k)
                std::printf(" %zu", args[k].size());
            });
  }

  void fill(Args& args, const bool* vec, size_t N) {
    for (int j = 0; j < arity(); ++j) {
      args[j].resize(vec[j] ? N : 1);
      for (auto& x : args[j]) x = draw_arg(j);
    }
  }

  // Random draws with edge values, an occasional y equal to a location, and
  // an occasional bad value, over every mask, propto, shape and size.
  void random_cases() {
    std::uniform_real_distribution<double> u(0, 1);
    const Plan& plan = *spec.plan;
    const unsigned all = 1u << arity();
    for (unsigned mask = 0; mask < all; ++mask) {
      for (int propto = 0; propto < 2; ++propto) {
        for (unsigned shape = 0; shape < all; ++shape) {
          for (int n : plan.sizes) {
            for (int rep = 0; rep < plan.reps; ++rep) {
              g_offset = rep & 1;
              for (int elt = 0; elt < 2; ++elt) {
                bool vec[4], any_vec = false;
                for (int k = 0; k < 4; ++k) {
                  vec[k] = k < arity() && (shape & (1u << k)) != 0;
                  any_vec = any_vec || vec[k];
                }
                const size_t N = static_cast<size_t>(n);
                if (!any_vec && N != 1) continue;
                if (n == 1 && any_vec) continue;
                Args args;
                for (int k = 0; k < arity(); ++k) {
                  args[k].resize(vec[k] ? N : 1);
                  for (auto& x : args[k]) x = draw_arg(k);
                  for (const auto& e : spec.edge[k])
                    if (u(rng) < 0.04)
                      args[k][static_cast<size_t>(u(rng) * args[k].size())] = e;
                }
                if (spec.copy_into_y >= 0 && u(rng) < 0.15)
                  args[0][0] = args[spec.copy_into_y][0];
                spec.perturb(spec, rng, args);
                unsigned variant = mask | (propto ? 0x80u : 0u);
                int64_t n_out = 1;
                if (elt) {
                  if (N == 1) continue;
                  variant |= 0x40u;
                  n_out = static_cast<int64_t>(N);
                }
                one_case(args, variant, n_out);
              }
            }
          }
        }
      }
    }
  }

  void fixed_sizes() {
    for (const auto& l : spec.plan->lens) {
      Args args;
      for (int k = 0; k < arity(); ++k)
        args[k].assign(l[k], spec.plan->fill[k]);
      for (unsigned variant : spec.plan->fixed_variants)
        one_case(args, variant, 1);
    }
  }

  // A special value (NaN, infinities, zeros, negatives, extremes) at the
  // first, middle and last index of each argument in turn, in every shape,
  // size, activity mask, propto setting and both output forms. Whatever the
  // Stan Math path accepts or rejects, and with which message, the fused path
  // must too.
  void systematic_single() {
    const int sizes[] = {2, 3, 7, 8, 9, 16, 17, 33, 129};
    const unsigned all = 1u << arity();
    for (int k = 0; k < arity(); ++k) {
      for (double special : kSpecial) {
        for (unsigned shape = 1; shape < all; ++shape) {
          bool vec[4];
          for (int j = 0; j < 4; ++j) vec[j] = (shape & (1u << j)) != 0;
          for (int n : sizes) {
            const size_t N = static_cast<size_t>(n);
            const size_t positions[] = {0, N / 2, N - 1};
            for (size_t pos : positions) {
              if (!vec[k] && pos != 0) continue;
              for (int off = 0; off < 2; ++off) {
                g_offset = off;
                Args args;
                fill(args, vec, N);
                args[k][pos] = special;
                for (unsigned mask : spec.plan->single_masks)
                  for (int propto = 0; propto < 2; ++propto)
                    for (int elt = 0; elt < 2; ++elt) {
                      unsigned variant = mask | (propto ? 0x80u : 0u);
                      int64_t n_out = 1;
                      if (elt) {
                        variant |= 0x40u;
                        n_out = static_cast<int64_t>(N);
                      }
                      one_case(args, variant, n_out);
                    }
              }
            }
          }
        }
      }
    }
    Args args;
    for (int k = 0; k < arity(); ++k) {
      for (double special : kSpecial) {
        for (int j = 0; j < arity(); ++j) args[j].assign(1, draw_arg(j));
        args[k][0] = special;
        for (unsigned mask : spec.plan->single_masks)
          for (int propto = 0; propto < 2; ++propto)
            one_case(args, mask | (propto ? 0x80u : 0u), 1);
      }
    }
  }

  // Several bad values at once, in different arguments, so that the order of
  // the checks decides which message comes out.
  void systematic_multiple() {
    std::uniform_real_distribution<double> u(0, 1);
    const int sizes[] = {2, 5, 8, 9, 17, 33, 129};
    const int shapes = (1 << arity()) - 1;
    const unsigned masks = 1u << arity();
    for (int rep = 0; rep < spec.plan->multiple_reps; ++rep) {
      g_offset = rep & 1;
      const int shape = 1 + static_cast<int>(u(rng) * shapes) % shapes;
      bool vec[4];
      for (int j = 0; j < 4; ++j) vec[j] = (shape & (1 << j)) != 0;
      const size_t N =
          static_cast<size_t>(sizes[static_cast<size_t>(u(rng) * 7) % 7]);
      Args args;
      for (int j = 0; j < arity(); ++j) {
        args[j].resize(vec[j] ? N : 1);
        for (auto& x : args[j]) x = draw_arg(j);
        for (int t = 0; t < 2; ++t)
          if (u(rng) < (t == 0 ? spec.plan->multiple_first_p : 0.2))
            args[j][static_cast<size_t>(u(rng) * args[j].size())] =
                kSpecial[static_cast<size_t>(u(rng) * 10) % 10];
      }
      unsigned variant = static_cast<unsigned>(u(rng) * masks) % masks |
                         (u(rng) < 0.5 ? 0x80u : 0u);
      int64_t n_out = 1;
      if (u(rng) < 0.3) {
        variant |= 0x40u;
        n_out = static_cast<int64_t>(N);
      }
      one_case(args, variant, n_out);
    }
  }

  // Sizes of zero, one and several in every combination, clean and with a
  // special value in one argument.
  void systematic_sizes() {
    std::uniform_real_distribution<double> u(0, 1);
    const size_t sizes[] = {0, 1, 2, 5};
    int combos = 1;
    for (int k = 0; k < arity(); ++k) combos *= 4;
    for (int c = 0; c < combos; ++c) {
      size_t lens[4];
      for (int k = arity() - 1, rest = c; k >= 0; --k, rest /= 4)
        lens[k] = sizes[rest % 4];
      for (int bad = -1; bad < arity(); ++bad)
        for (unsigned variant : spec.plan->size_variants) {
          Args args;
          for (int j = 0; j < arity(); ++j) {
            args[j].resize(lens[j]);
            for (auto& x : args[j]) x = draw_arg(j);
          }
          if (bad >= 0 && !args[bad].empty())
            args[bad][static_cast<size_t>(u(rng) * args[bad].size())] =
                kSpecial[static_cast<size_t>(u(rng) * 10) % 10];
          one_case(args, variant, 1);
        }
    }
  }
};

Outcome run_ordered_logistic(const std::vector<int>& y,
                             const std::vector<double>& lambda,
                             const std::vector<double>& cuts, unsigned variant,
                             bool fused) {
  stanli::dens::set_fused_density(fused);
  std::vector<int> idata(y);
  idata.push_back(stanli::kVectorizedDensityLayoutMarker);
  idata.push_back(static_cast<int>(cuts.size()));
  idata.push_back(-1);
  std::vector<double> lam(lambda), c(cuts);
  Outcome o;
  o.scratch.assign(lam.size() + c.size() + 1, kSentinel);
  o.out.assign(1, kSentinel);
  stanli::KernelCtx ctx;
  ctx.in[0] = {lam.data(), static_cast<int64_t>(lam.size())};
  ctx.in[1] = {c.data(), static_cast<int64_t>(c.size())};
  ctx.n_in = 2;
  ctx.out = {o.out.data(), 1};
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
  return o;
}

// A shared cutpoint vector with scalar and vector locations, cutpoint counts
// from 0 to 10, both propto settings, edge values and rejections. Bitwise.
void ordered_logistic_cases(Tally& tally) {
  std::mt19937_64 rng(20251005);
  std::uniform_real_distribution<double> u(0, 1);
  std::normal_distribution<double> nd(0, 2);
  const int cut_counts[] = {0, 1, 2, 3, 4, 6, 10};
  const int sizes[] = {1, 2, 3, 5, 8, 17, 64, 129, 600};
  auto one_case = [&](const std::vector<int>& y,
                      const std::vector<double>& lambda,
                      const std::vector<double>& cuts, unsigned variant) {
    const Outcome s = run_ordered_logistic(y, lambda, cuts, variant, false);
    const Outcome f = run_ordered_logistic(y, lambda, cuts, variant, true);
    compare("ordered_logistic_lpmf", tally, variant & 1u, s, f, 0, [&] {
      std::printf("n=%zu cuts=%zu variant=0x%x", y.size(), cuts.size(),
                  variant);
    });
  };

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
          cuts[0] = kNaN;
        else if (bad < 0.17 && !cuts.empty())
          cuts[0] = -kInf;
        else if (bad < 0.19 && !cuts.empty())
          cuts.back() = kInf;
        else if (bad < 0.21)
          lambda[0] = kInf;
        else if (bad < 0.23)
          lambda[0] = kNaN;
        else if (bad < 0.25 && n > 2)
          lambda.pop_back();
        for (unsigned variant : {0u, 0x80u, 0x87u})
          one_case(y, lambda, cuts, variant);
      }
    }
  }

  const std::vector<std::vector<double>> cut_sets{{}, {0.5}, {-1, 2}};
  for (const auto& cs : cut_sets)
    for (unsigned variant : {0u, 0x80u}) one_case({}, {}, cs, variant);
}

void print_tally(const char* name, const Tally& tally, int masks) {
  std::printf("%s max ULP by mask:", name);
  for (int m = 0; m < masks; ++m) std::printf(" %g", tally.worst_by_mask[m]);
  std::printf("\n%s: %ld cases, %ld rejected, max ULP %g\n", name, tally.cases,
              tally.rejected, tally.worst);
}

template <class Run>
void check_oracle_switch(const char* name, Run run_probe) {
  const size_t before = stanli::dens::fused_density_calls();
  run_probe(true);
  if (stanli::dens::fused_density_calls() != before + 1) {
    ++failures;
    std::printf("FAIL %s: fused path not taken\n", name);
  }
  const size_t mid = stanli::dens::fused_density_calls();
  run_probe(false);
  if (stanli::dens::fused_density_calls() != mid) {
    ++failures;
    std::printf("FAIL %s: oracle switch does not select the Stan Math path\n",
                name);
  }
}

}  // namespace

int main() {
  std::mt19937_64 rng;
  for (const Spec& spec : kSpecs) {
    if (spec.seed) rng.seed(spec.seed);
    Tally tally;
    Runner r{spec, rng, tally};
    r.random_cases();
    r.fixed_sizes();
    r.systematic_single();
    r.systematic_multiple();
    r.systematic_sizes();
    print_tally(spec.name, tally, 1 << spec.plan->arity);
  }
  Tally ordered;
  ordered_logistic_cases(ordered);
  print_tally("ordered_logistic_lpmf", ordered, 1);

  for (const Spec& spec : kSpecs) {
    const std::vector<double> y{0.5, 0.25, 0.75}, a{0.25}, b{1.5};
    const std::vector<double> ty{0.5, -1.0, 2.0}, nu{4.0};
    const std::vector<double>* three[] = {&y, &a, &b};
    const std::vector<double>* four[] = {&ty, &nu, &a, &b};
    const bool student = spec.plan->arity == 4;
    check_oracle_switch(spec.name, [&](bool fused) {
      run(spec.forward, student ? four : three, spec.plan->arity,
          student ? 0x8fu : 0x87u, 1, fused);
    });
  }
  check_oracle_switch("ordered_logistic_lpmf", [](bool fused) {
    run_ordered_logistic({2, 1, 3}, {0.5, -1.0, 2.0}, {-0.5, 0.75}, 0x87,
                         fused);
  });
  stanli::dens::set_fused_density(true);
  return finish("test_density_fused");
}
