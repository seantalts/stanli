// The adjoint of a register program, generated instead of replayed.
//
// An island's backward used to re-execute the whole program under
// stan::math::var: a vari allocated per operation, a virtual chain() per
// operation, and a nested tape torn down per call. That is bitwise-correct
// by construction -- it is the same var arithmetic CmdStan's generated code
// runs -- and it therefore costs what CmdStan costs, which is why the carver
// refused thirteen of the fourteen regions it could compile.
//
// This is the same derivative as a second pass over doubles. Reverse-mode
// source transformation over Program::Code, which is a closed set of about
// thirty-five opcodes:
//
//   forward   d = a * b
//   adjoint   t = adj[d]; adj[d] = 0;
//             adj[a] += val[b] * t; adj[b] += val[a] * t
//
// Every rule mirrors the corresponding stan-math rev implementation
// expression for expression -- operand grouping included, since the bar is
// bitwise agreement with the replay and not merely a correct derivative.
// tests/test_adjoint.cpp holds each opcode against the replay at fixed
// points, and STANLI_NO_NATIVE_ADJ=1 restores the replay at runtime, so the
// oracle is always one environment variable away.
//
// An adjoint instruction is its forward instruction read backwards, so it
// carries the same fields. What it adds is where to find VALUES: registers
// are mutable cells, and a register the forward overwrote no longer holds
// what the derivative needs. The generator finds those cases and has the
// forward save them into fresh checkpoint registers, which is the only
// analysis here that is not a table -- see gen_adjoint.
#ifndef STANLI_ADJOINT_HPP
#define STANLI_ADJOINT_HPP

#include <stanli/program.hpp>

#include <cstdint>
#include <vector>

namespace stanli {

// Optional, force-only scalar superinstructions. A nonzero tag on reverse
// instruction i means i and i+1 are executed by one dispatch. Tags are
// installed only inside a validated traced basic block; the second
// instruction remains in AdjProgram::code as the exact semantic oracle.
enum class AdjPair : uint8_t {
  None = 0,
  MovMov,
  MovConst,
  NegNeg,
  ConstMov,
  MulAdd,
  AddMul,
  MulMul,
  AddAdd,
  SubSub,
};

// One forward instruction, differentiated. `dst`/`a`/`b`/`c` index the
// ADJOINT file and are the forward instruction's own registers; `vd`/`va`/
// `vb`/`vc` index the VALUE file and are those same registers unless the
// generator had to checkpoint them.
struct AdjInstr {
  Program::Code code = Program::CONST;
  // DENSITY only: bit k set where argument k is downstream of a parameter.
  // Rides in the padding after `code`, as the graph's density ops carry
  // their activity in Op::variant.
  uint8_t mask = 0xf;
  // Also rides in that padding, preserving AdjInstr's size. None is the
  // ordinary interpreter; other values are validated AdjPair tags.
  AdjPair pair = AdjPair::None;
  int32_t dst = 0, a = 0, b = 0, c = 0;
  int32_t len = 0;
  int32_t vd = 0, va = 0, vb = 0, vc = 0;
  // Original forward instruction that produced this reverse instruction.
  // Negative for an ordinary straight-line adjoint.  A traced acyclic CFG
  // filters the reverse stream by this id, so only the path taken by the
  // double forward contributes.
  int32_t fwd_pc = -1;
};

// One maximal reverse range whose instructions all belong to one executed
// forward basic block. Ranges partition AdjProgram::code in order; `begin` is
// zero for the first range and the preceding range's `end` thereafter.
// `trace_pc` is any differentiated original PC in that block, whose packed
// execution bit is therefore shared by the whole range.
struct AdjTraceBlock {
  int32_t end = 0;
  int32_t trace_pc = -1;
};

struct AdjProgram {
  std::vector<AdjInstr> code;  // in reverse execution order
  // Optional force-only predecode for a validated forward-only CFG. Empty is
  // the established per-instruction trace filter. The plan changes neither
  // the trace representation nor reverse arithmetic/order.
  std::vector<AdjTraceBlock> trace_blocks;
  // Which adjoint cell each forward register accumulates into, normally
  // itself. A copy the forward never rewrites is the exception: it shares
  // its source's cell rather than moving a total across at the end, because
  // that is what the replay does -- a var copy shares a vari, so a read of
  // the copy lands on the source's adjoint in tape order. Collecting the
  // copy's contributions separately and transferring them in one lump would
  // regroup the sum and cost the last few bits.
  std::vector<int32_t> adj_reg;
  // Number of cells in the adjoint file. `adj_reg` is indexed by forward
  // register, but its entries are compact: registers made equivalent by a
  // copy share one cell, and the remaining equivalence classes are packed
  // densely. Checkpoint registers hold values only and are absent entirely.
  int n_regs = 0;
  // Number of original forward PCs represented by the optional packed path
  // trace.  Zero is the established straight-line/terminal-guard form.
  int trace_bits = 0;
  // True only when the force-only pair planner tagged at least one entry.
  bool has_pairs = false;
  bool empty() const { return adj_reg.empty(); }
};

// Accumulate adjoints backwards through `ap`. `val` is the forward register
// file as the forward pass left it (checkpoints included); `adj` is the
// adjoint file, zeroed by the caller and seeded at the live-out registers.
// Reads `val`, writes `adj`, allocates nothing.
//
// Seeding and harvesting go through `AdjProgram::adj_reg`: a copy the
// forward never rewrites shares one adjoint cell with its source, the way
// the replay's registers share one vari, so a live-out or live-in register
// may not have an adjoint cell of its own.
// `fwd` supplies normalized CALL payloads: one per CALL instruction, with a
// pre-resolved kernel backward and precomputed value/adjoint ranges.
void run_adjoint(const Program& fwd, const AdjProgram& ap, const double* val,
                 double* adj, const uint8_t* executed = nullptr);

}  // namespace stanli

#endif
