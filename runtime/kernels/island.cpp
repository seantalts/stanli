// OP_ISLAND: one op for a compiled region of scalar residue (island.hpp).
//
// Forward runs the program on doubles. Backward has two forms and the
// program says which:
//
//   * **generated** (adjoint.hpp), when the region compiled one: a second
//     double pass over the adjoint register file. No vari, no nested tape,
//     no allocation. The forward's register file is kept in the op's own
//     scratch so the backward can read the values it needs -- which is also
//     why the live-in snapshot the replay needs is not a separate copy here:
//     the register file IS the snapshot.
//   * **replayed**, otherwise: the program re-executed under stan-math
//     nested autodiff with the live-ins bound as var, seeded via the dot
//     trick (sum of out vars times their adjoints). It stays as the oracle
//     the generated form is verified against, and STANLI_NO_NATIVE_ADJ=1
//     selects it for every island.
//
// Either way the backward reads values snapshotted at forward time, never
// the arena: the in-place pass ran before islands existed and may have
// licensed a destructive overwrite of a live-in buffer on the strength of
// the replaced ops' scratch-only backwards.
#include <stanli/adjoint.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <vector>

namespace stanli {
namespace {

int64_t island_scratch(const Op& op, const Slot* slots) {
  const auto& p = *static_cast<const IslandProg*>(op.udata);
  // The generated backward reads the whole register file, so the forward
  // runs in scratch and leaves it there. n_regs covers the live-ins, which
  // occupy registers of their own. The replay only needs their snapshot.
  if (p.native_adj)
    return p.n_regs + (p.adj.trace_bits > 0 ? (p.adj.trace_bits + 63) / 64 : 0);
  return sum_in_lens(op, slots);
}

template <bool ReuseCallCtx>
void island_fwd_impl(KernelCtx& ctx) {
  const auto& p = *static_cast<const IslandProg*>(ctx.udata);
  if (p.native_adj) {
    // Mirrored by island_softmax3_fwd; keep these seed and harvest loops in
    // lockstep with runtime/src/program_softmax.cpp.
    for (size_t k = 0; k < p.ins.size(); ++k) {
      const auto& li = p.ins[k];
      const int input = li.input >= 0 ? li.input : (int)k;
      for (int i = 0; i < li.len; ++i)
        ctx.scratch[li.reg + i] = ctx.in[input].data[li.offset + i];
    }
    if (p.adj.trace_bits > 0) {
      const size_t trace_bytes =
          static_cast<size_t>((p.adj.trace_bits + 63) / 64) * sizeof(uint64_t);
      uint8_t* const executed =
          reinterpret_cast<uint8_t*>(ctx.scratch + p.n_regs);
      std::memset(executed, 0, trace_bytes);
      run_program_impl<ReuseCallCtx>(p, ctx.scratch, &p.code, executed,
                                     p.trace_pc.data(), ctx.program_call_hook);
    } else {
      run_program_impl<ReuseCallCtx>(p, ctx.scratch, nullptr, nullptr, nullptr,
                                     ctx.program_call_hook);
    }
    for (size_t m = 0; m < p.out_regs.size(); ++m)
      ctx.out.data[m] = ctx.scratch[p.out_regs[m]];
    return;
  }
  const double* in[6];
  int64_t off = 0;
  for (int k = 0; k < ctx.n_in; ++k) {
    for (int64_t i = 0; i < ctx.in[k].len; ++i)
      ctx.scratch[off + i] = ctx.in[k].data[i];
    in[k] = ctx.scratch + off;
    off += ctx.in[k].len;
  }
  run_island<double>(p, in, ctx.out.data);
}

void island_fwd(KernelCtx& ctx) { island_fwd_impl<false>(ctx); }

// Re-execute a pure selector while carrying the flattened live-in element
// each register currently denotes. Comparisons and constants have no origin;
// copies propagate one. Descending live-out seeding matches the var replay's
// reverse sweep over `j += out[m] * seed[m]`.
void island_bwd_selector(const IslandProg& p, KernelCtx& ctx) {
  static thread_local std::vector<double> value;
  static thread_local std::vector<int64_t> origin;
  if (static_cast<int64_t>(value.size()) < p.n_regs)
    value.resize(static_cast<size_t>(p.n_regs));
  if (static_cast<int64_t>(origin.size()) < p.n_regs)
    origin.resize(static_cast<size_t>(p.n_regs));
  std::fill(origin.begin(), origin.begin() + p.n_regs, int64_t{-1});

  int64_t input_base[6] = {0, 0, 0, 0, 0, 0};
  const double* input_value[6] = {nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr};
  int64_t total = 0;
  for (int k = 0; k < ctx.n_in; ++k) {
    input_base[k] = total;
    input_value[k] = ctx.scratch + total;
    total += ctx.in[k].len;
  }
  for (size_t k = 0; k < p.ins.size(); ++k) {
    const IslandProg::LiveIn& input = p.ins[k];
    const int op_input = input.input >= 0 ? input.input : static_cast<int>(k);
    for (int i = 0; i < input.len; ++i) {
      value[static_cast<size_t>(input.reg + i)] =
          input_value[op_input][input.offset + i];
      origin[static_cast<size_t>(input.reg + i)] =
          input_base[op_input] + input.offset + i;
    }
  }

  const int64_t n = static_cast<int64_t>(p.code.size());
  for (int64_t pc = 0; pc < n; ++pc) {
    const Program::Instr& instruction = p.code[static_cast<size_t>(pc)];
    switch (instruction.code) {
      case Program::CONST:
        value[static_cast<size_t>(instruction.dst)] =
            p.pool[static_cast<size_t>(instruction.a)];
        origin[static_cast<size_t>(instruction.dst)] = -1;
        break;
      case Program::CONSTR:
        for (int i = 0; i < instruction.len; ++i) {
          value[static_cast<size_t>(instruction.dst + i)] =
              p.pool[static_cast<size_t>(instruction.a + i)];
          origin[static_cast<size_t>(instruction.dst + i)] = -1;
        }
        break;
      case Program::MOV:
        value[static_cast<size_t>(instruction.dst)] =
            value[static_cast<size_t>(instruction.a)];
        origin[static_cast<size_t>(instruction.dst)] =
            origin[static_cast<size_t>(instruction.a)];
        break;
      case Program::MOVR:
        for (int i = 0; i < instruction.len; ++i) {
          value[static_cast<size_t>(instruction.dst + i)] =
              value[static_cast<size_t>(instruction.a + i)];
          origin[static_cast<size_t>(instruction.dst + i)] =
              origin[static_cast<size_t>(instruction.a + i)];
        }
        break;
#define STANLI_SELECTOR_COMPARE(code, op)                                  \
  case Program::code:                                                     \
    value[static_cast<size_t>(instruction.dst)] =                         \
        value[static_cast<size_t>(instruction.a)]                         \
            op value[static_cast<size_t>(instruction.b)];                 \
    origin[static_cast<size_t>(instruction.dst)] = -1;                    \
    break
        STANLI_SELECTOR_COMPARE(GT, >);
        STANLI_SELECTOR_COMPARE(GE, >=);
        STANLI_SELECTOR_COMPARE(LT, <);
        STANLI_SELECTOR_COMPARE(LE, <=);
        STANLI_SELECTOR_COMPARE(EQ, ==);
        STANLI_SELECTOR_COMPARE(NE, !=);
#undef STANLI_SELECTOR_COMPARE
      case Program::JZ:
        if (value[static_cast<size_t>(instruction.a)] == 0.0)
          pc = instruction.dst - 1;
        break;
      case Program::JMP:
        pc = instruction.dst - 1;
        break;
      default:
        // supports_selector_adjoint validated this immutable payload.
        return;
    }
  }

  for (size_t m = p.out_regs.size(); m-- > 0;) {
    const int64_t source = origin[static_cast<size_t>(p.out_regs[m])];
    if (source < 0) continue;
    for (int input = ctx.n_in; input-- > 0;) {
      if (source < input_base[input]) continue;
      const int64_t offset = source - input_base[input];
      if (offset < ctx.in[input].len && ctx.in_adj[input].data)
        ctx.in_adj[input].data[offset] += ctx.out_adj_vec.data[m];
      break;
    }
  }
}

struct NativeAdjointWorkspace {
  std::vector<double> adj;
  // A successful managed reset leaves the entire allocation clean. An
  // exception can interrupt the consume-and-clear sweep, in which case the
  // next managed entry recovers the whole shared TLS allocation.
  bool dirty = false;
};

struct NativeAdjointWorkspacePool {
  std::deque<NativeAdjointWorkspace> workspaces;
  size_t depth = 0;

