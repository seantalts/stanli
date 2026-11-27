// Same model, data and point, same bits, on a busy machine. A Gaussian
// process likelihood with a 151 x 151 covariance takes its gradient in 144
// separate processes, 24 at a time, the way a corpus replay or several
// sampling jobs run side by side, and every process must print the same
// bits. With STANLI_ACCELERATE the covariance goes through the accelerated
// multi_normal kernel and its dpotri. Accelerate's legacy dpotri_ sometimes
// returned a wrong inverse with threads of its own, and both tests failed
// with it; the kernel now calls dpotri from the newer LAPACK interface.
//
// Run with --child to print one gradient's hash.
//
// With --parallel-blas, the same model at N = 151, 512 and 1100 with the
// parallel_blas option on, so that Accelerate may use its own threads in
// dpotrf, dpotri and dgemv, must agree with the option off to rounding level
// in every one of 400 gradients in one process (kParallelBlasTolerance).
#include <stanli/capi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kModel = R"(
data {
  int<lower=1> N;
  matrix[N, N] D;
  vector[N] y;
}
parameters {
  real a;
  real<lower=0> etasq;
  real<lower=0> rhosq;
}
model {
  matrix[N, N] K;
  for (i in 1:(N - 1)) {
    K[i, i] = etasq + 0.01;
    for (j in (i + 1):N) {
      K[i, j] = etasq * exp(-rhosq * D[i, j]);
      K[j, i] = K[i, j];
    }
  }
  K[N, N] = etasq + 0.01;
  rhosq ~ normal(3, 0.25);
  etasq ~ normal(1, 0.25);
  a ~ normal(0, 1);
  y ~ multi_normal(rep_vector(a, N), K);
}
)";

std::string data(int n = 151) {
  std::string s = "{\"N\":" + std::to_string(n) + ",\"D\":[";
  char buf[40];
  for (int i = 0; i < n; ++i) {
    s += i ? ",[" : "[";
    for (int j = 0; j < n; ++j) {
      const double dx = std::sin(1.3 * i) - std::sin(1.3 * j);
      const double dy = std::cos(0.7 * i) - std::cos(0.7 * j);
      std::snprintf(buf, sizeof buf, "%s%.17g", j ? "," : "",
                    std::sqrt(dx * dx + dy * dy));
      s += buf;
    }
    s += "]";
  }
  s += "],\"y\":[";
  for (int i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof buf, "%s%.17g", i ? "," : "", std::sin(0.37 * i));
    s += buf;
  }
  return s + "]}";
}

int child() {
  char err[4096] = {};
  stanli_model* m = stanli_model_new_from_stan_with_opts(
      kModel, data().c_str(), nullptr, err, sizeof err);
  if (m == nullptr) {
    std::printf("compile failed: %s\n", err);
    return 1;
  }
  const double q[3] = {0.1, 0.2, 0.3};
  double lp = 0, g[3] = {};
  if (stanli_grad(m, q, &lp, g) != 0) {
    std::printf("gradient failed\n");
    return 1;
  }
  std::printf("%.17g %.17g %.17g %.17g\n", lp, g[0], g[1], g[2]);
  stanli_model_free(m);
  return 0;
}

stanli_model* model(int n, bool parallel_blas) {
  stanli_model_opts opts;
  stanli_model_opts_init(&opts);
  opts.parallel_blas = parallel_blas ? 1 : 0;
  char err[4096] = {};
  stanli_model* m = stanli_model_new_from_stan_with_opts(
      kModel, data(n).c_str(), &opts, err, sizeof err);
  if (m == nullptr) std::printf("compile failed: %s\n", err);
  return m;
}

