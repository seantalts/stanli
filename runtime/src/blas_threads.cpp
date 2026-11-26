// No Eigen here, and none of Accelerate's BLAS headers: thread_api.h
// declares only the threading controls.
#include <stanli/debug_switch.hpp>
#include <stanli/dense_blas.hpp>

#include <cstdint>
#include <cstdlib>
#include <thread>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

// The functions are weak imports below a macOS 15 deployment target, so the
// library still loads on older systems.
#if defined(STANLI_ACCELERATE) && defined(__APPLE__) && \
    __has_include(<vecLib/thread_api.h>)
#include <vecLib/thread_api.h>
#define STANLI_VECLIB_THREADING 1
#endif

namespace stanli {

const char* blas_provider() {
#ifdef STANLI_ACCELERATE
  return "accelerate";
#else
  return "none";
#endif
}

bool blas_thread_control_available() {
#ifdef STANLI_VECLIB_THREADING
  if (__builtin_available(macOS 15.0, *)) return true;
#endif
  return false;
}

bool blas_may_thread(bool parallel_blas, int parallel_chains,
                     int performance_cores) {
  return parallel_blas &&
         int64_t(parallel_chains) * 2 <= int64_t(performance_cores);
}

int performance_cores() {
  static const int cores = [] {
    if (const char* v = debug_env("STANLI_PERFORMANCE_CORES")) {
      const int n = std::atoi(v);
      if (n > 0) return n;
    }
#ifdef __APPLE__
    int n = 0;
    size_t len = sizeof n;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &n, &len, nullptr, 0) == 0 &&
        n > 0)
      return n;
#endif
    const unsigned hc = std::thread::hardware_concurrency();
    return hc > 0 ? static_cast<int>(hc) : 1;
  }();
  return cores;
}

namespace {
thread_local bool t_parallel_blas = false;
thread_local int t_parallel_chains = 1;
}  // namespace

ParallelBlasScope::ParallelBlasScope(bool parallel_blas)
    : previous_(t_parallel_blas) {
  t_parallel_blas = parallel_blas;
}
ParallelBlasScope::~ParallelBlasScope() { t_parallel_blas = previous_; }

ParallelChainsScope::ParallelChainsScope(int chains)
    : previous_(t_parallel_chains) {
  t_parallel_chains = chains;
}
ParallelChainsScope::~ParallelChainsScope() { t_parallel_chains = previous_; }

BlasCallThreads::BlasCallThreads() {
#ifdef STANLI_VECLIB_THREADING
  if (__builtin_available(macOS 15.0, *)) {
    const BLAS_THREADING want =
        t_parallel_blas &&
                blas_may_thread(true, t_parallel_chains, performance_cores())
            ? BLAS_THREADING_MULTI_THREADED
            : BLAS_THREADING_SINGLE_THREADED;
    saved_ = static_cast<unsigned>(BLASGetThreading());
    if (saved_ != static_cast<unsigned>(want))
      restore_ = BLASSetThreading(want) == 0;
  }
#endif
}

BlasCallThreads::~BlasCallThreads() {
#ifdef STANLI_VECLIB_THREADING
  if (__builtin_available(macOS 15.0, *)) {
    if (restore_) BLASSetThreading(static_cast<BLAS_THREADING>(saved_));
  }
#endif
}

}  // namespace stanli
