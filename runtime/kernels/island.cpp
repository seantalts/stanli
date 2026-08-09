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
#include <vector>

namespace stanli {
namespace {

int64_t island_scratch(const Op& op, const Slot* slots) {
  const auto& p = *static_cast<const IslandProg*>(op.udata);
  // The generated backward reads the whole register file, so the forward
  // runs in scratch and leaves it there. n_regs covers the live-ins, which
  // occupy registers of their own. The replay only needs their snapshot.
  if (p.native_adj) return p.n_regs;
  return sum_in_lens(op, slots);
}

void island_fwd(KernelCtx& ctx) {
  const auto& p = *static_cast<const IslandProg*>(ctx.udata);
  if (p.native_adj) {
    for (int k = 0; k < ctx.n_in; ++k)
      for (int64_t i = 0; i < ctx.in[k].len; ++i)
        ctx.scratch[p.ins[(size_t)k].reg + i] = ctx.in[k].data[i];
    run_program(p, ctx.scratch);
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

// The generated backward: seed the live-outs, sweep, harvest the live-ins.
void island_bwd_native(const IslandProg& p, KernelCtx& ctx) {
  static thread_local std::vector<double> adj;
  if ((int64_t)adj.size() < p.n_regs) adj.resize((size_t)p.n_regs);
  std::fill(adj.begin(), adj.begin() + p.n_regs, 0.0);
  // Through the sharing map, since a live-out register need not own its
  // adjoint cell. Descending, because two live-out slots can share a
  // register range (the carver aliases a dead copy-then-modify chain onto
  // its base) and the replay's seeding sum unwinds in that order.
  const auto& map = p.adj.adj_reg;
  for (size_t m = p.out_regs.size(); m-- > 0;)
    adj[(size_t)map[(size_t)p.out_regs[m]]] += ctx.out_adj_vec.data[m];
  run_adjoint(p, p.adj, ctx.scratch, adj.data());
  for (int k = 0; k < ctx.n_in; ++k) {
    if (!ctx.in_adj[k].data) continue;
    const auto& li = p.ins[(size_t)k];
    for (int i = 0; i < li.len; ++i)
      ctx.in_adj[k].data[i] += adj[(size_t)map[(size_t)(li.reg + i)]];
  }
}

void island_bwd(KernelCtx& ctx) {
  const auto& p = *static_cast<const IslandProg*>(ctx.udata);
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

void register_island_kernel() {
  register_kernel(OP_ISLAND, Kernel{island_fwd, island_bwd, island_scratch});
}

}  // namespace stanli
