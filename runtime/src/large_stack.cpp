#include <stanli/large_stack.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>

#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace stanli {
namespace {

#if defined(__SANITIZE_ADDRESS__)
#define STANLI_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STANLI_ASAN 1
#endif
#endif

// Measured worst-case stack per nesting level across the readers and passes.
// AddressSanitizer pads every frame, so it gets a larger allowance.
#ifdef STANLI_ASAN
constexpr size_t kBytesPerLevel = size_t{128} * 1024;
#else
constexpr size_t kBytesPerLevel = size_t{24} * 1024;
#endif
constexpr unsigned kMaxNesting = 40000;
constexpr unsigned kEmscriptenNesting = 1000;
#ifdef _WIN32
constexpr size_t kStackBytes = size_t{256} << 20;
// Left for the frames below the recursion and the guard page.
constexpr size_t kInlineSlackBytes = size_t{128} * 1024;
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

// A Windows thread has the stack its executable asks for, 1 MB by default,
// where kInlineNesting assumes the 8 MB of a POSIX main thread.
static unsigned inline_nesting_limit() {
#ifdef __EMSCRIPTEN__
  return kEmscriptenNesting;
#elif defined(_WIN32)
  thread_local const unsigned limit = [] {
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    const size_t bytes = high - low;
    const size_t usable =
        bytes > kInlineSlackBytes ? bytes - kInlineSlackBytes : 0;
    return static_cast<unsigned>(
        std::min<size_t>(kInlineNesting, usable / kBytesPerLevel));
  }();
  return limit;
#else
  return kInlineNesting;
#endif
}

unsigned nesting_limit() {
  if (large_stack_limit != 0) return large_stack_limit;
  return inline_nesting_limit();
}

bool needs_large_stack(unsigned nesting) {
  return large_stack_limit == 0 && !large_stack_unavailable() &&
         nesting > inline_nesting_limit();
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
