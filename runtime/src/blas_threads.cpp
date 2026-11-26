// No Eigen here, and none of Accelerate's BLAS headers: thread_api.h
// declares only the threading controls.
#include <stanli/dense_blas.hpp>

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

OneBlasThread::OneBlasThread() {
#ifdef STANLI_VECLIB_THREADING
  if (__builtin_available(macOS 15.0, *)) {
    saved_ = static_cast<unsigned>(BLASGetThreading());
    if (saved_ != static_cast<unsigned>(BLAS_THREADING_SINGLE_THREADED))
      restore_ = BLASSetThreading(BLAS_THREADING_SINGLE_THREADED) == 0;
  }
#endif
}

OneBlasThread::~OneBlasThread() {
#ifdef STANLI_VECLIB_THREADING
  if (__builtin_available(macOS 15.0, *)) {
    if (restore_) BLASSetThreading(static_cast<BLAS_THREADING>(saved_));
  }
#endif
}

}  // namespace stanli
