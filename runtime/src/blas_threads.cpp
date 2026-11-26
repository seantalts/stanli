// No Eigen here, and none of Accelerate's BLAS headers: thread_api.h
// declares only the threading controls.
#include <stanli/debug_switch.hpp>
#include <stanli/dense_blas.hpp>

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

BlasThreadPlan plan_blas_threads(int parallel_chains, int performance_cores,
                                 bool thread_control_available) {
  BlasThreadPlan plan;
  plan.cores_full = int64_t(parallel_chains) * 2 > int64_t(performance_cores);
  plan.limited = plan.cores_full && thread_control_available;
  return plan;
}

int performance_cores() {
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
}

bool blas_thread_control_available() {
#ifdef STANLI_VECLIB_THREADING
  if (__builtin_available(macOS 15.0, *)) return true;
#endif
  return false;
}

ChainBlasThreads::ChainBlasThreads(int parallel_chains) {
#ifdef STANLI_VECLIB_THREADING
  if (!plan_blas_threads(parallel_chains, performance_cores(),
                         blas_thread_control_available())
           .limited)
    return;
  if (__builtin_available(macOS 15.0, *)) {
    saved_ = static_cast<unsigned>(BLASGetThreading());
    active_ = BLASSetThreading(BLAS_THREADING_SINGLE_THREADED) == 0;
  }
#else
  (void)parallel_chains;
#endif
}

ChainBlasThreads::~ChainBlasThreads() {
#ifdef STANLI_VECLIB_THREADING
  if (__builtin_available(macOS 15.0, *)) {
    if (active_) BLASSetThreading(static_cast<BLAS_THREADING>(saved_));
  }
#endif
}

}  // namespace stanli
