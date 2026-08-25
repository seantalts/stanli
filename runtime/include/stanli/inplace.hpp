// Destructive functional updates. `v[n] = ...` under an unrolled loop and
// re-rolled slice stores both copy the whole base into a fresh slot. When a
// write is the last use of the base it overwrites, it can mutate that buffer
// in place instead.
#ifndef STANLI_INPLACE_HPP
#define STANLI_INPLACE_HPP

#include <stanli/graph.hpp>

#include <vector>

namespace stanli {

// Rewrites eligible OP_SET_INDEX / OP_SET_SLICE / OP_SET_SLICE_STRIDED ops
// to their INPLACE forms (whose out slot IS their first input) and renames
// later references to the dead output slot. `roots` are slots read from
// outside the op graph (jacobian terms, constrained-parameter views, the
// result): they are never overwritten and never renamed. Returns the number
// of writes rewritten. STANLI_NO_INPLACE=1 disables the pass.
// True for ops whose backward only routes adjoints or reads scratch, never
// input OR output value buffers. The destructive rewrite is sound only when
// both earlier readers and the producer of the current buffer version satisfy
// this strong property. Exposed so tests/test_pass_safety.cpp can verify every
// claimed opcode by poisoning all values between the sweeps.
bool backward_ignores_values(uint16_t opcode);
// Compatibility spelling retained for callers of the original input-only
// description. The actual contract is now deliberately stronger.
bool backward_ignores_input_values(uint16_t opcode);

int make_inplace_updates(Graph& g, const std::vector<int>& roots);

// Store-to-load forwarding, plus the dead ops it exposes. `mu[n] = e;` and
// a read of `mu[n]` in the same iteration lower to a write immediately
// followed by an OP_INDEX of the element just written; the read is
// replaced by the written value directly. When nothing then reads the
// vector, its writes are dead and go too, which is what leaves the plain
// per-lane arithmetic the re-roll pass can vectorize. Returns the number
// of ops removed. Disabled with the writes themselves under
// STANLI_NO_INPLACE.
int forward_stores_to_loads(Graph& g, const std::vector<int>& roots);

// Drops slice stores that overwrite their whole destination: the value the
// store writes IS the vector its readers want, so they read it directly.
// Applies to the four OP_SET_SLICE forms with start 0, unit stride and a
// value as long as the destination. Returns the number of stores dropped.
// Disabled under STANLI_NO_INPLACE.
int elide_full_extent_stores(Graph& g, const std::vector<int>& roots);

}  // namespace stanli

#endif
