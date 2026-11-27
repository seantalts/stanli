// Calls every BLAS and LAPACK routine that an accelerated kernel can reach,
// in the variants the kernels use, many times with Accelerate's own threads
// allowed, and compares each result with the single-threaded result of the
// same call in the same process. The input is G G^T / n + I, whose condition
// number is about 5, so any difference above rounding is a wrong result.
//
//   stress_blas_threads [--sizes 64,151,512] [--calls N] [--only NAMES]
//   stress_blas_threads --kernel-shapes [--scale X] [--max-n N] [--only NAMES]
//   stress_blas_threads --sweep [--scale X] [--max-n N] [--only NAMES]
//   stress_blas_threads --tri-range LO HI [--tri-step S] [--scale X]
//
// --calls (default 20000) is the number of threaded calls per routine,
// spread over the sizes with fewer calls at the larger sizes. Prints one line
// per routine and size: calls, how many differed, the largest normwise and
// elementwise relative differences, a class (identical, rounding at most 1e-12
// normwise, or WRONG), and how many threaded calls differed from the first
// threaded one.
//
// --kernel-shapes instead makes the calls that runtime/src/dense_blas.cpp
// makes for each accelerated kernel, with the kernels' sides, transposes,
// alpha, beta and leading dimensions, at rectangular and square shapes from
// the selection thresholds up to n = 4096 (--max-n lowers that). The
// backward pass of the Cholesky factor is replayed block by block as
// chol_blas_bwd runs it, on views into an n x n matrix with leading
// dimension n, and each of its calls is compared with its single-threaded
// result on the same inputs. --scale multiplies the number of calls. It
// prints one line per routine and shape, with the CPU time over the wall
// time of the threaded calls, which shows whether Accelerate used threads.
//
// --sweep makes the same calls at every size of the square kernels from
// their thresholds to n = 1200 and on coarser grids above, and at a grid of
// product and matrix-vector shapes, with fewer calls each. It prints each
// shape whose threaded result differs, then one line per routine.
//
// --tri-range makes multi_normal_cholesky's dtrtri and dtrmv calls at every
// n from LO to HI (or every S), in the same form as --sweep.
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

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
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

// ---- --kernel-shapes ------------------------------------------------------

double cpu_now() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return double(u.ru_utime.tv_sec) + 1e-6 * double(u.ru_utime.tv_usec) +
         double(u.ru_stime.tv_sec) + 1e-6 * double(u.ru_stime.tv_usec);
}
double wall_now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A rows x cols block of a column-major matrix with leading dimension ld.
struct Region {
  double* p;
  int rows, cols, ld;
  Vec get() const {
    Vec v(size_t(rows) * cols);
    for (int c = 0; c < cols; ++c)
      std::memcpy(&v[size_t(c) * rows], p + size_t(c) * ld,
                  sizeof(double) * rows);
    return v;
  }
  void put(const Vec& v) const {
    for (int c = 0; c < cols; ++c)
      std::memcpy(p + size_t(c) * ld, &v[size_t(c) * rows],
                  sizeof(double) * rows);
  }
};

struct Stat {
  long calls = 0, differ = 0, unlike_first = 0;
  double worst_norm = 0, worst_elt = 0, cpu = 0, wall = 0;
};

struct Checker {
  double scale = 1;
  std::string only;
  // In a sweep, one line per routine, and each shape that differs printed
  // when it is found.
  bool sweep = false;
  std::vector<std::pair<std::string, Stat>> lines;
  std::map<std::string, size_t> index;
  std::map<std::string, Stat> per_routine;

  bool wanted(const std::string& routine) const {
    return only.empty() || only.find(routine) != std::string::npos;
  }
  // Repeats fall with the work of the call: about 300 for a call of 1e5
  // flops and at least 3 for the largest.
  long repeats(double flops) const {
    const double r = scale * 300.0 * std::pow(1e5 / std::max(flops, 1e5), 0.45);
    return std::max<long>(3, std::lround(r));
  }

