// Is tail-call threaded dispatch worth converting ~150 kernel signatures?
//
// The executor's sweeps are already an indirect call through a resolved
// function-pointer array, not a switch, so the classic computed-goto win
// (one mega-branch aliasing every opcode transition) does not apply
// unchanged. This measures the remaining headroom directly: the same work,
// the same kernels, dispatched three ways.
//
//   LOOP     what the executor does today: for (i) fn[i](ctx[i])
//   TAIL     each kernel tail-calls the next step (musttail), so dispatch
//            overlaps the kernel epilogue and each site predicts on its own
//   INLINE   the floor: the kernel body inlined into a plain loop, no
//            indirection at all
//
// Kernels here are stand-ins with the shape of the real trivial ops (a few
// loads, an arithmetic op, a store), which is the case dispatch overhead
// actually dominates.
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <vector>

namespace {

struct Ctx {
  const double* a;
  const double* b;
  double* out;
  int64_t len;
};

template <typename F>
double time_ns(int reps, F&& f) {
  const auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < reps; ++r) f();
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

// ---- LOOP: today's shape ---------------------------------------------------
void add_fwd(Ctx& c) { c.out[0] = c.a[0] + c.b[0]; }
void mul_fwd(Ctx& c) { c.out[0] = c.a[0] * c.b[0]; }
void sub_fwd(Ctx& c) { c.out[0] = c.a[0] - c.b[0]; }
void neg_fwd(Ctx& c) { c.out[0] = -c.a[0]; }

using Fn = void (*)(Ctx&);

// ---- TAIL: each kernel chains to the next ----------------------------------
struct Step;
using TailFn = void (*)(Step*, Ctx*);
struct Step {
  TailFn fn;
};

void t_stop(Step*, Ctx*) {}

#if defined(__clang__)
#define STANLI_MUSTTAIL [[clang::musttail]]
#else
#define STANLI_MUSTTAIL
#endif

void t_add(Step* s, Ctx* c) {
  c->out[0] = c->a[0] + c->b[0];
  STANLI_MUSTTAIL return (s + 1)->fn(s + 1, c + 1);
}
void t_mul(Step* s, Ctx* c) {
  c->out[0] = c->a[0] * c->b[0];
  STANLI_MUSTTAIL return (s + 1)->fn(s + 1, c + 1);
}
void t_sub(Step* s, Ctx* c) {
  c->out[0] = c->a[0] - c->b[0];
  STANLI_MUSTTAIL return (s + 1)->fn(s + 1, c + 1);
}
void t_neg(Step* s, Ctx* c) {
  c->out[0] = -c->a[0];
  STANLI_MUSTTAIL return (s + 1)->fn(s + 1, c + 1);
}

}  // namespace

int main() {
  const int N = 4096;
  const int REPS = 20000;

  std::vector<double> vals((size_t)N + 8, 1.0001);
  for (int i = 0; i < N + 8; ++i) vals[(size_t)i] = 1.0 + 0.0001 * i;

  std::vector<Ctx> ctx((size_t)N + 1);
  std::vector<Fn> fns((size_t)N);
  std::vector<Step> steps((size_t)N + 1);
  std::vector<int> kind((size_t)N);
  for (int i = 0; i < N; ++i) {
    ctx[(size_t)i] =
        Ctx{&vals[(size_t)i], &vals[(size_t)i + 1], &vals[(size_t)i + 2], 1};
    kind[(size_t)i] = i % 4;
    switch (kind[(size_t)i]) {
      case 0:
        fns[(size_t)i] = add_fwd;
        steps[(size_t)i].fn = t_add;
        break;
      case 1:
        fns[(size_t)i] = mul_fwd;
        steps[(size_t)i].fn = t_mul;
        break;
      case 2:
        fns[(size_t)i] = sub_fwd;
        steps[(size_t)i].fn = t_sub;
        break;
      default:
        fns[(size_t)i] = neg_fwd;
        steps[(size_t)i].fn = t_neg;
        break;
    }
  }
  ctx[(size_t)N] = ctx[0];
  steps[(size_t)N].fn = t_stop;

  const double loop_ns = time_ns(REPS, [&] {
    for (int i = 0; i < N; ++i) fns[(size_t)i](ctx[(size_t)i]);
  });
  const double tail_ns =
      time_ns(REPS, [&] { steps[0].fn(steps.data(), ctx.data()); });
  // UNROLL4: today's loop, four separate call sites -- no kernel changes.
  const double unroll_ns = time_ns(REPS, [&] {
    int i = 0;
    for (; i + 4 <= N; i += 4) {
      fns[(size_t)i](ctx[(size_t)i]);
      fns[(size_t)i + 1](ctx[(size_t)i + 1]);
      fns[(size_t)i + 2](ctx[(size_t)i + 2]);
      fns[(size_t)i + 3](ctx[(size_t)i + 3]);
    }
    for (; i < N; ++i) fns[(size_t)i](ctx[(size_t)i]);
  });
  const double inline_ns = time_ns(REPS, [&] {
    for (int i = 0; i < N; ++i) {
      Ctx& c = ctx[(size_t)i];
      switch (kind[(size_t)i]) {
        case 0:
          c.out[0] = c.a[0] + c.b[0];
          break;
        case 1:
          c.out[0] = c.a[0] * c.b[0];
          break;
        case 2:
          c.out[0] = c.a[0] - c.b[0];
          break;
        default:
          c.out[0] = -c.a[0];
          break;
      }
    }
  });

  std::printf("LOOP   (today)      : %7.2f ns/op\n", loop_ns / N);
  std::printf("TAIL   (musttail)   : %7.2f ns/op  (%.0f%% of LOOP)\n",
              tail_ns / N, 100.0 * tail_ns / loop_ns);
  std::printf("UNROLL4 (loop x4)   : %7.2f ns/op  (%.0f%% of LOOP)\n",
              unroll_ns / N, 100.0 * unroll_ns / loop_ns);
  std::printf("INLINE (floor)      : %7.2f ns/op  (%.0f%% of LOOP)\n",
              inline_ns / N, 100.0 * inline_ns / loop_ns);
  std::printf("sink=%g\n", vals[(size_t)N / 2]);
  return 0;
}
