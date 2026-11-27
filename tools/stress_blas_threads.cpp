// Calls every BLAS and LAPACK routine that an accelerated kernel can reach,
// in the variants the kernels use, many times with Accelerate's own threads
// allowed, and compares each result with the single-threaded result of the
// same call in the same process. The input is G G^T / n + I, whose condition
// number is about 5, so any difference above rounding is a wrong result.
//
//   stress_blas_threads [--sizes 64,151,512] [--calls N] [--only NAMES]
//
// --calls (default 20000) is the number of threaded calls per routine,
// spread over the sizes with fewer calls at the larger sizes. Prints one line
// per routine and size: calls, how many differed, the largest normwise and
// elementwise relative differences, a class (identical, rounding at most 1e-12
// normwise, or WRONG), and how many threaded calls differed from the first
// threaded one.
//
// By default it binds each routine the way runtime/src/dense_blas.cpp does:
// dpotri from Accelerate's newer LAPACK interface (ACCELERATE_NEW_LAPACK),
// the others by their legacy Fortran symbols. Built from the same source
// with STRESS_NEW_LAPACK, it calls every routine from the newer interface.
// Either way dpotri_legacy_L and dlauum_legacy_L call the legacy dpotri_ and
// dlauum_ for comparison: with threads, these sometimes return a wrong
// result on macOS 26.6.
extern "C" {
void dpotri_legacy(const char*, const int*, double*, const int*,
                   int*) __asm("_dpotri_");
void dlauum_legacy(const char*, const int*, double*, const int*,
                   int*) __asm("_dlauum_");
void dlauum_new(const char*, const int*, double*, const int*,
                int*) __asm("_dlauum$NEWLAPACK");
}
#ifdef STRESS_NEW_LAPACK
#define ACCELERATE_NEW_LAPACK
#include <Accelerate/Accelerate.h>
#else
#include <vecLib/thread_api.h>
extern "C" {
void dgemm_(const char*, const char*, const int*, const int*, const int*,
            const double*, const double*, const int*, const double*, const int*,
            const double*, double*, const int*);
void dgemv_(const char*, const int*, const int*, const double*, const double*,
            const int*, const double*, const int*, const double*, double*,
            const int*);
void dpotrf_(const char*, const int*, double*, const int*, int*);
void dpotri_(const char*, const int*, double*, const int*,
             int*) __asm("_dpotri$NEWLAPACK");
void dtrtri_(const char*, const char*, const int*, double*, const int*, int*);
void dtrsm_(const char*, const char*, const char*, const char*, const int*,
            const int*, const double*, const double*, const int*, double*,
            const int*);
void dtrmm_(const char*, const char*, const char*, const char*, const int*,
            const int*, const double*, const double*, const int*, double*,
            const int*);
void dtrmv_(const char*, const char*, const char*, const int*, const double*,
            const int*, double*, const int*);
void dsymm_(const char*, const char*, const int*, const int*, const double*,
            const double*, const int*, const double*, const int*, const double*,
            double*, const int*);
}
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace {

using Vec = std::vector<double>;

struct Inputs {
  int n = 0;
  Vec spd, lower, lower_inverse, m1, m2, x;
};

Inputs make_inputs(int n) {
  Inputs in;
  in.n = n;
  std::mt19937_64 rng(7 + n);
  std::normal_distribution<double> normal;
  Vec g(size_t(n) * n);
  for (double& v : g) v = normal(rng);
  in.spd.assign(size_t(n) * n, 0.0);
  const double one = 1, zero = 0, scale = 1.0 / n;
  dgemm_("N", "T", &n, &n, &n, &scale, g.data(), &n, g.data(), &n, &zero,
         in.spd.data(), &n);
  for (int i = 0; i < n; ++i) in.spd[i + size_t(i) * n] += one;
  in.lower = in.spd;
  int info = 0;
  dpotrf_("L", &n, in.lower.data(), &n, &info);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < j; ++i) in.lower[i + size_t(j) * n] = 0;
  in.lower_inverse = in.lower;
  dtrtri_("L", "N", &n, in.lower_inverse.data(), &n, &info);
  in.m1.resize(size_t(n) * n);
  in.m2.resize(size_t(n) * n);
  in.x.resize(n);
  for (double& v : in.m1) v = normal(rng);
  for (double& v : in.m2) v = normal(rng);
  for (double& v : in.x) v = normal(rng);
  return in;
}