  // Runs f once on one thread and then `repeats` times with Accelerate's
  // threads, each time from the same contents of `out`, and compares the
  // threaded results bit for bit with the single-threaded one. Leaves the
  // last threaded result in `out`.
  template <class F>
  void check(const std::string& routine, const std::string& shape, Region out,
             double flops, F&& f) {
    if (!wanted(routine)) {
      f();
      return;
    }
    const long reps = repeats(flops);
    const Vec saved = out.get();
    BLASSetThreading(BLAS_THREADING_SINGLE_THREADED);
    f();
    const Vec ref = out.get();
    double ref_max = 0;
    for (double v : ref) ref_max = std::max(ref_max, std::fabs(v));
    BLASSetThreading(BLAS_THREADING_MULTI_THREADED);
    const std::string key = sweep ? routine : routine + " " + shape;
    auto it = index.find(key);
    if (it == index.end()) {
      it = index.emplace(key, lines.size()).first;
      lines.push_back({key, Stat{}});
    }
    Stat& s = lines[it->second].second;
    Stat& total = per_routine[routine];
    const long differ_before = s.differ;
    Vec first;
    for (long r = 0; r < reps; ++r) {
      out.put(saved);
      const double c0 = cpu_now(), w0 = wall_now();
      f();
      const double dc = cpu_now() - c0, dw = wall_now() - w0;
      const Vec got = out.get();
      if (r == 0)
        first = got;
      else if (got != first)
        ++s.unlike_first;
      double d = 0, e = 0;
      for (size_t i = 0; i < got.size(); ++i) {
        const double diff = std::fabs(got[i] - ref[i]);
        if (std::isnan(got[i]) != std::isnan(ref[i])) d = INFINITY;
        d = std::max(d, diff);
        if (ref[i] != 0)
          e = std::max(e, diff / std::fabs(ref[i]));
        else if (diff != 0)
          e = INFINITY;
      }
      const bool differs =
          std::memcmp(got.data(), ref.data(), sizeof(double) * got.size()) != 0;
      for (Stat* t : {&s, &total}) {
        ++t->calls;
        t->differ += differs;
        t->worst_norm = std::max(t->worst_norm, ref_max > 0 ? d / ref_max : d);
        t->worst_elt = std::max(t->worst_elt, e);
        t->cpu += dc;
        t->wall += dw;
      }
    }
    if (sweep && s.differ > differ_before) {
      std::printf("DIFF %s %s: %ld of %ld calls\n", routine.c_str(),
                  shape.c_str(), s.differ - differ_before, reps);
      std::fflush(stdout);
    }
  }
};

const char* classify(const Stat& s) {
  return s.differ == 0           ? "identical"
         : s.worst_norm <= 1e-12 ? "rounding"
                                 : "WRONG";
}

void print_stat(const std::string& key, const Stat& s) {
  std::printf(
      "%-48s calls %-6ld differ %-6ld norm %.3e elt %.3e %-9s "
      "unlike_first_threaded %-5ld cpu/wall %.2f\n",
      key.c_str(), s.calls, s.differ, s.worst_norm, s.worst_elt, classify(s),
      s.unlike_first, s.wall > 0 ? s.cpu / s.wall : 0.0);
  std::fflush(stdout);
}

Vec random_vec(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> normal;
  Vec v(n);
  for (double& x : v) x = normal(rng);
  return v;
}

