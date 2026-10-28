// Readers and passes recurse once per nesting level of the MIR, so a long
// left-associated sum or a deep chain of calls needs stack in proportion to
// its length. Programs nested no deeper than kInlineNesting (fewer on
// Windows, where the calling thread's stack is usually 1 MB) run on the
// calling thread, as they always have. A deeper one is read and lowered on a
// helper thread that has a large stack, and nesting beyond what that stack
// holds is a CompileError rather than an overflow.
#ifndef STANLI_LARGE_STACK_HPP
#define STANLI_LARGE_STACK_HPP

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace stanli {

inline constexpr unsigned kInlineNesting = 256;

// Thrown by a reader that meets nesting beyond nesting_limit().
struct NestingTooDeep : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The deepest statement/expression nesting a reader accepts on this thread.
// Emscripten has no threads, so it keeps to what its 8 MB stack holds.
unsigned nesting_limit();

// True when a program this deeply nested must be lowered on the large stack
// and this thread is not already on it.
bool needs_large_stack(unsigned nesting);

// True when threads cannot be started with a chosen stack size (Emscripten).
bool large_stack_unavailable();

namespace detail {
void run_on_large_stack(void (*body)(void*), void* context);
}

// Runs f on a thread with a large stack and returns its result, or rethrows
// what it threw. Already on such a thread, it runs f directly.
template <class F>
auto run_on_large_stack(F&& f) -> decltype(f()) {
  using Result = decltype(f());
  if constexpr (std::is_void_v<Result>) {
    detail::run_on_large_stack([](void* p) { (*static_cast<F*>(p))(); }, &f);
  } else {
    struct Call {
      F* f;
      std::optional<Result> result;
    } call{&f, std::nullopt};
    detail::run_on_large_stack(
        [](void* p) {
          auto* c = static_cast<Call*>(p);
          c->result.emplace((*c->f)());
        },
        &call);
    return std::move(*call.result);
  }
}

}  // namespace stanli

#endif
