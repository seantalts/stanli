// Math threads only when cores are free. Accelerate (STANLI_ACCELERATE)
// spreads a large product or factorization over several threads of its own.
// Chains that run in parallel limit their BLAS calls to one thread when
// parallel chains * 2 exceed the performance cores (stanli/dense_blas.hpp).
// The developer switch STANLI_PERFORMANCE_CORES sets the core count, so the
// test needs a build with developer switches.
//
// For a Gaussian process (cholesky_decompose and multi_normal_cholesky) and
// a large matrix product, with four chains:
//  * filled (4 performance cores): the four chains use at most about four
//    cores of CPU time, and the process gains no threads beyond the four
//    chain workers (macOS 15 or later, which has BLASSetThreading);
//  * free (64 performance cores): Accelerate starts threads of its own;
//  * in both, chain k draws the same values alone as next to three others.
//
// Arguments: gp|product filled|free [sizes].
#include "env_helpers.hpp"

#include <stanli/capi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>
#include <atomic>

#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <dirent.h>
#endif

namespace {

int failures = 0;

void expect(const std::string& what, bool ok) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

int thread_count() {
#ifdef __APPLE__
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t n = 0;
  if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return -1;
  for (mach_msg_type_number_t i = 0; i < n; ++i)
    mach_port_deallocate(mach_task_self(), threads[i]);
  vm_deallocate(mach_task_self(), (vm_address_t)threads,
                n * sizeof(thread_act_t));
  return (int)n;
#else
  DIR* d = opendir("/proc/self/task");
  if (!d) return -1;
  int n = 0;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') ++n;
  closedir(d);
  return n;
#endif
}

double cpu_seconds() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return (double)u.ru_utime.tv_sec + 1e-6 * (double)u.ru_utime.tv_usec +
         (double)u.ru_stime.tv_sec + 1e-6 * (double)u.ru_stime.tv_usec;
}

std::string json_vector(const std::vector<double>& v) {
  std::string s = "[";
  char buf[40];
  for (size_t i = 0; i < v.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%s%.17g", i ? "," : "", v[i]);
    s += buf;
  }
  return s + "]";
}

// Row-major nested arrays, which is how CmdStan JSON writes a matrix.
std::string json_matrix(const std::vector<double>& colmajor, int rows,
                        int cols) {
  std::string s = "[";
  for (int r = 0; r < rows; ++r) {
    std::vector<double> row((size_t)cols);
    for (int c = 0; c < cols; ++c)
      row[(size_t)c] = colmajor[(size_t)c * (size_t)rows + (size_t)r];
    s += (r ? "," : "") + json_vector(row);
  }
  return s + "]";
}

const char* kGp = R"(
data {
  int<lower=1> N;
  array[N] real x;
  vector[N] y;
}
parameters {
  real<lower=0> rho;
  real<lower=0> alpha;
  real<lower=0> sigma;
}
model {
  matrix[N, N] L = cholesky_decompose(
      add_diag(gp_exp_quad_cov(x, alpha, rho), square(sigma)));
  rho ~ inv_gamma(5, 5);
  alpha ~ std_normal();
  sigma ~ std_normal();
  y ~ multi_normal_cholesky(rep_vector(0, N), L);
}
)";

std::string gp_data(int n) {
  std::vector<double> x((size_t)n), y((size_t)n);
  for (int i = 0; i < n; ++i) {
    x[(size_t)i] = 10.0 * i / n;
    y[(size_t)i] = std::sin(x[(size_t)i]) + 0.3 * std::cos(7.3 * i);
  }
  return "{\"N\":" + std::to_string(n) + ",\"x\":" + json_vector(x) +
         ",\"y\":" + json_vector(y) + "}";
}