// The lower Cholesky factor of G G^T / n + I, condition number about 5,
// with its upper triangle zero.
Vec lower_factor(int n) {
  const Vec g = random_vec(size_t(n) * n, 11 + n);
  Vec a(size_t(n) * n, 0.0);
  const double scale = 1.0 / n, zero = 0;
  BLASSetThreading(BLAS_THREADING_SINGLE_THREADED);
  dgemm_("N", "T", &n, &n, &n, &scale, g.data(), &n, g.data(), &n, &zero,
         a.data(), &n);
  for (int i = 0; i < n; ++i) a[i + size_t(i) * n] += 1;
  int info = 0;
  dpotrf_("L", &n, a.data(), &n, &info);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < j; ++i) a[i + size_t(j) * n] = 0;
  return a;
}
Vec spd(int n) {
  const Vec l = lower_factor(n);
  Vec a(size_t(n) * n, 0.0);
  const double one = 1, zero = 0;
  dgemm_("N", "T", &n, &n, &n, &one, l.data(), &n, l.data(), &n, &zero,
         a.data(), &n);
  return a;
}

std::string dims(std::initializer_list<int> d) {
  std::string s;
  for (int v : d) s += (s.empty() ? "" : "x") + std::to_string(v);
  return s;
}

// gemm_blas_fwd and gemm_blas_bwd for A (ra x ca) times B (ca x cb).
void gemm_cases(Checker& ck, int ra, int ca, int cb) {
  const Vec a = random_vec(size_t(ra) * ca, 1 + ra),
            b = random_vec(size_t(ca) * cb, 2 + cb),
            d = random_vec(size_t(ra) * cb, 3 + ca);
  const double one = 1, zero = 0;
  const std::string shape = dims({ra, ca, cb});
  const double flops = 2.0 * ra * ca * cb;
  Vec c(size_t(ra) * cb, 0.0);
  ck.check("dgemm_NN_a1_b0", shape, {c.data(), ra, cb, ra}, flops, [&] {
    dgemm_("N", "N", &ra, &cb, &ca, &one, a.data(), &ra, b.data(), &ca, &zero,
           c.data(), &ra);
  });
  Vec a_adj = random_vec(size_t(ra) * ca, 4 + ra);
  ck.check("dgemm_NT_a1_b1", shape, {a_adj.data(), ra, ca, ra}, flops, [&] {
    dgemm_("N", "T", &ra, &ca, &cb, &one, d.data(), &ra, b.data(), &ca, &one,
           a_adj.data(), &ra);
  });
  Vec b_adj = random_vec(size_t(ca) * cb, 5 + cb);
  ck.check("dgemm_TN_a1_b1", shape, {b_adj.data(), ca, cb, ca}, flops, [&] {
    dgemm_("T", "N", &ca, &cb, &ra, &one, a.data(), &ra, d.data(), &ra, &one,
           b_adj.data(), &ca);
  });
}

// matvec_blas_fwd and matvec_blas_bwd.
void matvec_cases(Checker& ck, int rows, int cols) {
  const Vec x = random_vec(size_t(rows) * cols, 6 + rows),
            beta = random_vec(cols, 7), adj = random_vec(rows, 8);
  const double one = 1, zero = 0;
  const int inc = 1;
  const std::string shape = dims({rows, cols});
  const double flops = 2.0 * rows * cols;
  Vec y(rows, 0.0);
  ck.check("dgemv_N_a1_b0", shape, {y.data(), rows, 1, rows}, flops, [&] {
    dgemv_("N", &rows, &cols, &one, x.data(), &rows, beta.data(), &inc, &zero,
           y.data(), &inc);
  });
  Vec beta_adj = random_vec(cols, 9);
  ck.check("dgemv_T_a1_b1", shape, {beta_adj.data(), cols, 1, cols}, flops,
           [&] {
             dgemv_("T", &rows, &cols, &one, x.data(), &rows, adj.data(), &inc,
                    &one, beta_adj.data(), &inc);
           });
}

