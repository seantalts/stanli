#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>
#include <stanli/packet.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace stanli {

using ForwardFn = void (*)(KernelCtx&);
ForwardFn resolve_forward_fn(const Op& op);

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

// CALL support (program.hpp): the register machine invoking a graph kernel.
// Only pointer fields vary with a register-file base. Dispatch and the
// integer/shape metadata are immutable call-site facts, so builders resolve
// the former once and a program invocation reuses one context packet across
// all of its CALL instructions.
static void bind_call_fwd_ctx(const Program::Call& call, double* reg,
                              KernelCtx& ctx, EvalState* state) {
  ctx.n_in = call.n_in;
  for (int k = 0; k < call.n_in; ++k)
    ctx.in[k] = Desc{reg + call.in[k], call.in_len[k]};
  ctx.out = Desc{reg + call.out, call.out_len};
  ctx.variant = call.variant;
  ctx.scratch = reg + call.scratch;
  ctx.idata = call.idata.data();
  ctx.n_idata = (int64_t)call.idata.size();
  ctx.udata = call.udata_owner.get();
  // Not a call-site fact: the draw stream belongs to the evaluation, so it
  // is rebound with the pointer fields rather than resolved once.
  ctx.eval_state = state;
}

KernelCtx call_fwd_ctx(const Program::Call& call, double* reg) {
  KernelCtx ctx;
  bind_call_fwd_ctx(call, reg, ctx, nullptr);
  return ctx;
}

bool bind_call(Program::Call& call) {
  call.forward = nullptr;
  call.backward = nullptr;
  const Kernel* k = find_kernel(call.opcode);
  if (k == nullptr || k->forward == nullptr) return false;
  call.forward = k->forward;
  call.backward = k->backward;
  return true;
}

int64_t kernel_call_scratch(int64_t (*scratch_size)(const Op&, const Slot*),
                            uint16_t opcode, uint8_t variant, int8_t n_in,
                            const int32_t* in_len, int32_t out_len,
                            const int* idata, int64_t n_idata,
                            const void* udata) {
  if (scratch_size == nullptr) return 0;
  Op op;
  op.opcode = opcode;
  op.variant = variant;
  op.n_in = n_in;
  op.out = n_in;
  op.idata = idata;
  op.n_idata = n_idata;
  op.udata = udata;
  std::vector<Slot> slots((size_t)n_in + 1);
  for (int k = 0; k < n_in; ++k) {
    op.in[k] = k;
    slots[(size_t)k].len = in_len[k];
  }
  slots.back().len = out_len;
  return scratch_size(op, slots.data());
}