using Call = std::function<Vec(const Inputs&)>;

struct Routine {
  const char* name;
  Call call;
};

std::vector<Routine> routines() {
  const double one = 1, minus = -1, zero = 0;
  static const int inc = 1;
  return {
      {"dgemm_NN_beta0",
       [=](const Inputs& in) {
         Vec c(in.m1.size());
         dgemm_("N", "N", &in.n, &in.n, &in.n, &one, in.m1.data(), &in.n,
                in.m2.data(), &in.n, &zero, c.data(), &in.n);
         return c;
       }},
      {"dgemm_NT_beta1",
       [=](const Inputs& in) {
         Vec c = in.spd;
         dgemm_("N", "T", &in.n, &in.n, &in.n, &one, in.m1.data(), &in.n,
                in.m2.data(), &in.n, &one, c.data(), &in.n);
         return c;
       }},
      {"dgemm_TN_minus1_beta1",
       [=](const Inputs& in) {
         Vec c = in.spd;
         dgemm_("T", "N", &in.n, &in.n, &in.n, &minus, in.m1.data(), &in.n,
                in.m2.data(), &in.n, &one, c.data(), &in.n);
         return c;
       }},
      {"dgemv_N",
       [=](const Inputs& in) {
         Vec y(in.n);
         dgemv_("N", &in.n, &in.n, &one, in.m1.data(), &in.n, in.x.data(), &inc,
                &zero, y.data(), &inc);
         return y;
       }},
      {"dgemv_T",
       [=](const Inputs& in) {
         Vec y(in.n);
         dgemv_("T", &in.n, &in.n, &one, in.m1.data(), &in.n, in.x.data(), &inc,
                &zero, y.data(), &inc);
         return y;
       }},
      {"dpotrf_L",
       [=](const Inputs& in) {
         Vec a = in.spd;
         int info = 0;
         dpotrf_("L", &in.n, a.data(), &in.n, &info);
         return a;
       }},
      {"dtrtri_L",
       [=](const Inputs& in) {
         Vec a = in.lower;
         int info = 0;
         dtrtri_("L", "N", &in.n, a.data(), &in.n, &info);
         return a;
       }},
      {"dpotri_L",
       [=](const Inputs& in) {
         Vec a = in.lower;
         int info = 0;
         dpotri_("L", &in.n, a.data(), &in.n, &info);
         return a;
       }},
      {"dlauum_L",
       [=](const Inputs& in) {
         Vec a = in.lower_inverse;
         int info = 0;
         dlauum_new("L", &in.n, a.data(), &in.n, &info);
         return a;
       }},
      {"dpotri_legacy_L",
       [=](const Inputs& in) {
         Vec a = in.lower;
         int info = 0;
         dpotri_legacy("L", &in.n, a.data(), &in.n, &info);
         return a;
       }},
      {"dlauum_legacy_L",
       [=](const Inputs& in) {
         Vec a = in.lower_inverse;
         int info = 0;
         dlauum_legacy("L", &in.n, a.data(), &in.n, &info);
         return a;
       }},
      {"dtrsm_right_N",
       [=](const Inputs& in) {
         Vec b = in.m1;
         dtrsm_("R", "L", "N", "N", &in.n, &in.n, &one, in.lower.data(), &in.n,
                b.data(), &in.n);
         return b;
       }},
      {"dtrsm_left_T",
       [=](const Inputs& in) {
         Vec b = in.m1;
         dtrsm_("L", "L", "T", "N", &in.n, &in.n, &one, in.lower.data(), &in.n,
                b.data(), &in.n);
         return b;
       }},
      {"dtrmm_left_T",
       [=](const Inputs& in) {
         Vec b = in.m1;
         dtrmm_("L", "L", "T", "N", &in.n, &in.n, &one, in.lower.data(), &in.n,
                b.data(), &in.n);
         return b;
       }},
      {"dtrmv_N",
       [=](const Inputs& in) {
         Vec x = in.x;
         dtrmv_("L", "N", "N", &in.n, in.lower.data(), &in.n, x.data(), &inc);
         return x;
       }},
      {"dtrmv_T",
       [=](const Inputs& in) {
         Vec x = in.x;
         dtrmv_("L", "T", "N", &in.n, in.lower.data(), &in.n, x.data(), &inc);
         return x;
       }},
      {"dsymm_left_L",
       [=](const Inputs& in) {
         Vec c = in.m2;
         dsymm_("L", "L", &in.n, &in.n, &minus, in.spd.data(), &in.n,
                in.m1.data(), &in.n, &one, c.data(), &in.n);
         return c;
       }},
  };
}

