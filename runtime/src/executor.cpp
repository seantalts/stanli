#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
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
      STANLI_OPCODE_LIST(STANLI_OPCODE_NAME)
#undef STANLI_OPCODE_NAME
#define STANLI_DENSITY_OPCODE_NAME(code, fn, n, m) #code,
      STANLI_SCALAR_DENSITY_LIST(STANLI_DENSITY_OPCODE_NAME)
      STANLI_INT_DENSITY_LIST(STANLI_DENSITY_OPCODE_NAME)
      STANLI_SCALAR_CDF_LIST(STANLI_DENSITY_OPCODE_NAME)
      STANLI_INT_CDF_LIST(STANLI_DENSITY_OPCODE_NAME)
      STANLI_ORDERED_DENSITY_LIST(STANLI_DENSITY_OPCODE_NAME)
#undef STANLI_DENSITY_OPCODE_NAME
#define STANLI_UNARY_OPCODE_NAME(code, fn, v, d) #code,
      STANLI_SCALAR_UNARY_LIST(STANLI_UNARY_OPCODE_NAME)
#undef STANLI_UNARY_OPCODE_NAME
  };
  return opcode < OP_COUNT_ ? names[opcode] : "OP_?";
}

void register_kernel(uint16_t opcode, Kernel k) {
  assert(opcode < OP_COUNT_);
  g_table[opcode] = k;
}

void register_elementwise_kernels();
void register_density_kernels();
void register_legacy_kernels();
void register_matrix_kernels();
void register_ode_kernels();
void register_constrain_kernels();
void register_eltwise_kernels();
void register_mixture_kernels();
void register_island_kernel();

static void ensure_registered() {
  static const bool once = [] {
    register_elementwise_kernels();
    register_density_kernels();
    register_legacy_kernels();
    register_matrix_kernels();
    register_ode_kernels();
    register_constrain_kernels();
    register_eltwise_kernels();
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
  arena_len_ = off;
  values_.assign(arena_len_, 0.0);
  adjoints_.assign(arena_len_, 0.0);

  // A slot carries adjoint if it is a parameter or an op writes it. Slots
  // that are neither are data: kernels see a null adjoint Desc and skip them.
  written_.assign(graph_.slots.size(), 0);
  for (const auto& op : graph_.ops) {
    written_[op.out] = 1;
    if (op.out2 >= 0) written_[op.out2] = 1;
  }

  int64_t scratch = 0;
  for (auto& op : graph_.ops) {
    const Kernel& k = kernel(op.opcode);
    if (k.forward == nullptr)
      throw std::runtime_error("opcode not registered: " +
                               std::to_string(op.opcode));
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
    ctx_[i] = make_ctx_(graph_.ops[i]);
    const int o2 = graph_.ops[i].out2;
    if (o2 >= 0) out2_adj_ptr_[i] = adjoints_.data() + graph_.slots[o2].offset;
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

KernelCtx Executor::make_ctx_(const Op& op) {
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
    const bool active = s.is_param || written_[si];
    ctx.in_adj[i] =
        Desc{active ? adjoints_.data() + s.offset : nullptr, s.len};
  }
  if (so.len == 1) ctx.out_adj = adjoints_[so.offset];
  ctx.out_adj_vec = Desc{adjoints_.data() + so.offset, so.len};
  if (op.out2 >= 0) ctx.out2_adj = adjoints_[graph_.slots[op.out2].offset];
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
  std::snprintf(line, sizeof line, "%-22s %10s %12s %12s %6s %12s\n",
                "opcode", "calls", "fwd ns", "bwd ns", "%", "elems");
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
      e.fwd_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                      t1 - t0).count();
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
  g_values_only = true;
  try {
    run_forward_only();
  } catch (...) {
    g_values_only = false;
    throw;
  }
  g_values_only = false;
  const Slot& r = graph_.slots[graph_.result_slot];
  assert(r.len == 1);
  return values_[r.offset];
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
  adjoints_[graph_.slots[graph_.result_slot].offset] = 1.0;
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
              std::chrono::steady_clock::now() - t0).count();
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
