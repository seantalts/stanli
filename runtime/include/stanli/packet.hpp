// Packet (varmat-style) kernel math.
//
// CmdStan's default AoS `Matrix<var>` rev overloads evaluate their
// transcendentals over strided `.val()` expressions that Eigen cannot
// packet-vectorize, so they run scalar libm per element and reduce
// sequentially. The kernels mirrored that exactly, which bought bitwise
// parity with default CmdStan at the cost of leaving Eigen's packet math
// unused. stan-math's varmat overloads (`var_value<Matrix>`, which
// `stanc --O1` emits wherever a variable's whole use chain allows it)
// compute over contiguous doubles instead and do vectorize.
//
// Enabling packet math here is the same trade in stanli's terms: the
// reference becomes `stanc --O1` CmdStan rather than default CmdStan, and
// affected gradients move from bitwise to a low-ULP match. Measured gains
// per element (Apple M-series): exp 2.05x,
// inv_logit 1.93x, sum 4.3x, log 1.25x.
//
// OFF by default, and that default is a measurement, not caution. Per
// element the packet forms are 2-4x faster, but at whole-model scale the
// constrain and reduction work they touch is not where gradient time
// goes: bym2_offset_only (3845 constrained params) measures 41.4 us with
// packet math and 41.3 us without, lsat_model 47.0 vs 47.3 us -- both
// inside noise. Meanwhile it costs parity: up to 10 ULP against default
// CmdStan on the dfold fixture, and 1.4e-13 worst relative deviation
// across the corpus versus scalar mode.
//
// The infrastructure stays because the trade becomes worthwhile once the
// DENSITY path is converted -- that is where the time actually is, and it
// is the part `stanc --O1` varmat builds accelerate too. Opt in with
// STANLI_PACKET_MATH=1.
#ifndef STANLI_PACKET_HPP
#define STANLI_PACKET_HPP

namespace stanli {

// Read once from the environment on first call.
bool packet_math();

// True while Executor::forward_value_only() is running: a kernel may
// skip work whose only consumer is its own backward. See graph.hpp.
bool values_only();

// Test-only override. The fixture tests that pin bitwise/low-ULP parity
// with DEFAULT CmdStan are assertions about the AoS mirror specifically,
// so they turn packet math off rather than widen their tolerance; the
// packet path's reference is `stanc --O1` CmdStan, checked separately.
void set_packet_math(bool on);

// True unless a test turns it off: 1x1 and 2x2 matrix kernels then run on
// fixed-size Eigen types. Off selects the dynamic-size code, the reference
// the fixed paths must match bit for bit.
bool fixed_small_matrices();
void set_fixed_small_matrices(bool on);

}  // namespace stanli

#endif
