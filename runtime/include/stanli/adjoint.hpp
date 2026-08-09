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
//   adjoint   t = adj[d]; adj[d] = 0; adj[a] += val[b] * t; adj[b] += val[a] * t
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

// One forward instruction, differentiated. `dst`/`a`/`b`/`c` index the
// ADJOINT file and are the forward instruction's own registers; `vd`/`va`/
// `vb`/`vc` index the VALUE file and are those same registers unless the
// generator had to checkpoint them.
struct AdjInstr {
  Program::Code code = Program::CONST;
  int32_t dst = 0, a = 0, b = 0, c = 0;
  int32_t len = 0;
  int32_t vd = 0, va = 0, vb = 0, vc = 0;
};

struct AdjProgram {
  std::vector<AdjInstr> code;  // in reverse execution order
  bool empty() const { return code.empty(); }
};

// Accumulate adjoints backwards through `ap`. `val` is the forward register
// file as the forward pass left it (checkpoints included); `adj` is the
// adjoint file, zeroed by the caller and seeded at the live-out registers.
// Reads `val`, writes `adj`, allocates nothing.
void run_adjoint(const AdjProgram& ap, const double* val, double* adj);

// Generate the adjoint of `fwd`, appending checkpoint saves to `fwd` and
// growing its register count. Returns false and leaves both untouched when
// the program contains something the generator does not differentiate, in
// which case the caller keeps the var replay.
bool gen_adjoint(Program& fwd, AdjProgram* out);

}  // namespace stanli

#endif
