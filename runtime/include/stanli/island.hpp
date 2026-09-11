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
// with no term-dropping, the forward is type-uniform and one templated
// call serves both passes. The backward does bind per mask, to skip the
// partials of data arguments, but propto term-dropping needs the same
// masks on the VALUE (see legacy_fns.cpp's dirichlet note) -- out of
// scope until islands absorb target terms.
#ifndef STANLI_ISLAND_HPP
#define STANLI_ISLAND_HPP

#include <stanli/adjoint.hpp>
#include <stanli/program.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
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
    // Whether the slot it seeds is downstream of a parameter. The carver
    // knows; the adjoint generator propagates it to reach the densities.
    // True where nobody says otherwise, which is the all-active binding
    // this was before.
    bool active = true;
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

// Payload used only by OP_ISLAND with kIslandSoftmax3Variant. Ordinary islands
// retain the exact IslandProg size and allocation they had before this
// optimization existed.
// The inherited canonical Program stays untouched and remains the replay
// oracle wherever var replay is supported.
struct Softmax3IslandProg : IslandProg {
  std::shared_ptr<const Program> optimized_double;
};

// This value selects the derived payload's double-only plan while retaining
// the ordinary island opcode and kernel-table layout. Setting it on a plain
// IslandProg violates the tagged payload contract; the graph carver is the
// only production producer. Its
// forward must leave outputs and scratch bitwise-identical to OP_ISLAND's
// canonical forward: the profiled executor and direct kernel-table callers
// use that path, and the generated adjoint consumes either register file.
// test_softmax3_double_exact enforces this contract.
constexpr uint8_t kIslandSoftmax3Variant = 1;
// Generic variant for a canonical IslandProg whose forward bytecode contains
// Program::CALL. Executor binding selects its context-reusing forward once;
// ordinary islands retain the context-free evaluator with no runtime check.
constexpr uint8_t kIslandCallVariant = 2;

void island_calls_fwd(KernelCtx& ctx);

// True when the region's program has an observable effect: a draw from the
// caller's stream, a print, or a reject. A pass that reasons about purity has
// to leave such a region alone even when every one of its inputs is data.
// Nothing depends on this for correctness -- an effect kernel refuses to run
// without the evaluation state a compile-time pass has no business owning,
// so folding one fails rather than erasing it -- but that refusal costs the
// whole constant sub-graph, and naming the effect costs only the region.
bool island_has_effect(const Program& p);

// Run compact_program (program.hpp) over the region's forward code, live-ins
// included, before the adjoint generator reads it -- so the backward is
// generated from the compacted program rather than remapped onto it.
// STANLI_NO_ISLAND_COMPACT=1 disables this pass only.
void compact_island(IslandProg& p);

// Explicitly gate producer-destination forwarding and report whether it
// changed the program. The one-argument entry point remains the public default.
bool compact_island_gated(IslandProg& p, bool enable_destination_forwarding);

// Generate p.adj, appending checkpoint saves to p's forward code. False
// leaves p untouched and keeps the replay.
bool gen_adjoint(IslandProg& p);

// After gen_adjoint has captured the original forward program, return a
// double-only clone that replaces sufficiently common SOFTMAX(3) instructions
// with calls to an allocation-free private Program kernel. Null means refused;
// the canonical bytecode stays unchanged.
std::shared_ptr<const Program> specialize_softmax3(const IslandProg& p,
                                                   size_t min_count = 32);

// Evaluate on T = double (forward) or stan::math::var (backward replay,
// inside the caller's nested_rev_autodiff). The register file is reused
// by the caller, which owns its lifetime.
template <typename T>
void run_island(const IslandProg& p, const T* const* in, T* out, T* reg,
                EvalState* state = nullptr) {
  for (size_t k = 0; k < p.ins.size(); ++k) {
    const int input = p.ins[k].input >= 0 ? p.ins[k].input : (int)k;
    for (int i = 0; i < p.ins[k].len; ++i)
      reg[(size_t)(p.ins[k].reg + i)] = in[input][p.ins[k].offset + i];
  }

  run_program(p, reg, state);

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

// A straight-line run of a retained loop body compiled to one program. The
// loop executor seeds the live-in registers from its own slot bindings, runs
// the forward, and publishes the live-out registers as new slot versions;
// the generated adjoint runs over the same frame in the reverse pass.
struct SegmentItem {
  int op = -1;
  int alias_dst = -1, alias_src = -1;
};
struct SegmentBinding {
  int slot = -1;
  int reg = 0;
  int len = 0;
};
struct Segment {
  IslandProg program;
  std::vector<SegmentBinding> ins, outs;
};

// Whether a body op can be an item of a segment: the carver's vocabulary,
// including CALLs to kernels the executor would otherwise dispatch itself.
bool segment_supports(const Graph& g, const Op& op);

// Compile `items` in order. Slots in `constants` are absorbed into the pool;
// other slots read before they are written become live-ins. `live_outs` are
// the slots read after the run; slots that are both live-in and written are
// published as well, since the next visit reads them through their cell.
// False when an op is outside the vocabulary or the adjoint is refused.
bool compile_segment(
    const Graph& g, const std::vector<SegmentItem>& items,
    const std::unordered_map<int, const std::vector<double>*>& constants,
    const std::vector<int>& live_outs, const std::vector<char>& slot_active,
    Segment* out);

}  // namespace stanli

#endif