void run_call_var(const Program::Call& call, stan::math::var* reg) {
  using ArenaDoubles = stan::arena_t<std::vector<double>>;
  using ArenaVaris = stan::arena_t<std::vector<stan::math::vari*>>;
  if (call.forward == nullptr || call.backward == nullptr)
    throw std::logic_error("unbound Program::CALL var replay");

  std::array<int32_t, 6> in_offset{};
  int32_t total = 0;
  for (int k = 0; k < call.n_in; ++k) {
    in_offset[(size_t)k] = total;
    total += call.in_len[k];
  }
  const int32_t out_offset = total;
  total += call.out_len;
  const int32_t scratch_offset = total;
  total += call.scratch_len;

  ArenaDoubles values((size_t)total, 0.0);
  ArenaDoubles adjoints((size_t)total, 0.0);
  ArenaVaris input_varis;
  input_varis.reserve((size_t)out_offset);
  for (int k = 0; k < call.n_in; ++k) {
    for (int i = 0; i < call.in_len[k]; ++i) {
      const stan::math::var& x = reg[(size_t)(call.in[k] + i)];
      values[(size_t)(in_offset[(size_t)k] + i)] = x.val();
      input_varis.push_back(x.vi_);
    }
  }

  KernelCtx ctx;
  double* const value_base = values.empty() ? nullptr : values.data();
  ctx.n_in = call.n_in;
  for (int k = 0; k < call.n_in; ++k)
    ctx.in[k] = Desc{value_base ? value_base + in_offset[(size_t)k] : nullptr,
                     call.in_len[k]};
  ctx.out = Desc{value_base ? value_base + out_offset : nullptr, call.out_len};
  ctx.variant = call.variant;
  ctx.scratch = value_base ? value_base + scratch_offset : nullptr;
  ctx.idata = call.idata.data();
  ctx.n_idata = (int64_t)call.idata.size();
  ctx.udata = call.udata_owner.get();
  call.forward(ctx);

  ArenaVaris output_varis((size_t)call.out_len);
  for (int i = 0; i < call.out_len; ++i) {
    reg[(size_t)(call.out + i)] =
        stan::math::var(values[(size_t)(out_offset + i)]);
    output_varis[(size_t)i] = reg[(size_t)(call.out + i)].vi_;
  }

  const KernelFn backward = call.backward;
  const uint8_t variant = call.variant;
  const uint8_t input_adjoint_mask = call.input_adjoint_mask;
  const int8_t n_in = call.n_in;
  std::array<int32_t, 6> in_len{};
  for (int k = 0; k < n_in; ++k) in_len[(size_t)k] = call.in_len[k];
  const int32_t out_len = call.out_len;
  const stan::arena_t<std::vector<int>> idata(call.idata.begin(),
                                              call.idata.end());
  const void* const udata = call.udata_owner.get();
  stan::math::reverse_pass_callback([backward, variant, input_adjoint_mask,
                                     n_in, in_len, out_len, idata, udata,
                                     values, adjoints, input_varis,
                                     output_varis, in_offset, out_offset,
                                     scratch_offset]() mutable {
    std::fill(adjoints.begin(), adjoints.end(), 0.0);
    KernelCtx reverse;
    double* const value_base = values.empty() ? nullptr : values.data();
    double* const adjoint_base = adjoints.empty() ? nullptr : adjoints.data();
    reverse.n_in = n_in;
    for (int k = 0; k < n_in; ++k) {
      reverse.in[k] =
          Desc{value_base ? value_base + in_offset[(size_t)k] : nullptr,
               in_len[(size_t)k]};
      reverse.in_adj[k] =
          (input_adjoint_mask & (uint8_t)(1u << k))
              ? Desc{adjoint_base ? adjoint_base + in_offset[(size_t)k]
                                  : nullptr,
                     in_len[(size_t)k]}
              : Desc{nullptr, in_len[(size_t)k]};
    }
    reverse.out = Desc{value_base ? value_base + out_offset : nullptr, out_len};
    reverse.variant = variant;
    reverse.scratch = value_base ? value_base + scratch_offset : nullptr;
    reverse.idata = idata.data();
    reverse.n_idata = (int64_t)idata.size();
    reverse.udata = udata;
    for (int i = 0; i < out_len; ++i)
      adjoints[(size_t)(out_offset + i)] = output_varis[(size_t)i]->adj_;
    reverse.out_adj_vec =
        Desc{adjoint_base ? adjoint_base + out_offset : nullptr, out_len};
    reverse.out_adj = out_len == 1 ? output_varis[0]->adj_ : 0.0;
    backward(reverse);

    size_t vari_at = input_varis.size();
    for (int k = n_in; k-- > 0;) {
      vari_at -= (size_t)in_len[(size_t)k];
      if (!(input_adjoint_mask & (uint8_t)(1u << k))) continue;
      for (int i = in_len[(size_t)k]; i-- > 0;)
        input_varis[vari_at + (size_t)i]->adj_ +=
            adjoints[(size_t)(in_offset[(size_t)k] + i)];
    }
  });
}

void run_call(const Program::Call& call, double* reg, KernelCtx& ctx,
              EvalState* state) {
  if (call.forward == nullptr)
    throw std::logic_error("unbound Program::CALL forward");
  bind_call_fwd_ctx(call, reg, ctx, state);
  call.forward(ctx);
}

void run_call(const Program::Call& call, double* reg, EvalState* state) {
  KernelCtx ctx;
  run_call(call, reg, ctx, state);
}

void register_elementwise_kernels();
void register_density_kernels();
void register_legacy_kernels();
void register_matrix_kernels();
void register_algebra_kernels();
void register_quadrature_kernels();
void register_ode_kernels();
void register_dae_kernels();
void register_ode_adjoint_kernels();
void register_constrain_kernels();
void register_eltwise_kernels();
void register_scalar_binary_kernels();
void register_scalar_unary_ad_kernels();
void register_mixture_kernels();
void register_message_kernels();
void register_rng_kernel();
void register_island_kernel();
void register_structured_loop_kernel();

