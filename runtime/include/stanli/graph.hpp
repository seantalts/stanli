// Structured op graph: the runtime's IR and, during reverse mode, its tape.
// Lowering (lower.cpp) emits it from stanc3's transformed MIR; tests build
// the same structure programmatically.
#ifndef STANLI_GRAPH_HPP
#define STANLI_GRAPH_HPP

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace stanli {

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
  uint8_t variant = 0;  // density kernels: bits 0..5 per-arg activity
                        // (1 = autodiff), bit 6 = elementwise lp (out is
                        // len N, out[n] = element n's lp), bit 7 = propto
  int out = -1;
  int out2 = -1;  // optional second output (e.g. constrain jacobian term)
  int in[6] = {-1, -1, -1, -1, -1, -1};
  int n_in = 0;
  const int* idata = nullptr;  // integer immediates (outcome counts, dims)
  int64_t n_idata = 0;
  // Opaque per-op payload for kernels that need compile-time structure the
  // integer immediates cannot carry (ODEs, messages, declaration checks).
  const void* udata = nullptr;
  int64_t scratch_off = 0;  // into the scratch arena (filled at bind)
  int64_t scratch_len = 0;
};

struct Graph {
  std::vector<Slot> slots;
  std::vector<Op> ops;
  std::vector<std::vector<int>> idata_pool;  // owns per-op integer arrays
  // Owns per-op opaque payloads; pointers into this outlive lowering because
  // the graph is moved, never copied element-wise.
  std::vector<std::shared_ptr<void>> udata_pool;
  int result_slot = -1;

  int add_slot(int64_t len, bool is_param) {
    slots.push_back(Slot{0, len, is_param});
    return static_cast<int>(slots.size()) - 1;
  }

  int add_op(uint16_t opcode, std::initializer_list<int> ins, int out,
             std::vector<int> idata = {}) {
    Op op;
    op.opcode = opcode;
    op.out = out;
    op.n_in = 0;
    // Op::in is fixed-size and this used to write past it without a
    // word: a 7-input op corrupted n_in and whatever followed, and the
    // failure surfaced as a SIGBUS inside the kernel rather than here.
    // Six is a real ceiling on the lowering, so say so at the point that
    // knows.
    if (ins.size() > sizeof(op.in) / sizeof(op.in[0]))
      throw std::length_error("op has more inputs than Op::in holds");
    for (int s : ins) op.in[op.n_in++] = s;
    if (!idata.empty()) {
      idata_pool.push_back(std::move(idata));
      op.idata = idata_pool.back().data();
      op.n_idata = static_cast<int64_t>(idata_pool.back().size());
    }
    ops.push_back(op);
    return static_cast<int>(ops.size()) - 1;
  }
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
  Desc out2{nullptr, 0};  // second output value (scalar), if any
  // Backward only. Data inputs get {nullptr, len}: kernels skip them.
  Desc in_adj[6];
  double out_adj = 0;            // scalar-output ops
  Desc out_adj_vec{nullptr, 0};  // vector-output ops
  double out2_adj = 0;           // adjoint of the second output
};

// Payload for OP_REJECT and OP_PRINT: the literal chunks of the message,
// interleaved with the op's inputs at forward time. Chunk k precedes
// input k; a trailing chunk with no input after it is just appended.
struct MessageSpec {
  std::vector<std::string> chunks;
};

// Payload for generated runtime bound and dimension checks.
struct BoundCheckSpec {
  std::string name;
  bool bound_is_scalar = false;
  bool shapes_match = false;
};

// The two categorical families share one exact value/check/pullback op.
// `scalar_outcome` is a language type distinction: array[1] int must select
// Stan Math's vector overload even though its flat slot also has length one.
struct CategoricalSpec {
  bool logit = false;
  bool scalar_outcome = false;
  // These are independent: write_array values depend on q but instantiate on
  // double, while a graph-constant AutoDiffable local instantiates on var.
  bool arg_autodiff = false;
  bool propto = true;  // template flag; Stan Math decides what it can drop
};

class Executor {
 public:
  explicit Executor(Graph g);
  // Copy: the same graph and the same arena CONTENTS, with fresh contexts
  // bound to this instance's own arenas. The arena is what carries the
  // data and constant fills, so copying it is what makes the clone a
  // bound model rather than an empty one -- binding alone zeroes it.
  //
  // Multi-chain sampling is what this is for: one executor per chain,
  // because the arenas are per-evaluation mutable state, and copying an
  // op list is far cheaper than lowering the model again. Assignment
  // stays deleted; the contexts hold interior pointers, so a moved-from
  // or reassigned executor would be a dangling one.
  Executor(const Executor& src);
  Executor& operator=(const Executor&) = delete;

