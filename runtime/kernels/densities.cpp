// Registration for the density kernels. The forwards themselves live in
// densities_*.cpp, one shard per group, because a single translation
// unit holding all of them peaked at 7.6 GB and serialized the build.
//
// STANLI_DENSITY_PACK partitions that further, for the browser only. The
// long tail of densities and the distribution functions are 55% of the
// compressed download and most models use none of them, so the browser
// builds them into a side module that is fetched when a model turns out
// to need one. Native builds define neither macro and register
// everything, which is why the wheel is unaffected.
//
//   (neither)              native: everything, one library
//   STANLI_DENSITY_CORE    browser core: register core kernels only
//   STANLI_DENSITY_PACK    the side module: register pack kernels only
#include "densities_impl.hpp"

namespace stanli {
using namespace dens;

#if defined(STANLI_DENSITY_PACK)
// The side module has no g_table of its own: registration has to land in
// the core's. Shadowing register_kernel inside this translation unit
// keeps the shared macro blocks below identical in both builds.
extern "C" void stanli_register_kernel_c(uint16_t, void*, void*, void*);
static void pack_register_kernel(uint16_t opcode, Kernel k) {
  stanli_register_kernel_c(opcode, reinterpret_cast<void*>(k.forward),
                           reinterpret_cast<void*>(k.backward),
                           reinterpret_cast<void*>(k.scratch_size));
}
// Redirect the name, so the registration blocks below read the same in
// both builds. Shadowing does not work here: stanli::register_kernel is
// already declared, and a using-declaration collides with it.
#define register_kernel pack_register_kernel
#endif

#if defined(STANLI_DENSITY_PACK)
// The side module's entry point. Loaded through stanli_load_pack, which
// dlsyms this name.
extern "C" void stanli_pack_register() {
#else
void register_density_kernels() {
#endif

#if !defined(STANLI_DENSITY_PACK)
  // Core: the distributions models lean on, the integer-outcome ones, and
  // the hand-written kernels.
#define STANLI_REGISTER_DENSITY(code, fn, n, tier) \
  register_kernel(code, Kernel{fn##_fwd_gen, density_bwd<n>, density_scratch<n>});
  STANLI_SCALAR_DENSITY_LIST_COMMON(STANLI_REGISTER_DENSITY)
#undef STANLI_REGISTER_DENSITY
  // Discrete densities: the outcome sits in idata, so the real-argument
  // count is what the shared backward contracts.
#define STANLI_REGISTER_INT_DENSITY(code, fn, nreal, tier) \
  register_kernel(code,                                    \
                  Kernel{fn##_fwd_gen, density_bwd<nreal>, density_scratch<nreal>});
  STANLI_INT_DENSITY_LIST(STANLI_REGISTER_INT_DENSITY)
#undef STANLI_REGISTER_INT_DENSITY
  // The list is the default. A density whose forward needs more than the
  // shared one registers after it and wins: uniform_lpdf has to decide
  // support itself, because stan-math returns LOG_ZERO out of support
  // through an early return that never reaches the partials sink, and the
  // recorder would leave the value at 0 (CmdStan -inf, stanli finite --
  // caught by the dogs_log reference).
  register_kernel(OP_UNIFORM_LPDF,
                  Kernel{uniform_fwd, density_bwd<3>, density_scratch<3>});
  register_kernel(OP_POISSON_LOG_LPMF,
                  Kernel{poisson_log_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_BERNOULLI_LOGIT_LPMF,
                  Kernel{bernoulli_logit_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_BERNOULLI_LPMF,
                  Kernel{bernoulli_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_POISSON_LPMF,
                  Kernel{poisson_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_NEG_BINOMIAL_2_LPMF,
                  Kernel{neg_binomial_2_fwd, density_bwd<2>, density_scratch<2>});
  register_kernel(OP_BINOMIAL_LPMF,
                  Kernel{binomial_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_BINOMIAL_LOGIT_LPMF,
                  Kernel{binomial_logit_fwd, density_bwd<1>, density_scratch<1>});
  register_kernel(OP_BERNOULLI_LOGIT_GLM_LPMF,
                  Kernel{bernoulli_logit_glm_fwd, bernoulli_logit_glm_bwd,
                         sum_in_lens});
  register_kernel(OP_POISSON_LOG_GLM_LPMF,
                  Kernel{poisson_log_glm_fwd, poisson_log_glm_bwd,
                         sum_in_lens});
  register_kernel(OP_NEG_BINOMIAL_2_LOG_GLM_LPMF,
                  Kernel{neg_binomial_2_log_glm_fwd,
                         neg_binomial_2_log_glm_bwd, sum_in_lens});
  register_kernel(OP_BETA_BINOMIAL_LPMF,
                  Kernel{beta_binomial_fwd, density_bwd<2>,
                         density_scratch<2>});
#endif  // !STANLI_DENSITY_PACK

#if !defined(STANLI_DENSITY_CORE)
  // Pack: the long tail and the distribution functions. In a browser
  // build these live in the side module and arrive only if a model needs
  // one; in a native build this is the same function, so nothing moves.
#define STANLI_REGISTER_DENSITY(code, fn, n, tier) \
  register_kernel(code, Kernel{fn##_fwd_gen, density_bwd<n>, density_scratch<n>});
  STANLI_SCALAR_DENSITY_LIST_REST(STANLI_REGISTER_DENSITY)
#undef STANLI_REGISTER_DENSITY
#define STANLI_REGISTER_CDF(code, fn, n, tier) \
  register_kernel(code, Kernel{fn##_fwd_gen, density_bwd<n>, density_scratch<n>});
  STANLI_SCALAR_CDF_LIST(STANLI_REGISTER_CDF)
  STANLI_INT_CDF_LIST(STANLI_REGISTER_CDF)
#undef STANLI_REGISTER_CDF
#define STANLI_REGISTER_ORDERED(code, fn, nargs, vm) \
  register_kernel(code,                              \
                  Kernel{fn##_fwd_gen, density_bwd<nargs>, density_scratch<nargs>});
  STANLI_ORDERED_DENSITY_LIST(STANLI_REGISTER_ORDERED)
#undef STANLI_REGISTER_ORDERED
#endif  // !STANLI_DENSITY_CORE
}

}  // namespace stanli
