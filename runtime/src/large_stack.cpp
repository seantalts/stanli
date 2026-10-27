#include <stanli/large_stack.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>

#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif

namespace stanli {
namespace {

// Measured worst-case stack per nesting level across the readers and passes.
constexpr size_t kBytesPerLevel = size_t{24} * 1024;
constexpr unsigned kMaxNesting = 40000;
#ifdef _WIN32
constexpr size_t kStackBytes = size_t{256} << 20;
#else
constexpr size_t kStackBytes = size_t{1} << 30;
#endif

thread_local unsigned large_stack_limit = 0;

struct Job {
  void (*body)(void*);
  void* context;
  unsigned limit;
  std::exception_ptr error;
};

#ifndef __EMSCRIPTEN__
void* run_job(void* p) {
  Job* job = static_cast<Job*>(p);
  large_stack_limit = job->limit;
  try {
    job->body(job->context);
  } catch (...) {
    job->error = std::current_exception();
  }
  return nullptr;
}
#endif

}  // namespace

bool large_stack_unavailable() {
#ifdef __EMSCRIPTEN__
  return true;
#else
  return false;
#endif
}

unsigned nesting_limit() {
  return large_stack_limit != 0 ? large_stack_limit : kInlineNesting;
}

namespace detail {

void run_on_large_stack(void (*body)(void*), void* context) {
  if (large_stack_limit != 0 || large_stack_unavailable()) {
    body(context);
    return;
  }
#ifndef __EMSCRIPTEN__
  for (size_t bytes = kStackBytes; bytes >= size_t{64} << 20; bytes /= 4) {
    Job job{body, context,
            static_cast<unsigned>(
                std::min<size_t>(kMaxNesting, bytes / kBytesPerLevel)),
            nullptr};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_t thread;
    const bool started = pthread_attr_setstacksize(&attr, bytes) == 0 &&
                         pthread_create(&thread, &attr, run_job, &job) == 0;
    pthread_attr_destroy(&attr);
    if (!started) continue;
    pthread_join(thread, nullptr);
    if (job.error) std::rethrow_exception(job.error);
    return;
  }
  throw NestingTooDeep("could not start a thread with a large stack");
#endif
}

}  // namespace detail
}  // namespace stanli