std::vector<int> parse_list(const char* s) {
  std::vector<int> out;
  for (const char* p = s; *p;) {
    out.push_back(std::atoi(p));
    p = std::strchr(p, ',');
    if (!p) break;
    ++p;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<int> sizes = {64, 151, 512, 1024, 2048};
  long calls = 20000;
  std::string only;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--sizes"))
      sizes = parse_list(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--calls"))
      calls = std::atol(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--only"))
      only = argv[i + 1];
  }
  // Calls per size fall as n^-0.75: with the default sizes, about 52% of
  // them at n = 64 and 4% at n = 2048.
  double weight_sum = 0;
  for (int n : sizes) weight_sum += std::pow(double(n), -0.75);
  std::vector<long> per_size;
  for (int n : sizes)
    per_size.push_back(std::max<long>(
        std::lround(calls * std::pow(double(n), -0.75) / weight_sum), 1));

  const std::vector<Routine> all = routines();
  int wrong = 0;
  for (size_t k = 0; k < sizes.size(); ++k) {
    const Inputs in = make_inputs(sizes[k]);
    for (const Routine& r : all) {
      if (!only.empty() && only.find(r.name) == std::string::npos) continue;
      BLASSetThreading(BLAS_THREADING_SINGLE_THREADED);
      const Vec ref = r.call(in);
      BLASSetThreading(BLAS_THREADING_MULTI_THREADED);
      double ref_max = 0;
      for (double v : ref) ref_max = std::max(ref_max, std::fabs(v));
      long differ = 0, unlike_first = 0;
      double worst_norm = 0, worst_elt = 0;
      Vec first;
      for (long c = 0; c < per_size[k]; ++c) {
        const Vec got = r.call(in);
        if (c == 0)
          first = got;
        else if (got != first)
          ++unlike_first;
        double d = 0, e = 0;
        for (size_t i = 0; i < got.size(); ++i) {
          const double diff = std::fabs(got[i] - ref[i]);
          d = std::max(d, diff);
          if (ref[i] != 0)
            e = std::max(e, diff / std::fabs(ref[i]));
          else if (diff != 0)
            e = INFINITY;
        }
        if (d > 0 || std::isnan(d)) ++differ;
        worst_norm = std::max(worst_norm, d / ref_max);
        worst_elt = std::max(worst_elt, e);
      }
      const char* cls = differ == 0           ? "identical"
                        : worst_norm <= 1e-12 ? "rounding"
                                              : "WRONG";
      if (cls[0] == 'W') ++wrong;
      std::printf(
          "%-22s n=%-5d calls %-6ld differ %-6ld norm %.3e elt %.3e %-9s "
          "unlike_first_threaded %ld\n",
          r.name, sizes[k], per_size[k], differ, worst_norm, worst_elt, cls,
          unlike_first);
      std::fflush(stdout);
    }
  }
  return wrong ? 1 : 0;
}