static void ensure_registered() {
  static const bool once = [] {
    register_elementwise_kernels();
    register_density_kernels();
    register_legacy_kernels();
    register_matrix_kernels();
    register_algebra_kernels();
    register_quadrature_kernels();
    register_ode_kernels();
    register_dae_kernels();
    register_ode_adjoint_kernels();
    register_constrain_kernels();
    register_message_kernels();
    register_rng_kernel();
    register_eltwise_kernels();
    register_scalar_binary_kernels();
    register_scalar_unary_ad_kernels();
    register_mixture_kernels();
    register_island_kernel();
    register_structured_loop_kernel();
    return true;
  }();
  (void)once;
}

void Graph::compact_idata() {
  if (compact_idata_ || idata_pool.empty()) return;

  bool has_views = false;
  for (const Op& op : ops) {
    if (op.n_idata < 0 || (op.n_idata > 0 && op.idata == nullptr)) return;
    has_views |= op.idata != nullptr;
  }
  if (!has_views) {
    // Rewrites can remove every integer-using op but leave its pool behind.
    // No lookup or temporary allocation is needed to release an all-dead pool.
    std::vector<std::vector<int>>{}.swap(idata_pool);
    return;
  }

  // Exact bases are the mutable pool's ownership contract. In particular,
  // do not infer ownership using pointer ranges: const-folding subgraphs
  // borrow parent payloads, and hand-built graphs can borrow external data.
  constexpr size_t unused = std::numeric_limits<size_t>::max();
  struct Entry {
    const int* data;
    size_t pool_index;
    size_t offset = unused;
  };
  std::vector<Entry> entries;
  entries.reserve(idata_pool.size());
  for (size_t i = 0; i < idata_pool.size(); ++i)
    if (!idata_pool[i].empty()) entries.push_back({idata_pool[i].data(), i});
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return std::less<const int*>{}(a.data, b.data);
  });
  const auto find = [&](const int* p) {
    return std::lower_bound(entries.begin(), entries.end(), p,
                            [](const Entry& entry, const int* value) {
                              return std::less<const int*>{}(entry.data, value);
                            });
  };

  size_t total = 0;
  const size_t max_size = std::vector<int>{}.max_size();
  for (const Op& op : ops) {
    if (op.idata == nullptr) continue;
    const auto entry = find(op.idata);
    if (entry == entries.end() || entry->data != op.idata) return;
    const size_t size = idata_pool[entry->pool_index].size();
    if (static_cast<uint64_t>(op.n_idata) > size) return;
    if (entry->offset == unused) {
      if (size > max_size - total)
        throw std::length_error("integer payload storage is too large");
      entry->offset = total;
      total += size;
    }
  }

  std::shared_ptr<std::vector<int>> compact;
  if (total != 0) {
    compact = std::make_shared<std::vector<int>>(total);
    for (const Entry& entry : entries) {
      if (entry.offset == unused) continue;
      const auto& payload = idata_pool[entry.pool_index];
      std::copy(payload.begin(), payload.end(), compact->data() + entry.offset);
    }
  }

  // Every allocation and refusal precedes publication. The remaining raw
  // pointer assignments, shared_ptr move and vector swap cannot throw.
  compact_idata_ = std::move(compact);
  for (Op& op : ops) {
    if (op.idata == nullptr) continue;
    const auto entry = find(op.idata);
    assert(entry != entries.end() && entry->offset != unused);
    op.idata = compact_idata_->data() + entry->offset;
  }
  // clear() would retain the outer vector's per-payload metadata capacity.
  std::vector<std::vector<int>>{}.swap(idata_pool);
}

Executor::Executor(Graph g) : graph_(std::move(g)) {
  ensure_registered();
  graph_.compact_idata();
  bind_();
}

Executor::Executor(const Executor& src)
    : graph_(src.graph_),
      data_(src.data_pointer_exposed_
                ? std::make_shared<std::vector<double>>(*src.data_)
                : src.data_) {
  ensure_registered();
  bind_();
  std::copy(src.values_.begin(), src.values_.end(), values_.begin());
}

