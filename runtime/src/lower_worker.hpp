#ifndef STANLI_LOWER_WORKER_HPP
#define STANLI_LOWER_WORKER_HPP

#include <stan/math/rev/core/chainablestack.hpp>

#include <cstddef>
#include <exception>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>
#ifndef _WIN32
#include <pthread.h>
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
    try {
      thread_ = std::thread([this] { run(); });
    } catch (const std::system_error&) {
      return false;
    }
#else
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setstacksize(&attr, kStackBytes);
    const int rc = pthread_create(&thread_, &attr, &LowerWorker::entry, this);
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
    thread_.join();
#else
    pthread_join(thread_, nullptr);
#endif
  }

  void rethrow_if_failed() {
    join();
    if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
  }

 private:
  static constexpr size_t kStackBytes = size_t{64} << 20;

  void run() {
    try {
      stan::math::ChainableStack autodiff_stack_of_this_thread;
      body_();
    } catch (...) {
      error_ = std::current_exception();
    }
  }
#ifndef _WIN32
  static void* entry(void* self) {
    static_cast<LowerWorker*>(self)->run();
    return nullptr;
  }
  pthread_t thread_{};
#else
  std::thread thread_;
#endif
  std::function<void()> body_;
  std::exception_ptr error_;
  bool started_ = false;
};

}  // namespace lower_detail
}  // namespace stanli

#endif
