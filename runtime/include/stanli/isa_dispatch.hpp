// Which copy of the dense-matrix kernels is registered. Only meaningful in a
// build with STANLI_AVX2_KERNELS=ON; see runtime/src/isa_dispatch.cpp.
#ifndef STANLI_ISA_DISPATCH_HPP
#define STANLI_ISA_DISPATCH_HPP

namespace stanli {

// True when register_matrix_kernels() chose the AVX2 copy. Valid once the
// kernels have been registered.
bool matrix_kernels_use_avx2();

}  // namespace stanli

#endif
