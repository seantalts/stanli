// Runtime choice between the baseline and AVX2 copies of the dense-matrix
// kernels (matrix_fns.cpp, matrix_solve.cpp). Built only with
// -DSTANLI_AVX2_KERNELS=ON, and compiled with the baseline flags: nothing in
// this file may need more than x86-64.
//
// The kernels exist twice. The baseline copy is the normal translation unit
// with its entry points renamed *_base. The AVX2 copy is the same source built
// with -march=x86-64-v3 inside its own Eigen namespace (-DEigen=EigenAvx2),
// because Eigen's templates have the same mangled names under both targets but
// different packet sizes and blocking, and the linker would otherwise keep one
// copy of each and silently mix them. See
// notes/performance/2026-10-05-avx2-kernel-dispatch.md.
//
// The default is the baseline copy, so default-mode results do not change.
// The AVX2 copy is chosen only when asked for and the CPU can run it:
//   STANLI_FAST_MATH=1   fast mode: take the AVX2 kernels where available
//   STANLI_ISA=avx2      force AVX2 (testing); STANLI_ISA=baseline forces the
//                        baseline copy even when STANLI_FAST_MATH is set
//   STANLI_ISA_VERBOSE=1 report the choice on stderr
#include <cpuid.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "stanli/isa_dispatch.hpp"

// The AVX2 objects' static initializers are moved to the section
// avx2_init_array at build time (objcopy, see CMakeLists.txt) so the loader
// does not run AVX2 code on a CPU that lacks it. They run here, once, and only
// when the AVX2 kernels are selected. GNU ld defines these symbols for an
// output section whose name is a C identifier.
extern "C" {
typedef void (*StanliInitFn)(int, char**, char**);
extern StanliInitFn __start_avx2_init_array[] __attribute__((weak));
extern StanliInitFn __stop_avx2_init_array[] __attribute__((weak));
}

namespace stanli {

void register_matrix_kernels_base();
// Weak: a static libstanli.a does not carry the AVX2 objects (see
// CMakeLists.txt), and there this resolves to null and the baseline is used.
void register_matrix_kernels_avx2() __attribute__((weak));

namespace {

// Everything -march=x86-64-v3 may emit: AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE,
// with YMM state enabled by the OS. Raw cpuid/xgetbv rather than
// __builtin_cpu_supports, which clang does not accept for "lzcnt" or "movbe".
bool cpu_runs_x86_64_v3() {
  unsigned a, b, c, d;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  const bool fma = c & (1u << 12);
  const bool movbe = c & (1u << 22);
  const bool osxsave = c & (1u << 27);
  const bool avx = c & (1u << 28);
  const bool f16c = c & (1u << 29);
  if (!(fma && movbe && osxsave && avx && f16c)) return false;
  unsigned xcr0_lo, xcr0_hi;
  __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
  if ((xcr0_lo & 6u) != 6u) return false;  // XMM and YMM state
  if (__get_cpuid_max(0, nullptr) < 7) return false;
  __cpuid_count(7, 0, a, b, c, d);
  const bool bmi1 = b & (1u << 3);
  const bool avx2 = b & (1u << 5);
  const bool bmi2 = b & (1u << 8);
  if (!(bmi1 && avx2 && bmi2)) return false;
  if (!__get_cpuid(0x80000001u, &a, &b, &c, &d)) return false;
  return c & (1u << 5);  // LZCNT
}

bool env_on(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && std::strcmp(v, "0") != 0;
}

bool want_avx2(bool* forced) {
  const char* isa = std::getenv("STANLI_ISA");
  *forced = false;
  if (isa && !std::strcmp(isa, "baseline")) return false;
  if (isa && !std::strcmp(isa, "avx2")) {
    *forced = true;
    return true;
  }
  return env_on("STANLI_FAST_MATH");
}

bool g_avx2 = false;

void run_avx2_initializers() {
  for (StanliInitFn* f = __start_avx2_init_array;
       f && f < __stop_avx2_init_array; ++f)
    (*f)(0, nullptr, nullptr);
}

}  // namespace

bool matrix_kernels_use_avx2() { return g_avx2; }

void register_matrix_kernels() {
  bool forced = false;
  const bool wanted = want_avx2(&forced);
  const bool capable = register_matrix_kernels_avx2 && cpu_runs_x86_64_v3();
  if (wanted && !capable && forced)
    std::fprintf(stderr,
                 "stanli: STANLI_ISA=avx2 requested but this CPU lacks "
                 "AVX2/FMA/BMI2, or this build has no AVX2 kernels; using "
                 "the baseline kernels\n");
  g_avx2 = wanted && capable;
  if (env_on("STANLI_ISA_VERBOSE"))
    std::fprintf(stderr, "stanli: dense-matrix kernels: %s\n",
                 g_avx2 ? "avx2" : "baseline");
  if (g_avx2) {
    run_avx2_initializers();
    register_matrix_kernels_avx2();
  } else {
    register_matrix_kernels_base();
  }
}

}  // namespace stanli