// nid_glm_blas_value: X * beta and X' * mu_derivative, both with beta 0.
void glm_cases(Checker& ck, int rows, int cols) {
  const Vec x = random_vec(size_t(rows) * cols, 10 + rows),
            beta = random_vec(cols, 11), mu = random_vec(rows, 12);
  const double one = 1, zero = 0;
  const int inc = 1;
  const std::string shape = dims({rows, cols});
  const double flops = 2.0 * rows * cols;
  Vec y(rows, 0.0);
  ck.check("dgemv_N_a1_b0", shape, {y.data(), rows, 1, rows}, flops, [&] {
    dgemv_("N", &rows, &cols, &one, x.data(), &rows, beta.data(), &inc, &zero,
           y.data(), &inc);
  });
  Vec d_beta(cols, 0.0);
  ck.check("dgemv_T_a1_b0", shape, {d_beta.data(), cols, 1, cols}, flops, [&] {
    dgemv_("T", &rows, &cols, &one, x.data(), &rows, mu.data(), &inc, &zero,
           d_beta.data(), &inc);
  });
}

// mn_blas_fwd_value: dpotrf, dpotri and the square dgemv of the inverse.
void mn_cases(Checker& ck, int n) {
  const Vec sigma = spd(n), diff = random_vec(n, 13);
  const double one = 1, zero = 0;
  const int inc = 1;
  const std::string shape = dims({n});
  const double cube = double(n) * n * n;
  Vec a = sigma;
  int info = 0;
  ck.check("dpotrf_L", shape, {a.data(), n, n, n}, cube / 3,
           [&] { dpotrf_("L", &n, a.data(), &n, &info); });
  ck.check("dpotri_L", shape, {a.data(), n, n, n}, 2 * cube / 3,
           [&] { dpotri_("L", &n, a.data(), &n, &info); });
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < j; ++i) a[i + size_t(j) * n] = a[j + size_t(i) * n];
  Vec half(n, 0.0);
  ck.check("dgemv_N_a1_b0", shape + "sq", {half.data(), n, 1, n}, 2.0 * n * n,
           [&] {
             dgemv_("N", &n, &n, &one, a.data(), &n, diff.data(), &inc, &zero,
                    half.data(), &inc);
           });
}

// mnc_blas_fwd_value: dtrtri of L, then dtrmv with the inverse, N and T.
void mnc_cases(Checker& ck, int n) {
  Vec inv = lower_factor(n);
  const Vec y = random_vec(n, 14);
  const std::string shape = dims({n});
  const int inc = 1;
  int info = 0;
  ck.check("dtrtri_L", shape, {inv.data(), n, n, n}, double(n) * n * n / 3,
           [&] { dtrtri_("L", "N", &n, inv.data(), &n, &info); });
  Vec half = y;
  ck.check("dtrmv_N", shape, {half.data(), n, 1, n}, double(n) * n, [&] {
    dtrmv_("L", "N", "N", &n, inv.data(), &n, half.data(), &inc);
  });
  Vec scaled = half;
  ck.check("dtrmv_T", shape, {scaled.data(), n, 1, n}, double(n) * n, [&] {
    dtrmv_("L", "T", "N", &n, inv.data(), &n, scaled.data(), &inc);
  });
}

