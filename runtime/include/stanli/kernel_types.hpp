// Stable data layouts shared by the executor and native kernels. Keep Graph,
// Executor, and their payload types out: the expensive kernel translation
// units should not rebuild for changes to graph ownership or execution policy.
#ifndef STANLI_KERNEL_TYPES_HPP
#define STANLI_KERNEL_TYPES_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace stanli {

class WaRng;
class ReduceExecutionContext;

// Mutable state owned by one bound Executor and one operation. Most kernels
// need none; the retained loop keeps its tape here.
struct KernelState {
  virtual ~KernelState() = default;
  // Called on fresh state when copying an Executor. Reuse is optional: a
  // refusal must leave the destination fresh and the source untouched.
  // Implementations must relocate every mutable pointer into private storage.
  virtual bool clone_from(const KernelState&) { return false; }
};

// Per-evaluation resources that are neither graph structure nor arena state.
// The caller owns every pointed-to resource. In particular, an RNG stream
// belongs to one chain/drawing thread, never to a compiled model or executor.
struct EvalState {
  WaRng* wa_rng = nullptr;
  ReduceExecutionContext* reduce = nullptr;
  // Executor::forward_value_only(), CmdStan's log_prob<double>: a kernel may
  // skip work whose only consumer is its own backward, and takes the double
  // overload where Stan Math's double and var overloads differ.
  bool values_only = false;
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
  // Read from outside the graph after a forward run, such as a write_array
  // column, or filled once at bind. A forward-only executor never gives a
  // pinned slot's storage to another slot.
  bool pinned = false;
};

struct Op {
  uint16_t opcode = 0;
  // Opcode-specific compact mode, read and written through the opcode
  // family's typed view (DensityMode, SolverActivity, ...); other kernels use
  // it for contracts such as an RNG family.
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
  // CSE may share a pure forward value and scratch while retaining each
  // source pullback and its separate adjoint. Names the surviving output;
  // the survivor names itself. Fits the existing tail padding of Op.
  int32_t primal_source = -1;
};

// Op::variant of the general density kernels. Encoders write every field;
// a zero byte means no argument is autodiff, the summed lp, and no propto.
struct DensityMode {
  uint8_t active = 0;        // bit k: in[k] is autodiff (k < 6)
  bool elementwise = false;  // out[n] is element n's lp instead of the sum
  bool propto = false;       // drop terms constant in the active arguments

  static constexpr uint8_t kActiveBits = 0x3f;
  static constexpr uint8_t kElementwiseBit = 1u << 6;
  static constexpr uint8_t kProptoBit = 1u << 7;

  constexpr bool is_active(int k) const { return ((active >> k) & 1u) != 0; }
  constexpr uint8_t byte() const {
    return (uint8_t)((active & kActiveBits) |
                     (elementwise ? kElementwiseBit : 0u) |
                     (propto ? kProptoBit : 0u));
  }
  static constexpr DensityMode of(uint8_t byte) {
    return {(uint8_t)(byte & kActiveBits), (byte & kElementwiseBit) != 0,
            (byte & kProptoBit) != 0};
  }
  static constexpr DensityMode all_active(int n_args, bool propto = false) {
    return {(uint8_t)((1u << n_args) - 1u), false, propto};
  }
};

// How OP_SUM_VEC and OP_MEAN add their terms; the op's variant holds it.
// InOrder is how Matrix<var>, std::vector and strided Eigen views reduce.
// Packet is Eigen's packet grouping for a container whose first element is
// aligned; Phased is the same for a view that starts `offset` elements into
// one. Pairwise is for the sums that re-roll and partition build from a
// loop's target terms, which no Stan expression fixes the order of. None of
// them depends on the address of the slot.
enum class SumGrouping : uint8_t {
  InOrder = 0,
  Packet = 1,
  Phased = 2,
  Pairwise = 3
};

// Op::variant of OP_ODE, OP_ODE_ADJOINT, OP_DAE and OP_QUADRATURE: bit k says
// in[k] is autodiff. Runtime-control inputs are never active.
struct SolverActivity {
  uint8_t inputs = 0;

