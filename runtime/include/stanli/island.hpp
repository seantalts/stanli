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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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
  // Pure control-flow selectors need no arithmetic pullback: each live-out
  // seed routes to the live-in selected on the executed path.  The kernel
  // replays their constants/comparisons/copies on doubles while carrying
  // live-in identities, avoiding a nested var tape.  This remains separate
  // from native_adj because it consumes the packed live-in snapshot rather
  // than the generated adjoint's full forward value file.
  bool selector_adj = false;
  // For a generated adjoint over a forward-only CFG, one entry per final
  // forward instruction: the original PC whose execution it records, or -1
  // for an inserted value checkpoint.  The packed bits live immediately
  // after the forward value file in OP_ISLAND scratch.
  std::vector<int32_t> trace_pc;
  // Compact adjoint cells which can remain nonzero after a successful native
  // sweep: mapped live-ins, harvested by the caller, and explicitly seeded
  // live-outs. The generator maps, sorts, and deduplicates this immutable
  // reset plan after adj_reg is final. Sparse reset remains force-only; the
  // eligibility bit is a fail-closed size/density gate, not a selection flag.
  std::vector<int32_t> sparse_adj_clear_cells;
  bool sparse_adj_clear_eligible = false;
  // A traced CFG appends value checkpoints and may attach graph-kernel
  // payloads used only by its backward. Retain the immutable pre-generation
  // Program as the forward/backward oracle selected by STANLI_NO_NATIVE_ADJ.
  // Null for ordinary islands.
  std::shared_ptr<const Program> var_replay;
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

// Generate the same ordinary adjoint rules for a forward-only acyclic CFG.
// The executed forward PCs are recorded in a compact trace and filter the
// globally reversed instruction stream.  Kept as an explicit seam so loop
// steps whose scratch layout does not carry a trace continue to use only
// gen_adjoint.  False is transactional and leaves p untouched.
bool gen_cfg_adjoint(IslandProg& p);

// Build the optional block-level trace-filter plan for an already generated
// forward-only CFG adjoint. False is transactional and leaves an existing
// plan untouched; the ordinary per-instruction trace remains the fallback.
// Kept explicit for structural/fail-closed tests. Production calls it only
// under STANLI_CFG_ADJ_TRACE_BLOCKS=1.
bool prepare_cfg_trace_blocks(IslandProg& p);

// Greedily tag a small whitelist of scalar pairs inside an already validated
// CFG trace-block plan. False is transactional and leaves all tags unchanged.
// CALLs, ranged rules, control flow, and structured rules are never paired.
bool prepare_cfg_adjoint_superinstructions(IslandProg& p);

// Decide whether a generated CFG adjoint should be selected in production.
// Generation and selection are deliberately separate: tests and benchmarks
// can exercise every supported pullback, while measured regressions fail
// closed to var replay. STANLI_CFG_STRUCTURED_NATIVE=1 is the explicit A/B
// override for structured CFGs.
bool cfg_native_profitable(const IslandProg& p);

// Recognize the allocation-free selector backward above. Deliberately
// fail-closed: only pure, forward-only selector bytecode is admitted. The
// immutable Program is not changed.
bool supports_selector_adjoint(const IslandProg& p);

// After gen_adjoint has captured the original forward program, return a
// double-only clone that replaces sufficiently common SOFTMAX(3) instructions
// with calls to an allocation-free private Program kernel. Null means refused;
// the canonical bytecode stays unchanged.
std::shared_ptr<const Program> specialize_softmax3(const IslandProg& p,
                                                   size_t min_count = 32);

// Evaluate on T = double (forward) or stan::math::var (backward replay,
// inside the caller's nested_rev_autodiff). The register file is reused
// between calls. Not reentrant; islands cannot contain islands.
template <typename T>
void run_island(const IslandProg& p, const T* const* in, T* out) {
  const Program& execution =
      p.var_replay ? *p.var_replay : static_cast<const Program&>(p);
  static thread_local std::vector<T> reg;
  if ((int64_t)reg.size() < execution.n_regs)
    reg.resize((size_t)execution.n_regs);
  for (size_t k = 0; k < p.ins.size(); ++k) {
    const int input = p.ins[k].input >= 0 ? p.ins[k].input : (int)k;
    for (int i = 0; i < p.ins[k].len; ++i)
      reg[(size_t)(p.ins[k].reg + i)] = in[input][p.ins[k].offset + i];
  }

  run_program(execution, reg);

  for (size_t i = 0; i < execution.out_regs.size(); ++i)
    out[i] = reg[(size_t)execution.out_regs[i]];
}

struct Graph;  // graph.hpp

// A correctness-first compilation of an explicit contiguous graph fragment.
// Unlike carve_islands this is a construction seam, not an optimization: it
// has no minimum size, pricing, or automatic live-out discovery. The caller
// supplies output order and parameter-dependency provenance. Payload owners
// keep every Program::Call::udata pointer valid when the result outlives the
// source graph.
struct GraphFragmentProgram {
  IslandProg program;
  std::vector<int> live_in_slots;
  std::vector<std::shared_ptr<void>> udata_owners;
};

// Compile g.ops[op_begin, op_end). `live_out_slots` is ordered and determines
// program.out_regs packing. `fills` may be absorbed only for slots no graph op
// writes. `slot_active[s] != 0` means slot s depends on a model parameter and
// is applied to LiveIn::active before adjoint generation.
//
// CALLs may have vector/container outputs and may call OP_ISLAND, provided all
// non-null payloads have owners in g.udata_pool. Effects, RNG, OP_SCAN, out2,
// malformed ranges and programs whose CALL adjoint cannot be generated are
// refused transactionally. On false, `out` is unchanged and `diagnostic`, if
// supplied, describes the refusal.
bool compile_graph_fragment(
    const Graph& g, size_t op_begin, size_t op_end,
    const std::vector<int>& live_out_slots,
    const std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<uint8_t>& slot_active, GraphFragmentProgram* out,
    std::string* diagnostic = nullptr);

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