void Executor::bind_() {
  const auto checked_size = [](int64_t a, int64_t b) {
    const auto max = static_cast<uint64_t>(std::vector<double>{}.max_size());
    if (a < 0 || b < 0 || static_cast<uint64_t>(a) > max ||
        static_cast<uint64_t>(b) > max - static_cast<uint64_t>(a))
      throw std::length_error("executor arena size overflow");
    return a + b;
  };
  // Only parameters and written slots need private value storage. Classify
  // both outputs, including inactive writes: activity is not immutability.
  std::vector<char> written(graph_.slots.size(), 0);
  for (const auto& op : graph_.ops) {
    written[op.out] = 1;
    if (op.out2 >= 0) written[op.out2] = 1;
  }
  int64_t off = 0, data_size = 0;
  data_offsets_.assign(graph_.slots.size(), -1);
  for (auto& s : graph_.slots) {
    if (s.is_param) {
      s.offset = off;
      off = checked_size(off, s.len);
    }
  }
  n_params_ = off;
  for (size_t i = 0; i < graph_.slots.size(); ++i) {
    auto& s = graph_.slots[i];
    if (s.is_param) continue;
    if (written[i]) {
      s.offset = off;
      off = checked_size(off, s.len);
    } else {
      s.offset = data_size;
      data_offsets_[i] = data_size;
      data_size = checked_size(data_size, s.len);
    }
  }
  values_.assign(off, 0.0);
  if (!data_) data_ = std::make_shared<std::vector<double>>(data_size, 0.0);
  assert(static_cast<int64_t>(data_->size()) == data_size);

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
      adj_off = checked_size(adj_off, s.len);
    }
  }
  assert(adj_off == n_params_);
  for (size_t i = 0; i < graph_.slots.size(); ++i) {
    const Slot& s = graph_.slots[i];
    if (!s.is_param && (written[i] || (int)i == graph_.result_slot)) {
      adjoint_offsets[i] = adj_off;
      adj_off = checked_size(adj_off, s.len);
    }
  }
  adjoints_.assign(adj_off, 0.0);
  result_adjoint_offset_ = -1;
  if (graph_.result_slot >= 0) {
    result_adjoint_offset_ = adjoint_offsets[graph_.result_slot];
    assert(result_adjoint_offset_ >= 0);
  }

  int64_t scratch = 0;
  // Scratch layout is binding state, not graph structure. Only the bound
  // context pointers survive this function; copies compute their own layout.
  std::vector<int64_t> scratch_offsets;
  scratch_offsets.reserve(graph_.ops.size());
  for (const auto& op : graph_.ops) {
    const Kernel& k = kernel(op.opcode);
    if (op.opcode == OP_NONE_ || k.forward == nullptr)
      // Name it. A browser build can be missing a kernel because its
      // density pack has not been loaded yet, and the caller decides what
      // to do from this string.
      throw std::runtime_error(std::string("opcode not registered: ") +
                               opcode_name(op.opcode));
    scratch_offsets.push_back(scratch);
    scratch = checked_size(
        scratch, k.scratch_size ? k.scratch_size(op, graph_.slots.data()) : 0);
  }
  scratch_.assign(scratch, 0.0);

  // Assemble every kernel context once, now that all three arenas are sized
  // and every offset is final. Reassembling one per op per sweep cost a
  // scattered slot lookup per input and ~300 bytes of stores, twice per
  // gradient, which on the serial models (one op per observation, nothing to
  // vectorize) was a third of the time.
  ctx_.resize(graph_.ops.size());
  kernel_states_.clear();
  out2_adj_ptr_.assign(graph_.ops.size(), nullptr);
  for (size_t i = 0; i < graph_.ops.size(); ++i) {
    ctx_[i] =
        make_ctx_(graph_.ops[i], scratch_offsets[i], written, adjoint_offsets);
    const Kernel& k = kernel(graph_.ops[i].opcode);
    if (k.make_state) {
      kernel_states_.emplace_back(
          k.make_state(graph_.ops[i], graph_.slots.data()));
      ctx_[i].state = kernel_states_.back().get();
    }
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
  for (size_t i = 0; i < graph_.ops.size(); ++i) {
    fwd_fn_[i] = resolve_forward_fn(graph_.ops[i]);
  }
  for (size_t i = graph_.ops.size(); i-- > 0;) {
    void (*b)(KernelCtx&) = kernel(graph_.ops[i].opcode).backward;
    if (b) bwd_.push_back(BwdStep{b, &ctx_[i], out2_adj_ptr_[i]});
  }
}

