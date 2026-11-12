// Evaluation-mode flags the kernels consult.
#ifndef STANLI_PACKET_HPP
#define STANLI_PACKET_HPP

namespace stanli {

// True while Executor::forward_value_only() is running: a kernel may
// skip work whose only consumer is its own backward. That function and its
// callers are documented in graph.hpp; kernels consult only this flag.
bool values_only();

// True unless a test turns it off: 1x1 and 2x2 matrix kernels then run on
// fixed-size Eigen types. Off selects the dynamic-size code, the reference
// the fixed paths must match bit for bit.
bool fixed_small_matrices();
void set_fixed_small_matrices(bool on);

}  // namespace stanli

#endif
