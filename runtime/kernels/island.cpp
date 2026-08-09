// OP_ISLAND: one op for a compiled region of scalar residue (island.hpp).
//
// Forward runs the program on doubles. Backward replays it under stan-math
// nested autodiff with the live-ins bound as var, seeds via the dot trick
// (sum of out vars times their adjoints), and harvests the live-ins'
// adjoints. The replay reads the live-in SNAPSHOT taken at forward time,
// never the arena: the in-place pass ran before islands existed and may
// have licensed a destructive overwrite of a live-in buffer on the strength
// of the replaced ops' scratch-only backwards.
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <vector>

namespace stanli {
namespace {

void island_fwd(KernelCtx& ctx) {
  const auto& p = *static_cast<const IslandProg*>(ctx.udata);
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

void island_bwd(KernelCtx& ctx) {
  const auto& p = *static_cast<const IslandProg*>(ctx.udata);
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
  register_kernel(OP_ISLAND, Kernel{island_fwd, island_bwd, sum_in_lens});
}

}  // namespace stanli
