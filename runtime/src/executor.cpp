#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>
#include <stanli/packet.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace stanli {

static Kernel g_table[OP_COUNT_];

Kernel& kernel(uint16_t opcode) {
  assert(opcode < OP_COUNT_);
  return g_table[opcode];
}

// Default OFF, on measurement: see packet.hpp. Opt in with
// STANLI_PACKET_MATH=1.
static bool g_packet_math = [] {
  const char* e = std::getenv("STANLI_PACKET_MATH");
  return e != nullptr && e[0] != '0';
}();

bool packet_math() { return g_packet_math; }
void set_packet_math(bool on) { g_packet_math = on; }

// Set only for the duration of Executor::forward_value_only(). Kernels
// that read it must still write ctx.out; all they may skip is work whose
// only consumer is their own backward.
static thread_local bool g_values_only = false;
bool values_only() { return g_values_only; }

const char* opcode_name(uint16_t opcode) {
  static const char* const names[] = {
      "OP_NONE_",
#define STANLI_OPCODE_NAME(name) #name,
#define STANLI_DENSITY_OPCODE_NAME(code, fn, n, m) #code,
#define STANLI_UNARY_OPCODE_NAME(code, fn, value, delta, topology) #code,
      STANLI_ALL_OPCODES(STANLI_OPCODE_NAME, STANLI_DENSITY_OPCODE_NAME,
                         STANLI_UNARY_OPCODE_NAME)
#undef STANLI_OPCODE_NAME
#undef STANLI_DENSITY_OPCODE_NAME
#undef STANLI_UNARY_OPCODE_NAME
  };
  return opcode < OP_COUNT_ ? names[opcode] : "OP_?";
}

void register_kernel(uint16_t opcode, Kernel k) {
  assert(opcode < OP_COUNT_);
  g_table[opcode] = k;
}

static void ensure_registered();

const Kernel* find_kernel(uint16_t opcode) {
  ensure_registered();
  if (opcode >= OP_COUNT_) return nullptr;
  const Kernel& k = g_table[opcode];
  return k.forward ? &k : nullptr;
}

// CALL support (program.hpp): the register machine invoking a graph
// kernel. The context is assembled per call from the payload's ranges --
// every field is a pointer into the register file plus immediates, so
// this is loads and stores, no allocation.
KernelCtx call_fwd_ctx(const Program::Call& call, double* reg) {
  KernelCtx ctx;
  ctx.n_in = call.n_in;
  for (int k = 0; k < call.n_in; ++k)
    ctx.in[k] = Desc{reg + call.in[k], call.in_len[k]};
  ctx.out = Desc{reg + call.out, call.out_len};
  ctx.variant = call.variant;
  ctx.scratch = reg + call.scratch;
  ctx.idata = call.idata.data();
  ctx.n_idata = (int64_t)call.idata.size();
  return ctx;
}

void run_call(const Program::Call& call, double* reg) {
  KernelCtx ctx = call_fwd_ctx(call, reg);
  const Kernel* k = find_kernel(call.opcode);
  assert(k != nullptr);  // the carver only emits registered opcodes
  k->forward(ctx);
}

void register_elementwise_kernels();
void register_density_kernels();
void register_legacy_kernels();
void register_matrix_kernels();
void register_ode_kernels();
void register_constrain_kernels();
void register_eltwise_kernels();
void register_scalar_binary_kernels();
void register_scalar_unary_ad_kernels();
void register_mixture_kernels();
void register_message_kernels();
void register_island_kernel();

static void ensure_registered() {
  static const bool once = [] {
    register_elementwise_kernels();
    register_density_kernels();
    register_legacy_kernels();
    register_matrix_kernels();
    register_ode_kernels();
    register_constrain_kernels();
    register_message_kernels();
    register_eltwise_kernels();
    register_scalar_binary_kernels();
    register_scalar_unary_ad_kernels();
    register_mixture_kernels();
    register_island_kernel();
    return true;
  }();
  (void)once;
}

