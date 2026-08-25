// A tape island: one op standing in for a region of scalar code the graph
// is better off not holding as ops.
//
// Two things produce one. The carver (island.cpp) takes regions no pass
// could vectorize -- cross-lane recurrences: HMM forward algorithms,
// state-space updates -- and compiles them when that is measurably
// cheaper than the ops. Lowering produces the other kind: a region whose
// control flow depends on a parameter cannot become graph ops at all, so
// it is compiled out of MIR directly and the island is how it exists.
//
// The program itself is the shared register machine (program.hpp), so
// both kinds run the same way. Forward runs on plain doubles -- one
// dispatch where the region had thousands. Backward replays the program
// under stan-math nested autodiff and harvests the live-ins' adjoints:
// the same var arithmetic CmdStan's generated code runs for the same
// statements, so gradients match by construction. A branch on a parameter
// differentiates the taken branch, which is exactly what the generated
// C++ does, because it is the same autodiff seeing the same arithmetic.
//
// Registers are mutable cells, one per element of every slot the region
// touches; a len-k slot is k consecutive registers, so Eigen::Map works
// on ranges. Values come only from the forward double pass; the var pass
// exists for its adjoints.
//
// Densities appear only in propto-OFF form (the carver refuses propto):
// with no term-dropping, the instantiation is type-uniform and one
// templated call serves both passes. Propto term-dropping depends on
// argument TYPES (see legacy_fns.cpp's dirichlet note), which would need
// per-mask binding -- out of scope until islands absorb target terms.
#ifndef STANLI_ISLAND_HPP
#define STANLI_ISLAND_HPP

#include <stanli/adjoint.hpp>
#include <stanli/program.hpp>

#include <cstdint>
#include <vector>

namespace stanli {

struct IslandProg : Program {
  // Live-in k seeds registers [ins[k].reg, ins[k].reg + ins[k].len) from
  // the op's ctx.in[k]; the kernel snapshots the same values into scratch
  // so the backward replay is immune to later in-place overwrites.
  struct LiveIn {
    int reg = 0;
    int len = 0;
    // Normally live-in k reads ctx.in[k] at offset zero. Necessity regions
    // with more graph values than Op::in can hold pack a leading group with
    // OP_CONCAT2 and point several register ranges into that one descriptor.
    int input = -1;
    int offset = 0;
  };
  std::vector<LiveIn> ins;
  // The generated backward (adjoint.hpp), empty for a program the generator
  // does not differentiate.
  AdjProgram adj;
  // Whether to run it. STANLI_NO_NATIVE_ADJ clears this and nothing else:
  // the adjoint is still generated and the carve estimate still assumes it,
  // so the two backwards are compared over the SAME islands running the
  // SAME forward program, which is the only comparison worth having.
  bool native_adj = false;
};

// Generate p.adj, appending checkpoint saves to p's forward code. False
// leaves p untouched and keeps the replay.
bool gen_adjoint(IslandProg& p);

// Evaluate on T = double (forward) or stan::math::var (backward replay,
// inside the caller's nested_rev_autodiff). The register file is reused
// between calls. Not reentrant; islands cannot contain islands.
template <typename T>
void run_island(const IslandProg& p, const T* const* in, T* out) {
  static thread_local std::vector<T> reg;
  if ((int64_t)reg.size() < p.n_regs) reg.resize((size_t)p.n_regs);
  for (size_t k = 0; k < p.ins.size(); ++k) {
    const int input = p.ins[k].input >= 0 ? p.ins[k].input : (int)k;
    for (int i = 0; i < p.ins[k].len; ++i)
      reg[(size_t)(p.ins[k].reg + i)] = in[input][p.ins[k].offset + i];
  }

  run_program(p, reg);

  for (size_t i = 0; i < p.out_regs.size(); ++i)
    out[i] = reg[(size_t)p.out_regs[i]];
}

struct Graph;  // graph.hpp

// The carver: replace maximal compilable runs of scalar residue with
// OP_ISLAND ops (payload in g.udata_pool) plus one INDEX/SLICE per
// live-out writing the original slot ids. Runs after every other pass.
// fills provides the constant pool for CONSTR absorption; target_terms
// and extra_roots are the slots the pass must not absorb.
// Returns the number of islands carved. STANLI_NO_ISLAND=1 disables this
// pass only: the islands lowering emits for parameter-dependent control
// flow are not an optimization and are always on.
int carve_islands(Graph& g,
                  const std::vector<std::pair<int, std::vector<double>>>& fills,
                  const std::vector<int>& target_terms,
                  const std::vector<int>& extra_roots);

}  // namespace stanli

#endif
