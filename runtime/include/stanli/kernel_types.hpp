// Stable data layouts shared by the executor and native kernels. Keep Graph,
// Executor, and their payload types out: the expensive kernel translation
// units should not rebuild for changes to graph ownership or execution policy.
#ifndef STANLI_KERNEL_TYPES_HPP
#define STANLI_KERNEL_TYPES_HPP

#include <cstdint>

namespace stanli {

class WaRng;

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
  int64_t offset = 0;  // into the value arena (filled at bind)
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
};

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
  Desc out2{nullptr, 0};  // second output value (scalar), if any
  // Backward only. Data inputs get {nullptr, len}: kernels skip them.
  Desc in_adj[6];
  double out_adj = 0;            // scalar-output ops
  Desc out_adj_vec{nullptr, 0};  // vector-output ops
  double out2_adj = 0;           // adjoint of the second output
};

}  // namespace stanli

#endif