Executor::Executor(Graph g) : graph_(std::move(g)) {
  ensure_registered();
  bind_();
}

Executor::Executor(const Executor& src) : graph_(src.graph_) {
  ensure_registered();
  bind_();
  // bind_ zeroes the arena. The source's arena is where compile_model's
  // data and constant fills went, and the slot layout is a deterministic
  // function of the graph, so the two arenas agree element for element.
  // Copying into the existing buffer rather than assigning the vector
  // keeps the contexts' interior pointers valid by construction.
  std::copy(src.values_.begin(), src.values_.end(), values_.begin());
}

void Executor::bind_() {
  // Parameters first so the gradient vector is contiguous in declaration
  // order; then everything else.
  int64_t off = 0;
  for (auto& s : graph_.slots) {
    if (s.is_param) {
      s.offset = off;
      off += s.len;
    }
  }
  n_params_ = off;
  for (auto& s : graph_.slots) {
    if (!s.is_param) {
      s.offset = off;
      off += s.len;
    }
  }
  values_.assign(off, 0.0);

  // A slot carries adjoint if it is a parameter or an op writes it. Slots
  // that are neither are data: kernels see a null adjoint Desc and skip them.
  std::vector<char> written(graph_.slots.size(), 0);
  for (const auto& op : graph_.ops) {
    written[op.out] = 1;
    if (op.out2 >= 0) written[op.out2] = 1;
  }

  // Adjoint addresses never escape the executor, so unlike values they do
  // not need a hole for every externally addressable data slot or for slots
  // whose producer an optimization pass removed. Pack the active cells into
  // one arena. Keeping parameters first preserves the contiguous memcpy of
  // the returned gradient; keeping one arena preserves the single fast
  // memset on dense graphs.
  std::vector<int64_t> adjoint_offsets(graph_.slots.size(), -1);
  int64_t adj_off = 0;
  for (size_t i = 0; i < graph_.slots.size(); ++i) {
    const Slot& s = graph_.slots[i];
    if (s.is_param) {
      adjoint_offsets[i] = adj_off;
      adj_off += s.len;
    }
  }
  assert(adj_off == n_params_);
  for (size_t i = 0; i < graph_.slots.size(); ++i) {
    const Slot& s = graph_.slots[i];
    if (!s.is_param && (written[i] || (int)i == graph_.result_slot)) {
      adjoint_offsets[i] = adj_off;
      adj_off += s.len;
    }
  }
  adjoints_.assign(adj_off, 0.0);
  result_adjoint_offset_ = -1;
  if (graph_.result_slot >= 0) {
    result_adjoint_offset_ = adjoint_offsets[graph_.result_slot];
    assert(result_adjoint_offset_ >= 0);
  }

  int64_t scratch = 0;
  for (auto& op : graph_.ops) {
    const Kernel& k = kernel(op.opcode);
    if (k.forward == nullptr)
      // Name it. A browser build can be missing a kernel because its
      // density pack has not been loaded yet, and the caller decides what
      // to do from this string.
      throw std::runtime_error(std::string("opcode not registered: ") +
                               opcode_name(op.opcode));
    op.scratch_off = scratch;
    op.scratch_len =
        k.scratch_size ? k.scratch_size(op, graph_.slots.data()) : 0;
    scratch += op.scratch_len;
  }
  scratch_.assign(scratch, 0.0);

  // Assemble every kernel context once, now that all three arenas are sized
  // and every offset is final. Reassembling one per op per sweep cost a
  // scattered slot lookup per input and ~300 bytes of stores, twice per
  // gradient, which on the serial models (one op per observation, nothing to
  // vectorize) was a third of the time.
  ctx_.resize(graph_.ops.size());
  out2_adj_ptr_.assign(graph_.ops.size(), nullptr);
  for (size_t i = 0; i < graph_.ops.size(); ++i) {
    ctx_[i] = make_ctx_(graph_.ops[i], written, adjoint_offsets);
    const int o2 = graph_.ops[i].out2;
    if (o2 >= 0) {
      assert(adjoint_offsets[o2] >= 0);
      out2_adj_ptr_[i] = adjoints_.data() + adjoint_offsets[o2];
    }
  }
  // Resolve dispatch now that ctx_ is final (it never reallocates after
  // this, so BwdStep may hold pointers into it).
  fwd_fn_.resize(graph_.ops.size());
  bwd_.clear();
  bwd_.reserve(graph_.ops.size());
  for (size_t i = 0; i < graph_.ops.size(); ++i)
    fwd_fn_[i] = kernel(graph_.ops[i].opcode).forward;
  for (size_t i = graph_.ops.size(); i-- > 0;) {
    void (*b)(KernelCtx&) = kernel(graph_.ops[i].opcode).backward;
    if (b) bwd_.push_back(BwdStep{b, &ctx_[i], out2_adj_ptr_[i]});
  }
}