  int64_t n_params() const { return n_params_; }
  // Number of doubles in the reverse-mode arena. This is intentionally
  // observable: inactive data and slots left behind by graph rewrites must
  // not silently return to the per-gradient clear path.
  int64_t adjoint_storage_size() const {
    return static_cast<int64_t>(adjoints_.size());
  }
  // The bound graph, so a second executor over the same model can be
  // built without re-lowering. Multi-chain sampling needs one executor
  // per chain -- the arenas are mutable per-evaluation state -- and
  // copying the op list is far cheaper than compiling the model again.
  const Graph& graph() const { return graph_; }
  // The unconstrained parameter vector: the first n_params() arena entries,
  // in parameter-slot declaration order.
  double* params_data() { return values_.data(); }
  double* param_ptr(int slot) {
    return values_.data() + graph_.slots[slot].offset;
  }
  double* value_ptr(int slot) {
    return values_.data() + graph_.slots[slot].offset;
  }

  // Forward through all ops; returns value of result_slot (must be scalar).
  double forward();
  // The value from CmdStan's log_prob<double> instantiation. Kernels may
  // skip partials or select a double-only overload. OP_CATEGORICAL thereby
  // observes propto's compile-time scalar type, while OP_ODE solves states
  // alone instead of the coupled state-plus-sensitivity system. Use this
  // where CmdStan uses the double path, chiefly when deciding whether an
  // initial point is valid.
  //
  // Safe to interleave with gradient(): that always runs a full forward
  // first, so nothing stale survives into a reverse sweep.
  double forward_value_only();
  // Forward for graphs whose result is not a scalar (tests only).
  void run_forward_only();
  // forward() + reverse sweep. grad_out receives d result / d params in
  // param-slot declaration order. Returns the forward value.
  double gradient(double* grad_out);
  int64_t n_grad_evals() const { return n_grad_evals_; }

  // Opt-in per-opcode accounting (calls, forward/backward ns, elements).
  // Off by default and off the fast path when off; STANLI_PROFILE=1 makes
  // the CLI tools enable it and print the report. Toggling off stops
  // accumulation but keeps what was collected.
  void set_profile(bool on);
  // Human-readable table sorted by total time; empty when nothing was
  // collected.
  std::string profile_report() const;

 private:
  void bind_();
  KernelCtx make_ctx_(const Op& op, const std::vector<char>& written,
                      const std::vector<int64_t>& adjoint_offsets);

  struct ProfEntry {
    int64_t calls = 0;  // forward invocations
    int64_t fwd_ns = 0;
    int64_t bwd_ns = 0;
    int64_t elems = 0;  // output elements per forward call, summed
  };

  Graph graph_;
  std::vector<double> values_;
  std::vector<double> adjoints_;
  int64_t result_adjoint_offset_ = -1;
  std::vector<double> scratch_;
  // One context per op, assembled once at bind. Every field in it is a
  // pointer into an arena that never moves after binding, or an immediate
  // copied from the op, so the only per-evaluation work is refreshing the
  // two scalar adjoints the reverse sweep passes by value.
  std::vector<KernelCtx> ctx_;
  // The dispatch tables, resolved at bind. The sweeps walk these instead
  // of reading each op's opcode and indexing the global kernel table:
  // per op that saved an opcode load, a table index, a 3-pointer Kernel
  // struct load, and (backward) a null test, which is a real fraction of
  // the ~5 ns of executor overhead that sits on top of a small kernel.
  std::vector<void (*)(KernelCtx&)> fwd_fn_;  // parallel to ctx_
  // Reverse execution order, ops with no backward already dropped.
  struct BwdStep {
    void (*fn)(KernelCtx&);
    KernelCtx* ctx;
    const double* out2_adj;  // null when the op has no second output
  };
  std::vector<BwdStep> bwd_;
  std::vector<double*> out2_adj_ptr_;  // parallel to ops; null when no out2
  bool profile_ = false;
  std::vector<ProfEntry> prof_;  // indexed by opcode; empty until enabled
  int64_t n_grad_evals_ = 0;
  int64_t n_params_ = 0;
};

}  // namespace stanli

#endif