  constexpr bool is_active(int k) const { return ((inputs >> k) & 1u) != 0; }
  constexpr bool any() const { return inputs != 0; }
  constexpr uint8_t byte() const { return inputs; }
  static constexpr SolverActivity of(uint8_t byte) { return {byte}; }
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

// The evaluation's EvalState::values_only. A context run outside an
// evaluation (no EvalState) is a full forward.
inline bool values_only(const KernelCtx& ctx) {
  return ctx.eval_state != nullptr && ctx.eval_state->values_only;
}

// Both are copied per call on hot paths; the typed views above decode the
// existing byte rather than widening either struct.
static_assert(sizeof(void*) != 8 || sizeof(Op) == 80, "Op grew");
static_assert(sizeof(void*) != 8 || sizeof(KernelCtx) == 336, "KernelCtx grew");

inline DensityMode density_mode(const KernelCtx& c) {
  return DensityMode::of(c.variant);
}
inline DensityMode density_mode(const Op& op) {
  return DensityMode::of(op.variant);
}
// The same density call writing one lp per element.
inline uint8_t elementwise_density(uint8_t variant) {
  DensityMode mode = DensityMode::of(variant);
  mode.elementwise = true;
  return mode.byte();
}
inline SolverActivity solver_activity(const KernelCtx& c) {
  return SolverActivity::of(c.variant);
}
inline SolverActivity solver_activity(const Op& op) {
  return SolverActivity::of(op.variant);
}

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

// A matrix-vector product in a retained loop may also have runtime columns,
// flagged by idata[flag]. in[3] then holds the column count and in[4] the
// vector's live length.
inline int64_t live_product_cols(const KernelCtx& c, int64_t flag) {
  if (!c.dyn_lengths || c.n_idata <= flag || !c.idata[flag]) return c.idata[1];
  const double cols = c.in[3].data[0], rows = c.in[4].data[0];
  const int64_t live = static_cast<int64_t>(cols);
  if (!(cols >= 0) || cols > static_cast<double>(c.idata[1]) ||
      static_cast<double>(live) != cols)
    throw std::domain_error("logical extent exceeds graph capacity");
  if (rows != cols)
    throw std::invalid_argument("multiply: Columns of m1 (" +
                                std::to_string(live) + ") and Rows of m2 (" +
                                std::to_string(static_cast<int64_t>(rows)) +
                                ") must match in size");
  return live;
}

enum BwdMicroKind : uint8_t {
  kMicroAdd,
  kMicroSub,
  kMicroScaled,
  kMicroFn,
  kMicroFnNonzero,
};

struct BwdMicro {
  double* dst;
  const double* seed;
  const double* a;
  const double* b;
  double (*fn)(double seed, double a, double b);
  uint8_t kind;
  uint16_t opcode;
};

inline void run_bwd_micro(const BwdMicro& m) {
  switch (m.kind) {
    case kMicroAdd:
      *m.dst += *m.seed;
      break;
    case kMicroSub:
      *m.dst -= *m.seed;
      break;
    case kMicroScaled:
      if (m.b != nullptr && *m.b == 0.0) break;
      *m.dst += *m.seed * *m.a;
      break;
    case kMicroFn:
      *m.dst += m.fn(*m.seed, *m.a, *m.b);
      break;
    case kMicroFnNonzero:
      if (*m.a != 0.0) *m.dst += m.fn(*m.seed, *m.a, *m.b);
      break;
  }
}

inline BwdMicro bwd_micro(uint8_t kind, double* dst, const double* seed,
                          const double* a = nullptr, const double* b = nullptr,
                          double (*fn)(double, double, double) = nullptr) {
  return BwdMicro{dst, seed, a, b, fn, kind, 0};
}

// Appends the micro-ops that reproduce this context's backward exactly, in
// its order, and returns true; returns false and appends nothing when the
// backward is not of that form.
using CachedMicroFn = bool (*)(const KernelCtx&, std::vector<BwdMicro>&);

}  // namespace stanli

#endif