KernelCtx Executor::make_ctx_(const Op& op, int64_t scratch_offset,
                              const std::vector<char>& written,
                              const std::vector<int64_t>& adjoint_offsets) {
  KernelCtx ctx;
  ctx.n_in = op.n_in;
  for (int i = 0; i < op.n_in; ++i) {
    const Slot& s = graph_.slots[op.in[i]];
    ctx.in[i] = Desc{slot_data_(op.in[i]), s.len};
  }
  const Slot& so = graph_.slots[op.out];
  ctx.out = Desc{slot_data_(op.out), so.len};
  if (op.out2 >= 0) {
    const Slot& s2 = graph_.slots[op.out2];
    ctx.out2 = Desc{slot_data_(op.out2), s2.len};
  }
  ctx.variant = op.variant;
  ctx.scratch = scratch_.empty() ? nullptr : scratch_.data() + scratch_offset;
  ctx.idata = op.idata;
  ctx.udata = op.udata;
  ctx.eval_state = &eval_state_;
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

void Executor::detach_data_() {
  if (data_.unique()) return;
  auto replacement = std::make_shared<std::vector<double>>(*data_);
  data_ = std::move(replacement);
  // Bound contexts hold input pointers. No output can belong to data_.
  for (size_t k = 0; k < graph_.ops.size(); ++k)
    for (int i = 0; i < graph_.ops[k].n_in; ++i) {
      const int slot = graph_.ops[k].in[i];
      if (data_offsets_[slot] >= 0) ctx_[k].in[i].data = slot_data_(slot);
    }
}

double* Executor::value_ptr(int slot) {
  if (data_offsets_[slot] >= 0) {
    detach_data_();
    data_pointer_exposed_ = true;
  }
  return slot_data_(slot);
}

void Executor::set_values(int slot, const double* data, size_t size) {
  if (slot < 0 || static_cast<size_t>(slot) >= graph_.slots.size() ||
      size > static_cast<uint64_t>(graph_.slots[slot].len))
    throw std::out_of_range("executor fill exceeds slot");
  if (data_offsets_[slot] >= 0) detach_data_();
  if (size) std::copy_n(data, size, slot_data_(slot));
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

void Executor::run_forward_only() { run_forward_only(EvalState{}); }

void Executor::run_forward_only(EvalState state) {
  struct RestoreState {
    EvalState& slot;
    EvalState previous;
    ~RestoreState() { slot = previous; }
  } restore{eval_state_, eval_state_};
  eval_state_ = state;
  // The profiled path keeps the opcode-keyed loop (attribution needs the
  // opcode anyway, and the timing calls dwarf dispatch cost), but it must
  // invoke the same bound function pointer as the fast path. The registry is
  // mutable for extension kernels; consulting it here would make profiling
  // change both variant specialization and post-bind kernel overrides.
  if (profile_) {
    const size_t np = graph_.ops.size();
    for (size_t i = 0; i < np; ++i) {
      const uint16_t op = graph_.ops[i].opcode;
      const auto t0 = std::chrono::steady_clock::now();
      fwd_fn_[i](ctx_[i]);
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
  return slot_data_(graph_.result_slot)[0];
}

double Executor::gradient(double* grad_out) {
  ++n_grad_evals_;
  const double v = forward();
  std::memset(adjoints_.data(), 0, sizeof(double) * adjoints_.size());
  assert(result_adjoint_offset_ >= 0);
  adjoints_[result_adjoint_offset_] = 1.0;
  if (profile_) {
    // Profile the exact bound backward plan. Each context belongs to ctx_,
    // so its index supplies opcode attribution without a second graph walk
    // or extra fields in the fast-path BwdStep layout.
    for (const BwdStep& step : bwd_) {
      KernelCtx& ctx = *step.ctx;
      const size_t pi = static_cast<size_t>(step.ctx - ctx_.data());
      assert(pi < graph_.ops.size());
      if (ctx.out_adj_vec.len == 1) ctx.out_adj = ctx.out_adj_vec.data[0];
      if (step.out2_adj) ctx.out2_adj = *step.out2_adj;
      const auto t0 = std::chrono::steady_clock::now();
      step.fn(ctx);
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