KernelCtx Executor::make_ctx_(const Op& op, const std::vector<char>& written,
                              const std::vector<int64_t>& adjoint_offsets) {
  KernelCtx ctx;
  ctx.n_in = op.n_in;
  for (int i = 0; i < op.n_in; ++i) {
    const Slot& s = graph_.slots[op.in[i]];
    ctx.in[i] = Desc{values_.data() + s.offset, s.len};
  }
  const Slot& so = graph_.slots[op.out];
  ctx.out = Desc{values_.data() + so.offset, so.len};
  if (op.out2 >= 0) {
    const Slot& s2 = graph_.slots[op.out2];
    ctx.out2 = Desc{values_.data() + s2.offset, s2.len};
  }
  ctx.variant = op.variant;
  ctx.scratch = scratch_.data() + op.scratch_off;
  ctx.idata = op.idata;
  ctx.udata = op.udata;
  ctx.n_idata = op.n_idata;
  for (int i = 0; i < op.n_in; ++i) {
    const int si = op.in[i];
    const Slot& s = graph_.slots[si];
    const bool active = s.is_param || written[si];
    assert(!active || adjoint_offsets[si] >= 0);
    ctx.in_adj[i] =
        Desc{active ? adjoints_.data() + adjoint_offsets[si] : nullptr, s.len};
  }
  assert(adjoint_offsets[op.out] >= 0);
  const int64_t out_adj_off = adjoint_offsets[op.out];
  if (so.len == 1) ctx.out_adj = adjoints_[out_adj_off];
  ctx.out_adj_vec = Desc{adjoints_.data() + out_adj_off, so.len};
  if (op.out2 >= 0) {
    assert(adjoint_offsets[op.out2] >= 0);
    ctx.out2_adj = adjoints_[adjoint_offsets[op.out2]];
  }
  return ctx;
}

void Executor::set_profile(bool on) {
  profile_ = on;
  if (on && prof_.empty()) prof_.resize(OP_COUNT_);
}

std::string Executor::profile_report() const {
  int64_t grand = 0;
  for (const auto& e : prof_) grand += e.fwd_ns + e.bwd_ns;
  if (grand == 0) return "";
  // Opcodes by total time, descending.
  std::vector<uint16_t> order;
  for (uint16_t op = 0; op < prof_.size(); ++op)
    if (prof_[op].calls > 0) order.push_back(op);
  std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) {
    return prof_[a].fwd_ns + prof_[a].bwd_ns >
           prof_[b].fwd_ns + prof_[b].bwd_ns;
  });
  char line[160];
  std::string out;
  std::snprintf(line, sizeof line, "%-22s %10s %12s %12s %6s %12s\n", "opcode",
                "calls", "fwd ns", "bwd ns", "%", "elems");
  out += line;
  for (uint16_t op : order) {
    const ProfEntry& e = prof_[op];
    std::snprintf(line, sizeof line,
                  "%-22s %10lld %12lld %12lld %5.1f%% %12lld\n",
                  opcode_name(op), (long long)e.calls, (long long)e.fwd_ns,
                  (long long)e.bwd_ns,
                  100.0 * (double)(e.fwd_ns + e.bwd_ns) / (double)grand,
                  (long long)e.elems);
    out += line;
  }
  std::snprintf(line, sizeof line, "%-22s %10s %12lld ns total\n", "", "",
                (long long)grand);
  out += line;
  return out;
}

