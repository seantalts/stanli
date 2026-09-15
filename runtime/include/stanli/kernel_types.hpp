// Stable data layouts shared by the executor and native kernels. Keep Graph,
// Executor, and their payload types out: the expensive kernel translation
// units should not rebuild for changes to graph ownership or execution policy.
#ifndef STANLI_KERNEL_TYPES_HPP
#define STANLI_KERNEL_TYPES_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace stanli {

class WaRng;

// Mutable state owned by one bound Executor and one operation. Most kernels
// need none; the retained loop keeps its tape here.
struct KernelState {
  virtual ~KernelState() = default;
};

// Per-evaluation resources that are neither graph structure nor arena state.
// The caller owns every pointed-to resource. In particular, an RNG stream
// belongs to one chain/drawing thread, never to a compiled model or executor.
struct EvalState {
  WaRng* wa_rng = nullptr;
};

// A view of one contiguous buffer. len == 1 means scalar.
struct Desc {
  double* data;
  int64_t len;
};

// A value in the graph. Slots with is_param are the unconstrained parameter
// vector, in declaration order; everything else is data or an intermediate.
struct Slot {
  int64_t offset = 0;  // within its bound value buffer (filled at bind)
  int64_t len = 0;
  bool is_param = false;
};

struct Op {
  uint16_t opcode = 0;
  // Opcode-specific compact mode. Density kernels use bits 0..5 for
  // per-argument activity, bit 6 for elementwise lp, and bit 7 for propto;
  // other kernels use it for contracts such as ODE scalar types or RNG family.
  uint8_t variant = 0;
  int out = -1;
  int out2 = -1;  // optional second output (e.g. constrain jacobian term)
  int in[6] = {-1, -1, -1, -1, -1, -1};
  int n_in = 0;
  const int* idata = nullptr;  // integer immediates (outcome counts, dims)
  int64_t n_idata = 0;
  // Opaque per-op payload for kernels that need compile-time structure the
  // integer immediates cannot carry (ODEs, messages, declaration checks).
  const void* udata = nullptr;
  // Operands that are logical views into a fixed capacity. dyn_extent_in
  // names the input holding the live length; dyn_lengths names the operands
  // it applies to. Storage stays at dyn_capacity throughout.
  int64_t dyn_capacity = 0;
  int8_t dyn_extent_in = -1;
  uint8_t dyn_lengths = 0;
};

// Bit 6 of a dynamic-length mask names the output; bits 0..5 name inputs.
inline constexpr uint8_t kDynamicLengthOutput = 1u << 6;
static_assert(sizeof(Op::in) / sizeof(Op::in[0]) == 6,
              "a dynamic-length mask has one bit per input");

// Per-call view handed to kernels. Assembled by the executor; kernels never
// see slots or arenas directly.
struct KernelCtx {
  Desc in[6];
  int n_in = 0;
  Desc out{nullptr, 0};
  uint8_t variant = 0;
  double* scratch = nullptr;
  const int* idata = nullptr;
  int64_t n_idata = 0;
  const void* udata = nullptr;
  EvalState* eval_state = nullptr;
  KernelState* state = nullptr;
  Desc out2{nullptr, 0};  // second output value (scalar), if any
  // Backward only. Data inputs get {nullptr, len}: kernels skip them.
  Desc in_adj[6];
  double out_adj = 0;            // scalar-output ops
  Desc out_adj_vec{nullptr, 0};  // vector-output ops
  double out2_adj = 0;           // adjoint of the second output
  int64_t dyn_capacity = 0;
  int8_t dyn_extent_in = -1;
  uint8_t dyn_lengths = 0;
};

inline void apply_dynamic_length(KernelCtx& c) {
  assert(c.in[c.dyn_extent_in].len == 1);
  const double raw = c.in[c.dyn_extent_in].data[0];
  const int64_t live = static_cast<int64_t>(raw);
  if (!(raw >= 0) || raw > static_cast<double>(c.dyn_capacity) ||
      static_cast<double>(live) != raw)
    throw std::domain_error("logical extent exceeds graph capacity");
  for (size_t k = 0; k < sizeof(c.in) / sizeof(c.in[0]); ++k)
    if (c.dyn_lengths & (1u << k)) {
      c.in[k].len = live;
      c.in_adj[k].len = live;
    }
  if (c.dyn_lengths & kDynamicLengthOutput) {
    c.out.len = live;
    c.out_adj_vec.len = live;
  }
  c.n_in = c.dyn_extent_in;
}

}  // namespace stanli

#endif
