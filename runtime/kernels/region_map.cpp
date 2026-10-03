#include <stanli/adjoint.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include <stanli/region_map.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace stanli {
namespace {

int64_t region_map_scratch(const Op& op, const Slot*) {
  const auto& p = *static_cast<const RegionMapProg*>(op.udata);
  return (int64_t)p.n_regs + p.adj.n_regs + p.count * p.saved_cells +
         region_map_lane_cells(p);
}

void run_prologue(const RegionMapProg& p, double* reg) {
  for (const auto& I : p.prologue) {
    switch (I.code) {
      case Program::CONST:
        reg[I.dst] = p.pool[(size_t)I.a];
        break;
      case Program::CONSTR:
        std::copy_n(p.pool.data() + I.a, I.len, reg + I.dst);
        break;
      default:
        std::fill_n(reg + I.dst, I.len, p.pool[(size_t)I.a]);
        break;
    }
  }
}

template <bool ReuseCallCtx>
void run_once(const RegionMapProg& p, KernelCtx& ctx) {
  try {
    run_program_impl<ReuseCallCtx>(p, ctx.scratch, ctx.eval_state);
  } catch (const std::out_of_range& e) {
    if (std::string_view(e.what()) != "register-program index out of range")
      throw;
    throw std::out_of_range("structured index out of range");
  }
}

template <bool ReuseCallCtx>
void run_iterations(const RegionMapProg& p, KernelCtx& ctx) {
  double total = 0.0;
  double* row = ctx.scratch + p.n_regs + p.adj.n_regs;
  for (int64_t i = 0; i < p.count; ++i) {
    ctx.scratch[p.iter_reg] = static_cast<double>(p.lo + i);
    run_once<ReuseCallCtx>(p, ctx);
    total += ctx.scratch[p.out_regs[0]];
    for (const auto& span : p.saved) {
      std::copy_n(ctx.scratch + span.first, span.second, row);
      row += span.second;
    }
  }
  ctx.out.data[0] = total;
}

void seed_registers(const RegionMapProg& p, KernelCtx& ctx) {
  for (size_t k = 0; k < p.ins.size(); ++k) {
    const auto& li = p.ins[k];
    if (li.input < 0) continue;
    std::copy_n(ctx.in[li.input].data + li.offset, li.len,
                ctx.scratch + li.reg);
  }
  run_prologue(p, ctx.scratch);
}

void run_scalar_forward(const RegionMapProg& p, KernelCtx& ctx) {
  if (p.calls.empty())
    run_iterations<false>(p, ctx);
  else
    run_iterations<true>(p, ctx);
}

double* lane_region(const RegionMapProg& p, KernelCtx& ctx) {
  return ctx.scratch + p.n_regs + p.adj.n_regs + p.count * p.saved_cells;
}

void run_lane_forward(const RegionMapProg& p, KernelCtx& ctx) {
  std::exception_ptr original;
  try {
    region_map_lanes_forward(p, ctx, lane_region(p, ctx));
    region_map_lanes_scatter_saved(p, ctx, lane_region(p, ctx),
                                   ctx.scratch + p.n_regs + p.adj.n_regs);
    return;
  } catch (...) {
    original = std::current_exception();
  }
  seed_registers(p, ctx);
  run_scalar_forward(p, ctx);
  std::rethrow_exception(original);
}

void region_map_fwd(KernelCtx& ctx) {
  const auto& p = *static_cast<const RegionMapProg*>(ctx.udata);
  seed_registers(p, ctx);
  if (p.lanes.active)
    run_lane_forward(p, ctx);
  else
    run_scalar_forward(p, ctx);
}

template <bool ReuseCallCtx>
void sweep(const RegionMapProg& p, KernelCtx& ctx, double* adj) {
  const double seed = ctx.out_adj_vec.data[0];
  const int64_t target = p.adj.adj_reg[(size_t)p.out_regs[0]];
  const double* saved = ctx.scratch + p.n_regs + p.adj.n_regs;
  for (int64_t i = p.count; i-- > 0;) {
    ctx.scratch[p.iter_reg] = static_cast<double>(p.lo + i);
    if (p.recompute) {
      run_once<ReuseCallCtx>(p, ctx);
    } else {
      const double* row = saved + i * p.saved_cells;
      for (const auto& span : p.saved) {
        std::copy_n(row, span.second, ctx.scratch + span.first);
        row += span.second;
      }
    }
    for (const auto& span : p.transient)
      std::fill_n(adj + span.first, span.second, 0.0);
    adj[target] += seed;
    run_adjoint(p, p.adj, ctx.scratch, adj);
  }
}

void region_map_bwd(KernelCtx& ctx) {
  const auto& p = *static_cast<const RegionMapProg*>(ctx.udata);
  const bool continuing = continue_input_adjoints(p, ctx);
  double* adj = ctx.scratch + p.n_regs;
  std::fill_n(adj, p.adj.n_regs, 0.0);
  const auto& map = p.adj.adj_reg;
  for (const auto& li : p.ins) {
    if (li.input < 0 || !continuing || !li.immutable ||
        !ctx.in_adj[li.input].data)
      continue;
    for (int i = 0; i < li.len; ++i)
      adj[(size_t)map[(size_t)(li.reg + i)]] =
          ctx.in_adj[li.input].data[li.offset + i];
  }
  if (p.calls.empty())
    sweep<false>(p, ctx, adj);
  else
    sweep<true>(p, ctx, adj);
  for (const auto& li : p.ins) {
    if (li.input < 0 || !ctx.in_adj[li.input].data) continue;
    for (int i = 0; i < li.len; ++i) {
      const double contribution = adj[(size_t)map[(size_t)(li.reg + i)]];
      if (continuing && li.immutable)
        ctx.in_adj[li.input].data[li.offset + i] = contribution;
      else
        ctx.in_adj[li.input].data[li.offset + i] += contribution;
    }
  }
}

}  // namespace

void register_region_map_kernel() {
  register_kernel(OP_REGION_MAP, Kernel{region_map_fwd, region_map_bwd,
                                        region_map_scratch, nullptr});
}

}  // namespace stanli