// chol_blas_fwd's dpotrf, then chol_blas_bwd's blocked loop call for call,
// with the factor L and a random lower adjoint W.
void chol_cases(Checker& ck, int n) {
  Vec a = spd(n);
  int info = 0;
  const std::string shape = dims({n});
  ck.check("dpotrf_L", "chol " + shape, {a.data(), n, n, n},
           double(n) * n * n / 3,
           [&] { dpotrf_("L", &n, a.data(), &n, &info); });
  const Vec L = lower_factor(n);
  Vec W = random_vec(size_t(n) * n, 15 + n);
  for (int c = 0; c < n; ++c)
    for (int r = 0; r < c; ++r) W[r + size_t(c) * n] = 0;
  const auto at = [n](auto* base, int r, int c) {
    return base + r + size_t(c) * n;
  };
  const double one = 1, minus = -1;
  int block = std::max(n / 8, 8);
  block = std::min(block, 128);
  const std::string tag = "cholbwd n=" + std::to_string(n);
  for (int k = n; k > 0; k -= block) {
    const int j = std::max(0, k - block);
    const int nb = k - j, mb = n - k;
    const double* R = at(L.data(), j, 0);
    const double* D = at(L.data(), j, j);
    const double* B = at(L.data(), k, 0);
    const double* C = at(L.data(), k, j);
    double* R_adj = at(W.data(), j, 0);
    double* D_adj = at(W.data(), j, j);
    double* B_adj = at(W.data(), k, 0);
    double* C_adj = at(W.data(), k, j);
    if (mb > 0) {
      ck.check("dtrsm_R_L_N", tag, {C_adj, mb, nb, n}, double(mb) * nb * nb,
               [&] {
                 dtrsm_("R", "L", "N", "N", &mb, &nb, &one, D, &n, C_adj, &n);
               });
      if (j > 0)
        ck.check("dgemm_NN_am1_b1", tag, {B_adj, mb, j, n}, 2.0 * mb * j * nb,
                 [&] {
                   dgemm_("N", "N", &mb, &j, &nb, &minus, C_adj, &n, R, &n,
                          &one, B_adj, &n);
                 });
      ck.check("dgemm_TN_am1_b1", tag, {D_adj, nb, nb, n}, 2.0 * nb * nb * mb,
               [&] {
                 dgemm_("T", "N", &nb, &nb, &mb, &minus, C_adj, &n, C, &n, &one,
                        D_adj, &n);
               });
    }
    for (int c = 1; c < nb; ++c)
      for (int r = 0; r < c; ++r) D_adj[r + size_t(c) * n] = 0.0;
    ck.check("dtrmm_L_L_T", tag, {D_adj, nb, nb, n}, double(nb) * nb * nb, [&] {
      dtrmm_("L", "L", "T", "N", &nb, &nb, &one, D, &n, D_adj, &n);
    });
    for (int c = 1; c < nb; ++c)
      for (int r = 0; r < c; ++r)
        D_adj[r + size_t(c) * n] = D_adj[c + size_t(r) * n];
    ck.check("dtrsm_L_L_T", tag, {D_adj, nb, nb, n}, double(nb) * nb * nb, [&] {
      dtrsm_("L", "L", "T", "N", &nb, &nb, &one, D, &n, D_adj, &n);
    });
    ck.check("dtrsm_R_L_N", tag, {D_adj, nb, nb, n}, double(nb) * nb * nb, [&] {
      dtrsm_("R", "L", "N", "N", &nb, &nb, &one, D, &n, D_adj, &n);
    });
    if (j > 0 && mb > 0)
      ck.check("dgemm_TN_am1_b1", tag, {R_adj, nb, j, n}, 2.0 * nb * j * mb,
               [&] {
                 dgemm_("T", "N", &nb, &j, &mb, &minus, C_adj, &n, B, &n, &one,
                        R_adj, &n);
               });
    if (j > 0)
      ck.check("dsymm_L_L_am1_b1", tag, {R_adj, nb, j, n}, 2.0 * nb * nb * j,
               [&] {
                 dsymm_("L", "L", &nb, &j, &minus, D_adj, &n, R, &n, &one,
                        R_adj, &n);
               });
    for (int d = 0; d < nb; ++d) D_adj[d + size_t(d) * n] *= 0.5;
  }
}

int report(const Checker& ck) {
  for (const auto& [key, s] : ck.lines) print_stat(key, s);
  std::printf("per routine\n");
  int wrong = 0;
  for (const auto& [routine, s] : ck.per_routine) {
    print_stat("TOTAL " + routine, s);
    wrong += classify(s)[0] == 'W';
  }
  return wrong ? 1 : 0;
}

