#ifndef STANLI_LOWER_WORKER_HPP
#define STANLI_LOWER_WORKER_HPP

#include <stan/math/rev/core/chainablestack.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <process.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#endif

namespace stanli {
namespace lower_detail {

class LowerWorker {
 public:
  LowerWorker() = default;
  LowerWorker(const LowerWorker&) = delete;
  LowerWorker& operator=(const LowerWorker&) = delete;
  ~LowerWorker() { join(); }

  // False when the thread could not be created; the body has not run.
  bool start(std::function<void()> body) {
    body_ = std::move(body);
#ifdef _WIN32
    thread_ = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, static_cast<unsigned>(stack_bytes()), &LowerWorker::entry,
        this, STACK_SIZE_PARAM_IS_RESERVATION, nullptr));
    if (thread_ == nullptr) return false;
#else
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    int rc = pthread_attr_setstacksize(&attr, stack_bytes());
    if (rc == 0)
      rc = pthread_create(&thread_, &attr, &LowerWorker::entry, this);
    pthread_attr_destroy(&attr);
    if (rc != 0) return false;
#endif
    started_ = true;
    return true;
  }

  void join() {
    if (!started_) return;
    started_ = false;
#ifdef _WIN32
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
#else
    pthread_join(thread_, nullptr);
#endif
  }

  void rethrow_if_failed() {
    join();
    if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
  }

 private:
  static size_t stack_bytes() {
    size_t bytes = size_t{64} << 20;
#ifndef _WIN32
    rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) == 0 &&
        limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur > bytes)
      bytes = std::min<size_t>(static_cast<size_t>(limit.rlim_cur),
                               size_t{1} << 30);
#endif
    return bytes;
  }

  void run() {
    try {
      stan::math::ChainableStack autodiff_stack_of_this_thread;
      body_();
    } catch (...) {
      error_ = std::current_exception();
    }
  }
#ifdef _WIN32
  static unsigned __stdcall entry(void* self) {
    static_cast<LowerWorker*>(self)->run();
    return 0;
  }
  HANDLE thread_ = nullptr;
#else
  static void* entry(void* self) {
    static_cast<LowerWorker*>(self)->run();
    return nullptr;
  }
  pthread_t thread_{};
#endif
  std::function<void()> body_;
  std::exception_ptr error_;
  bool started_ = false;
};

}  // namespace lower_detail
}  // namespace stanli

#endif
