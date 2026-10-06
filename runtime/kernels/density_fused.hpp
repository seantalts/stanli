#ifndef STANLI_KERNELS_DENSITY_FUSED_HPP
#define STANLI_KERNELS_DENSITY_FUSED_HPP

#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>

#include <cstddef>

namespace stanli {
namespace dens {

bool fused_density_enabled();
void set_fused_density(bool on);
std::size_t fused_density_calls();

void normal_lpdf_fused(KernelCtx& ctx);
void cauchy_lpdf_fused(KernelCtx& ctx);
void student_t_lpdf_fused(KernelCtx& ctx);
void lognormal_lpdf_fused(KernelCtx& ctx);
void ordered_logistic_lpmf_fused(KernelCtx& ctx);

void normal_lpdf_fwd_gen(KernelCtx& ctx);
void cauchy_lpdf_fwd_gen(KernelCtx& ctx);
void student_t_lpdf_fwd_gen(KernelCtx& ctx);
void lognormal_lpdf_fwd_gen(KernelCtx& ctx);

#ifdef STANLI_FUSED_ONLY
inline constexpr bool kFusedOnly = true;
#else
inline constexpr bool kFusedOnly = false;
#endif

using FusedKernel = void (*)(KernelCtx&);

template <typename Code>
constexpr FusedKernel fused_kernel_for(Code code) {
  if (code == OP_NORMAL_LPDF) return &normal_lpdf_fused;
  if (code == OP_CAUCHY_LPDF) return &cauchy_lpdf_fused;
  if (code == OP_STUDENT_T_LPDF) return &student_t_lpdf_fused;
  if (code == OP_LOGNORMAL_LPDF) return &lognormal_lpdf_fused;
  return nullptr;
}

inline bool fused_density_active() {
  if constexpr (kFusedOnly)
    return true;
  else
    return fused_density_enabled();
}

}  // namespace dens
}  // namespace stanli

#endif