// diag_post_multiply(X, s) * X' is an N x K by K x N product, and its
// adjoint is another; Accelerate threads both from about N = 512, K = 128.
const char* kProduct = R"(
data {
  int<lower=1> N;
  int<lower=1> K;
  matrix[N, K] X;
  matrix[N, N] Y;
}
parameters {
  vector[K] s;
}
model {
  s ~ std_normal();
  to_vector(Y) ~ normal(to_vector(diag_post_multiply(X, s) * X'), 1);
}
)";

std::string product_data(int n, int k) {
  std::vector<double> x((size_t)n * k), y((size_t)n * n);
  const double scale = 1.0 / std::sqrt((double)k);
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = scale * std::sin(1.0 + 0.7 * (double)i);
  for (size_t i = 0; i < y.size(); ++i) y[i] = 0.1 * std::cos(0.3 * (double)i);
  return "{\"N\":" + std::to_string(n) + ",\"K\":" + std::to_string(k) +
         ",\"X\":" + json_matrix(x, n, k) + ",\"Y\":" + json_matrix(y, n, n) +
         "}";
}

struct Run {
  std::vector<double> draws;
  double cpu = 0, wall = 0;
  int max_threads = 0;
  int rc = -1;
};

Run sample(stanli_model* m, int chains, int chain_id, int threads) {
  stanli_sample_opts o;
  stanli_sample_opts_init(&o);
  o.seed = 20261126;
  o.chains = chains;
  o.chain_id = chain_id;
  o.warmup = 60;
  o.samples = 40;
  o.max_depth = 5;
  o.num_threads = threads;
  const int64_t n = stanli_n_unconstrained(m);
  Run run;
  run.draws.resize((size_t)(chains * stanli_n_stored_draws(&o) * n));

  std::atomic<bool> done{false};
  std::atomic<int> peak{thread_count()};
  std::thread monitor([&] {
    while (!done.load()) {
      const int now = thread_count();
      if (now > peak.load()) peak.store(now);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  char err[4096]{};
  const double cpu0 = cpu_seconds();
  const auto t0 = std::chrono::steady_clock::now();
  run.rc = stanli_sample_multi_metric(
      m, &o, nullptr, 0, run.draws.data(), nullptr, nullptr, nullptr, nullptr,
      nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  run.wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  run.cpu = cpu_seconds() - cpu0;
  done.store(true);
  monitor.join();
  run.max_threads = peak.load();
  if (run.rc != 0) std::printf("sampling failed: %s\n", err);
  stanli_string_free(stanli_take_messages());
  return run;
}

bool thread_limit_available() {
#if defined(STANLI_ACCELERATE) && defined(__APPLE__)
  if (__builtin_available(macOS 15.0, *)) return true;
#endif
  return false;
}

void check_model(const char* name, const char* code, const std::string& data,
                 bool filled) {
  char err[8192]{};
  stanli_model* m = stanli_model_new_from_stan_with_opts(
      code, data.c_str(), nullptr, err, sizeof err);
  if (m == nullptr) {
    expect(std::string(name) + " compiles: " + err, false);
    return;
  }
  const int C = 4;

  // The parallel run first, so no earlier BLAS call has warmed up a pool.
  // Its threads are this one, the monitor and the four chain workers.
  const int before = thread_count();
  const Run par = sample(m, C, 1, C);
  expect(std::string(name) + " parallel run succeeds", par.rc == 0);
  const double cores = par.cpu / par.wall;
  std::printf(
      "%s: %d chains, wall %.3f s, cpu %.3f s, %.2f cores, "
      "threads %d before, at most %d during\n",
      name, C, par.wall, par.cpu, cores, before, par.max_threads);
  if (filled && thread_limit_available()) {
    expect(std::string(name) + " uses at most about 4 cores (" +
               std::to_string(cores) + ")",
           cores <= 4.4);
    expect(std::string(name) + " starts no threads beyond its chain workers (" +
               std::to_string(par.max_threads) + " > " +
               std::to_string(before + 1 + C) + ")",
           par.max_threads <= before + 1 + C);
  }
#ifdef STANLI_ACCELERATE
  if (!filled)
    expect(std::string(name) + " lets Accelerate start threads (" +
               std::to_string(par.max_threads) +
               " <= " + std::to_string(before + 1 + C) + ")",
           par.max_threads > before + 1 + C);
#endif

  const size_t per_chain = par.draws.size() / C;
  for (int k = 0; k < C; ++k) {
    const Run solo = sample(m, 1, 1 + k, 1);
    expect(std::string(name) + " chain alone succeeds", solo.rc == 0);
    size_t differ = 0;
    for (size_t i = 0; i < per_chain && i < solo.draws.size(); ++i)
      differ += solo.draws[i] != par.draws[(size_t)k * per_chain + i];
    if (differ)
      std::printf("%s chain %d: %zu of %zu values differ\n", name, k + 1,
                  differ, per_chain);
    expect(std::string(name) + " chain " + std::to_string(k + 1) +
               " draws the same alone as with three others",
           solo.draws.size() == per_chain && differ == 0);
  }
  stanli_model_free(m);
}

}  // namespace

int main(int argc, char** argv) {
  if (!stanli_has_embedded_stanc()) {
    std::printf("SKIP: this build does not embed stanc\n");
    return 77;
  }
  if (!stanli_thread_safe()) {
    std::printf("SKIP: this build runs chains sequentially\n");
    return 77;
  }
  if (!debug_switches_available()) {
    std::printf("SKIP: STANLI_PERFORMANCE_CORES needs developer switches\n");
    return 77;
  }
  // Adaptation notices and early rejections are expected with short runs.
  stanli_capture_messages(1);
  // One model per process: Accelerate keeps its threads once started, so a
  // second model in the same process would start with them already there.
  const std::string which = argc > 1 ? argv[1] : "gp";
  const std::string cores = argc > 2 ? argv[2] : "filled";
  if (cores != "filled" && cores != "free") {
    std::printf("usage: test_blas_chains gp|product filled|free [sizes]\n");
    return 2;
  }
  const bool filled = cores == "filled";
  test_setenv("STANLI_PERFORMANCE_CORES", filled ? "4" : "64");
  std::printf("%s performance cores, %s\n", filled ? "4" : "64",
              thread_limit_available() ? "BLASSetThreading available"
                                       : "no BLASSetThreading");
  if (which == "gp") {
    const int n = argc > 3 ? std::atoi(argv[3]) : 300;
    std::printf("gp N=%d\n", n);
    check_model("gp", kGp, gp_data(n), filled);
  } else if (which == "product") {
    const int n = argc > 3 ? std::atoi(argv[3]) : 512;
    const int k = argc > 4 ? std::atoi(argv[4]) : 128;
    std::printf("product %dx%d times %dx%d\n", n, k, k, n);
    check_model("product", kProduct, product_data(n, k), filled);
  } else {
    std::printf("usage: test_blas_chains gp|product filled|free [sizes]\n");
    return 2;
  }
  if (failures == 0) std::printf("ok\n");
  return failures == 0 ? 0 : 1;
}