void Executor::run_forward_only() {
  // The profiled path keeps the opcode-keyed loop (attribution needs the
  // opcode anyway, and the timing calls dwarf dispatch cost).
  if (profile_) {
    const size_t np = graph_.ops.size();
    for (size_t i = 0; i < np; ++i) {
      const uint16_t op = graph_.ops[i].opcode;
      const auto t0 = std::chrono::steady_clock::now();
      kernel(op).forward(ctx_[i]);
      const auto t1 = std::chrono::steady_clock::now();
      ProfEntry& e = prof_[op];
      ++e.calls;
      e.fwd_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      e.elems += ctx_[i].out.len;
    }
    return;
  }
  // Unrolled by hand: four separate indirect-call sites predict
  // independently, and the loop bookkeeping amortizes. Measured against a
  // musttail-chained alternative in tools/bench_dispatch.cpp -- the unroll
  // won (79-80% of the plain loop vs 85-95%, and no kernel signature
  // changes), so this is the whole of "threaded dispatch" worth having.
  const size_t n = fwd_fn_.size();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    fwd_fn_[i](ctx_[i]);
    fwd_fn_[i + 1](ctx_[i + 1]);
    fwd_fn_[i + 2](ctx_[i + 2]);
    fwd_fn_[i + 3](ctx_[i + 3]);
  }
  for (; i < n; ++i) fwd_fn_[i](ctx_[i]);
}

double Executor::forward_value_only() {
  struct ValuesOnly {
    ValuesOnly() { g_values_only = true; }
    ~ValuesOnly() { g_values_only = false; }
  } guard;
  return forward();
}

double Executor::forward() {
  run_forward_only();
  const Slot& r = graph_.slots[graph_.result_slot];
  assert(r.len == 1);
  return values_[r.offset];
}

double Executor::gradient(double* grad_out) {
  ++n_grad_evals_;
  const double v = forward();
  std::memset(adjoints_.data(), 0, sizeof(double) * adjoints_.size());
  assert(result_adjoint_offset_ >= 0);
  adjoints_[result_adjoint_offset_] = 1.0;
  if (profile_) {
    for (size_t pi = graph_.ops.size(); pi-- > 0;) {
      const Kernel& k = kernel(graph_.ops[pi].opcode);
      if (!k.backward) continue;
      KernelCtx& ctx = ctx_[pi];
      if (ctx.out_adj_vec.len == 1) ctx.out_adj = ctx.out_adj_vec.data[0];
      if (out2_adj_ptr_[pi]) ctx.out2_adj = *out2_adj_ptr_[pi];
      const auto t0 = std::chrono::steady_clock::now();
      k.backward(ctx);
      prof_[graph_.ops[pi].opcode].bwd_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - t0)
              .count();
    }
    std::memcpy(grad_out, adjoints_.data(), sizeof(double) * n_params_);
    return v;
  }
  // Same unroll as the forward sweep (see run_forward_only).
  const auto step = [](const BwdStep& s) {
    KernelCtx& ctx = *s.ctx;
    // The only fields that move between evaluations: the scalar adjoints,
    // which kernels take by value.
    if (ctx.out_adj_vec.len == 1) ctx.out_adj = ctx.out_adj_vec.data[0];
    if (s.out2_adj) ctx.out2_adj = *s.out2_adj;
    s.fn(ctx);
  };
  const size_t nb = bwd_.size();
  size_t i = 0;
  for (; i + 4 <= nb; i += 4) {
    step(bwd_[i]);
    step(bwd_[i + 1]);
    step(bwd_[i + 2]);
    step(bwd_[i + 3]);
  }
  for (; i < nb; ++i) step(bwd_[i]);
  std::memcpy(grad_out, adjoints_.data(), sizeof(double) * n_params_);
  return v;
}

}  // namespace stanli