// Every size of the square kernels from its threshold to 1200, then a
// coarser grid to max_n, and a grid of products and matrix-vector shapes.
int sweep(double scale, int max_n, const std::string& only) {
  Checker ck;
  ck.scale = scale;
  ck.only = only;
  ck.sweep = true;
  std::vector<int> square;
  for (int n = 32; n <= std::min(max_n, 1200); ++n) square.push_back(n);
  for (int n = 1216; n <= std::min(max_n, 2048); n += 16) square.push_back(n);
  for (int n = 2112; n <= max_n; n += 64) square.push_back(n);
  for (int n : square) {
    mn_cases(ck, n);
    if (n >= 64) {
      mnc_cases(ck, n);
      chol_cases(ck, n);
    }
  }
  const int ds[] = {32,  64,  100,  128,  200,  256, 400,
                    512, 785, 1000, 1500, 2048, 4096};
  for (int ra : ds)
    for (int ca : ds)
      for (int cb : ds)
        if (std::max({ra, ca, cb}) <= max_n && double(ra) * ca * cb <= 2.2e9)
          gemm_cases(ck, ra, ca, cb);
  const int rows[] = {256, 300, 512, 1000, 2048, 4096, 10000, 30000, 100000};
  const int cols[] = {10, 16, 24, 64, 100, 256, 1000, 4096};
  for (int r : rows)
    for (int c : cols) {
      if (r < 10000 && c < 64) continue;
      if (double(r) * c > 5e8) continue;
      matvec_cases(ck, r, c);
      if (r >= 4096 && c >= 16) glm_cases(ck, r, c);
    }
  return report(ck);
}

// multi_normal_cholesky's dtrtri and dtrmv at every n from lo to hi in steps
// of step, on the leading n x n block of one factor of size hi, which is the
// factor of the leading block of its matrix.
int tri_range(double scale, int lo, int hi, int step) {
  Checker ck;
  ck.scale = scale;
  ck.sweep = true;
  const Vec full = lower_factor(hi);
  const int inc = 1;
  for (int n = lo; n <= hi; n += step) {
    Vec inv(size_t(n) * n);
    for (int c = 0; c < n; ++c)
      std::memcpy(&inv[size_t(c) * n], &full[size_t(c) * hi],
                  sizeof(double) * n);
    const Vec y = random_vec(n, 14);
    const std::string shape = dims({n});
    int info = 0;
    ck.check("dtrtri_L", shape, {inv.data(), n, n, n}, double(n) * n * n / 3,
             [&] { dtrtri_("L", "N", &n, inv.data(), &n, &info); });
    Vec half = y;
    ck.check("dtrmv_N", shape, {half.data(), n, 1, n}, double(n) * n, [&] {
      dtrmv_("L", "N", "N", &n, inv.data(), &n, half.data(), &inc);
    });
    Vec scaled = half;
    ck.check("dtrmv_T", shape, {scaled.data(), n, 1, n}, double(n) * n, [&] {
      dtrmv_("L", "T", "N", &n, inv.data(), &n, scaled.data(), &inc);
    });
  }
  return report(ck);
}