// The largest relative difference that a gradient with parallel_blas on may
// have from the gradient with it off: the log density against its own
// magnitude and the gradient against its largest entry. Threads change the
// bits of dpotrf and of the newer dpotri for some sizes, so the gradients
// need not be equal. Measured over 2000 gradients of this model per size:
//   M3 Ultra, macOS 26.6.2: equal at N = 151 and 512, at most 2.9e-13 at
//     1100 and 9.6e-13 at 2048;
//   M1 CI runner, macOS 26.7.2: at most 1.2e-15 at N = 151, equal at 512
//     and 1100.
// The bound is about 100 times the largest of these. With the legacy dpotri_
// in its place on the M3 Ultra, 68 of 2000 gradients at N = 512 were
// more than 1e-12 off, the worst by 1.3e-4 and the closest by 2.5e-11, and
// in three runs of this test 19 to 26 of the 400 gradients at N = 512 were
// beyond the bound, so the test failed every time.
constexpr double kParallelBlasTolerance = 1e-10;

struct Gradient {
  double lp = 0, g[3] = {};
  bool ok = false;
};

Gradient gradient_values(stanli_model* m) {
  const double q[3] = {0.1, 0.2, 0.3};
  Gradient r;
  r.ok = stanli_grad(m, q, &r.lp, r.g) == 0;
  return r;
}

// The relative distance of a gradient from the one-thread gradient: the log
// density against its own magnitude, and the gradient against its largest
// entry.
double distance(const Gradient& got, const Gradient& want) {
  double largest = 0, worst = 0;
  for (int k = 0; k < 3; ++k) {
    largest = std::max(largest, std::fabs(want.g[k]));
    worst = std::max(worst, std::fabs(got.g[k] - want.g[k]));
  }
  const double lp =
      std::fabs(got.lp - want.lp) / std::max(std::fabs(want.lp), 1.0);
  const double g = largest > 0 ? worst / largest : worst;
  return std::isnan(lp) || std::isnan(g) ? INFINITY : std::max(lp, g);
}

int parallel_blas_repeats() {
  const int repeats = 400;
  bool ok = true;
  for (int n : {151, 512, 1100}) {
    stanli_model* off = model(n, false);
    stanli_model* on = model(n, true);
    if (off == nullptr || on == nullptr) return 1;
    const Gradient want = gradient_values(off);
    int differ = 0, over = 0;
    double worst = 0;
    for (int k = 0; k < repeats; ++k) {
      const Gradient got = gradient_values(on);
      const double d = got.ok ? distance(got, want) : INFINITY;
      differ += d != 0;
      over += !(d <= kParallelBlasTolerance);
      worst = std::max(worst, d);
    }
    stanli_model_free(off);
    stanli_model_free(on);
    std::printf(
        "N = %d: %d of %d gradients with parallel_blas on differ from it "
        "off, %d beyond %.0e, largest relative difference %.3e\n",
        n, differ, repeats, over, kParallelBlasTolerance, worst);
    ok = ok && want.ok && over == 0;
  }
  std::printf(ok ? "ok\n"
                 : "FAIL parallel_blas moves the gradient beyond rounding\n");
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--child") == 0) return child();
  if (!stanli_has_embedded_stanc()) {
    std::printf("SKIP: this build does not embed stanc\n");
    return 77;
  }
  if (argc > 1 && std::strcmp(argv[1], "--parallel-blas") == 0)
    return parallel_blas_repeats();
  std::vector<std::string> results;
  for (int round = 0; round < 6; ++round) {
    std::vector<std::string> batch(24);
    std::vector<std::thread> runs;
    for (size_t k = 0; k < batch.size(); ++k)
      runs.emplace_back([&, k] {
        const std::string cmd = std::string("'") + argv[0] + "' --child";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return;
        char buf[256] = {};
        if (fgets(buf, sizeof buf, f)) batch[k] = buf;
        pclose(f);
      });
    for (auto& t : runs) t.join();
    results.insert(results.end(), batch.begin(), batch.end());
  }
  const std::set<std::string> distinct(results.begin(), results.end());
  std::printf("%zu processes, %zu different results\n", results.size(),
              distinct.size());
  for (const std::string& r : distinct) std::printf("  %s", r.c_str());
  const bool ok = distinct.size() == 1 && !distinct.begin()->empty() &&
                  distinct.begin()->find("failed") == std::string::npos;
  std::printf(ok ? "ok\n" : "FAIL the gradient differs between processes\n");
  return ok ? 0 : 1;
}