  NativeAdjointWorkspace& acquire() {
    if (depth == workspaces.size()) workspaces.emplace_back();
    return workspaces[depth++];
  }

  void release() { --depth; }
};

class NativeAdjointUse {
 public:
  explicit NativeAdjointUse(NativeAdjointWorkspacePool& pool)
      : pool_(pool), workspace_(pool_.acquire()) {}
  ~NativeAdjointUse() { pool_.release(); }
  NativeAdjointWorkspace& workspace() { return workspace_; }

 private:
  NativeAdjointWorkspacePool& pool_;
  NativeAdjointWorkspace& workspace_;
};

bool env_enabled(const char* name) {
  const char* const value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool managed_sparse_adj_reset() {
  return env_enabled("STANLI_ISLAND_SPARSE_ADJ_RESET") &&
         !env_enabled("STANLI_NO_ISLAND_SPARSE_ADJ_RESET");
}

// The generated backward: seed the live-outs, sweep, harvest the live-ins.
void island_bwd_native(const IslandProg& p, KernelCtx& ctx) {
  static thread_local NativeAdjointWorkspacePool pool;
  NativeAdjointUse use(pool);
  NativeAdjointWorkspace& workspace = use.workspace();
  const bool managed_reset = managed_sparse_adj_reset();
  if (managed_reset && workspace.dirty) {
    std::fill(workspace.adj.begin(), workspace.adj.end(), 0.0);
    workspace.dirty = false;
  }
  if ((int64_t)workspace.adj.size() < p.adj.n_regs)
    workspace.adj.resize((size_t)p.adj.n_regs);
  if (!managed_reset)
    std::fill(workspace.adj.begin(),
              workspace.adj.begin() + p.adj.n_regs, 0.0);
  workspace.dirty = true;
  std::vector<double>& adj = workspace.adj;
  // Through the sharing map, since a live-out register need not own its
  // adjoint cell. Descending, because two live-out slots can share a
  // register range (the carver aliases a dead copy-then-modify chain onto
  // its base) and the replay's seeding sum unwinds in that order.
  const auto& map = p.adj.adj_reg;
  for (size_t m = p.out_regs.size(); m-- > 0;)
    adj[(size_t)map[(size_t)p.out_regs[m]]] += ctx.out_adj_vec.data[m];
  const uint8_t* const executed =
      p.adj.trace_bits > 0
          ? reinterpret_cast<const uint8_t*>(ctx.scratch + p.n_regs)
          : nullptr;
  run_adjoint(p, p.adj, ctx.scratch, adj.data(), executed);
  for (size_t k = 0; k < p.ins.size(); ++k) {
    const auto& li = p.ins[k];
    const int input = li.input >= 0 ? li.input : (int)k;
    if (!ctx.in_adj[input].data) continue;
    for (int i = 0; i < li.len; ++i)
      ctx.in_adj[input].data[li.offset + i] +=
          adj[(size_t)map[(size_t)(li.reg + i)]];
  }

  if (managed_reset) {
    if (p.sparse_adj_clear_eligible) {
      for (int32_t cell : p.sparse_adj_clear_cells)
        adj[static_cast<size_t>(cell)] = 0.0;
    } else {
      // Small or dense reset plans keep the contiguous full-clear fallback.
      std::fill(adj.begin(), adj.begin() + p.adj.n_regs, 0.0);
    }
    workspace.dirty = false;
  }
}

void island_bwd(KernelCtx& ctx) {
  const auto& p = *static_cast<const IslandProg*>(ctx.udata);
  if (p.selector_adj) {
    island_bwd_selector(p, ctx);
    return;
  }
  if (p.native_adj) {
    island_bwd_native(p, ctx);
    return;
  }
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  int64_t total = 0;
  for (int k = 0; k < ctx.n_in; ++k) total += ctx.in[k].len;
  std::vector<var> vin((size_t)total);
  const var* in[6];
  int64_t off = 0;
  for (int k = 0; k < ctx.n_in; ++k) {
    for (int64_t i = 0; i < ctx.in[k].len; ++i)
      vin[(size_t)(off + i)] = ctx.scratch[off + i];
    in[k] = vin.data() + off;
    off += ctx.in[k].len;
  }
  std::vector<var> vout(p.out_regs.size());
  run_island<var>(p, in, vout.data());
  var j = 0.0;
  for (size_t m = 0; m < vout.size(); ++m)
    j += vout[m] * ctx.out_adj_vec.data[m];
  stan::math::grad(j.vi_);
  off = 0;
  for (int k = 0; k < ctx.n_in; ++k) {
    if (ctx.in_adj[k].data)
      for (int64_t i = 0; i < ctx.in[k].len; ++i)
        ctx.in_adj[k].data[i] += vin[(size_t)(off + i)].adj();
    off += ctx.in[k].len;
  }
}

}  // namespace

void island_calls_fwd(KernelCtx& ctx) { island_fwd_impl<true>(ctx); }

void register_island_kernel() {
  register_kernel(OP_ISLAND, Kernel{island_fwd, island_bwd, island_scratch});
}

}  // namespace stanli