int kernel_shapes(double scale, int max_n, const std::string& only) {
  Checker ck;
  ck.scale = scale;
  ck.only = only;
  const auto fits = [max_n](std::initializer_list<int> d) {
    for (int v : d)
      if (v > max_n && v < 10000) return false;
    // The long sides of matrix-vector products and of nn_rbm1bJ100's
    // product stay at every --max-n.
    return true;
  };
  // The corpus models that reach the thresholds use gemm at 60000 x 785 x
  // 100, the Cholesky factor at n = 101, multi_normal at n = 151 and
  // normal_id_glm at 5000 x 24; each of these shapes is in the lists below.
  // All three dimensions at least gemm_min_dim = 32: square, nearly square
  // and strongly rectangular.
  const int gemm[][3] = {
      {32, 32, 32},
      {33, 37, 41},
      {32, 32, 4096},
      {32, 4096, 32},
      {4096, 32, 32},
      {40, 64, 1000},
      {64, 64, 64},
      {100, 50, 200},
      {151, 151, 151},
      {512, 128, 512},
      {128, 512, 128},
      {256, 1024, 64},
      {500, 784, 100},
      {100, 784, 500},
      {1000, 100, 1000},
      {1024, 1024, 1024},
      {2048, 64, 2048},
      {64, 2048, 64},
      {3000, 40, 1500},
      {4096, 512, 1024},
      {2048, 2048, 2048},
      {4096, 4096, 4096},
      // The product of the corpus model nn_rbm1bJ100.
      {60000, 785, 100},
  };
  for (const auto& g : gemm)
    if (fits({g[0], g[1], g[2]})) gemm_cases(ck, g[0], g[1], g[2]);
  // Matrix-vector products from both thresholds: rows >= 256 and
  // cols >= 64, or rows >= 10000 and cols >= 10.
  const int matvec[][2] = {
      {256, 64},   {257, 65},    {300, 1000},   {1000, 300}, {256, 4096},
      {4096, 256}, {4096, 4096}, {10000, 10},   {10000, 11}, {12345, 24},
      {50000, 10}, {20000, 64},  {100000, 100},
  };
  for (const auto& m : matvec)
    if (fits({m[0], m[1]})) matvec_cases(ck, m[0], m[1]);
  // normal_id_glm from 4096 rows and 16 columns.
  const int glm[][2] = {{4096, 16}, {5000, 24}, {20000, 64}, {100000, 16}};
  for (const auto& m : glm)
    if (fits({m[0], m[1]})) glm_cases(ck, m[0], m[1]);
  // multi_normal from n = 32; multi_normal_cholesky and the Cholesky
  // factor from n = 64.
  for (int n : {32, 48, 64, 100, 151, 256, 512, 1000, 1024, 2048, 4096})
    if (fits({n})) mn_cases(ck, n);
  for (int n : {64, 65, 100, 151, 256, 512, 1000, 1024, 2048, 4096})
    if (fits({n})) mnc_cases(ck, n);
  for (int n : {64, 65, 100, 101, 151, 200, 256, 512, 1000, 1024, 2048, 4096})
    if (fits({n})) chol_cases(ck, n);
  return report(ck);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<int> sizes = {64, 151, 512, 1024, 2048};
  long calls = 20000;
  std::string only;
  bool shapes = false, sweeping = false;
  int tri_lo = 0, tri_hi = 0, tri_step = 1;
  double scale = 1;
  int max_n = 4096;
  for (int i = 1; i < argc; ++i) {
    const bool has_value = i + 1 < argc;
    if (!std::strcmp(argv[i], "--kernel-shapes"))
      shapes = true;
    else if (!std::strcmp(argv[i], "--sweep"))
      sweeping = true;
    else if (has_value && !std::strcmp(argv[i], "--scale"))
      scale = std::atof(argv[++i]);
    else if (i + 2 < argc && !std::strcmp(argv[i], "--tri-range")) {
      tri_lo = std::atoi(argv[++i]);
      tri_hi = std::atoi(argv[++i]);
    } else if (has_value && !std::strcmp(argv[i], "--tri-step"))
      tri_step = std::max(1, std::atoi(argv[++i]));
    else if (has_value && !std::strcmp(argv[i], "--max-n"))
      max_n = std::atoi(argv[++i]);
    else if (has_value && !std::strcmp(argv[i], "--sizes"))
      sizes = parse_list(argv[++i]);
    else if (has_value && !std::strcmp(argv[i], "--calls"))
      calls = std::atol(argv[++i]);
    else if (has_value && !std::strcmp(argv[i], "--only"))
      only = argv[++i];
  }
  if (tri_hi > 0) return tri_range(scale, tri_lo, tri_hi, tri_step);
  if (sweeping) return sweep(scale, max_n, only);
  if (shapes) return kernel_shapes(scale, max_n, only);
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
