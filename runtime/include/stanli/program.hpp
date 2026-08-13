// One compiled register program, two callers.
//
// An ODE right-hand side and a tape island are the same machine: a flat
// instruction list over a register file, compiled once at load time and
// run templated on the scalar -- double for values, var when stan-math's
// autodiff needs to see the arithmetic. They grew separately, so they had
// two instruction sets that overlapped on eleven opcodes and disagreed on
// the rest: the ODE side had branches and comparisons and no densities,
// the island side had densities, reductions and ranges and no branches.
//
// This is the union. Each caller keeps its own entry contract (ode_prog.hpp
// seeds t/y/theta/x_r per integrator step; island.hpp seeds live-ins and
// replays the backward under nested autodiff) and its own register file,
// because an island may contain an ODE call and a program must not be
// walking over another program's registers.
//
// Constants live in `pool` rather than in the instruction: it keeps Instr
// at 24 bytes, and a program that absorbs a data array needs the pool
// anyway.
#ifndef STANLI_PROGRAM_HPP
#define STANLI_PROGRAM_HPP

#include <stanli/program_density.hpp>

#include <stan/math.hpp>

#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace stanli {

struct KernelCtx;  // graph.hpp; only CALL's helpers touch it

// Structural facts used by the program compilers and the generated-adjoint
// pass. The evaluator's arithmetic stays in the explicit switch below: its
// grouping is observable and deliberately mirrors stan-math. These facts are
// different -- output/range shape and values that must survive until reverse
// -- and having one row per instruction prevents several classification
// switches from drifting apart.
enum ProgramOpFlag : uint16_t {
  kProgramNoInputs = 1u << 0,
  kProgramNoAdjoint = 1u << 1,
  kProgramRangeA = 1u << 2,
  kProgramRangeB = 1u << 3,
  kProgramSaveA = 1u << 4,
  kProgramSaveB = 1u << 5,
  kProgramSaveC = 1u << 6,
  kProgramSaveOut = 1u << 7,
  kProgramNoOutput = 1u << 8,
  kProgramRangeOutput = 1u << 9,
};

#define STANLI_PROGRAM_CODE_LIST(X)                                       \
  X(CONST, kProgramNoInputs)                                              \
  X(CONSTR, kProgramNoInputs | kProgramRangeOutput)                       \
  X(MOV, 0)                                                               \
  X(MOVR, kProgramRangeA | kProgramRangeOutput)                           \
  X(ADD, 0)                                                               \
  X(SUB, 0)                                                               \
  X(MUL, kProgramSaveA | kProgramSaveB)                                   \
  X(DIV, kProgramSaveA | kProgramSaveB)                                   \
  X(POW, kProgramSaveA | kProgramSaveB | kProgramSaveOut)                 \
  X(FMAX, kProgramSaveA | kProgramSaveB)                                  \
  X(FMIN, kProgramSaveA | kProgramSaveB)                                  \
  X(NEG, 0)                                                               \
  X(EXP, kProgramSaveOut)                                                 \
  X(LOG, kProgramSaveA)                                                   \
  X(SQRT, kProgramSaveOut)                                                \
  X(SQUARE, kProgramSaveA)                                                \
  X(INV, kProgramSaveA)                                                   \
  X(FABS, kProgramSaveA)                                                  \
  X(INV_LOGIT, kProgramSaveOut)                                           \
  X(LOG1M, kProgramSaveA)                                                 \
  X(TANH, kProgramSaveA)                                                  \
  X(GT, 0)                                                                \
  X(GE, 0)                                                                \
  X(LT, 0)                                                                \
  X(LE, 0)                                                                \
  X(EQ, 0)                                                                \
  X(NE, 0)                                                                \
  X(JZ, kProgramNoAdjoint | kProgramNoOutput)                             \
  X(JMP, kProgramNoAdjoint | kProgramNoOutput)                            \
  X(LOG_RANGE, kProgramRangeA | kProgramSaveA | kProgramRangeOutput)      \
  X(EXP_RANGE, kProgramRangeA | kProgramSaveOut | kProgramRangeOutput)    \
  X(DOT, kProgramRangeA | kProgramRangeB | kProgramSaveA | kProgramSaveB) \
  X(LSE_RANGE, kProgramRangeA | kProgramSaveA | kProgramSaveOut)          \
  X(SOFTMAX, kProgramRangeA | kProgramSaveOut | kProgramRangeOutput)      \
  X(LSE2, kProgramSaveA | kProgramSaveB)                                  \
  X(LOG_MIX, kProgramSaveA | kProgramSaveB | kProgramSaveC)               \
  X(FMA, kProgramSaveA | kProgramSaveB)                                   \
  X(DENSITY, 0)                                                           \
  X(CALL, 0)

struct Program {
  enum Code : uint8_t {
#define STANLI_PROGRAM_ENUM(name, flags) name,
    STANLI_PROGRAM_CODE_LIST(STANLI_PROGRAM_ENUM)
#undef STANLI_PROGRAM_ENUM
    // CONST/CONSTR, MOV/MOVR, arithmetic, comparisons, jumps, ranged
    // arithmetic, densities, and CALL appear above in that order. Their
    // exact execution semantics live in run_program below.
    // Any scalar continuous density: `len` selects which
    // (program_density.hpp). One opcode rather than one per density is
    // what lets the machine speak the runtime's whole list instead of a
    // hand-picked subset of it.
    //
    // Arguments live in `a`, `b`, `c`, and in the contiguous run starting
    // at `a` for the five densities that take four (student_t,
    // skew_normal, exp_mod_normal, pareto_type_2,
    // skew_double_exponential). Two forms rather than always the run,
    // because making the common ones contiguous means copying their
    // arguments into a fresh block: measured, that cost the HMM regions
    // ~35% more registers and instructions and pushed four of them --
    // each 1.5x or better -- back over the carve estimate's line.
    //
    // propto-OFF only (the island carver refuses propto). With no
    // term-dropping the value does not depend on which arguments are
    // autodiff, so binding all of them as T reproduces the scalar op's
    // value exactly; the extra partials computed for data arguments are
    // discarded when the executor hands the island a null adjoint.
    // Any graph kernel, by opcode: the payload is calls[a]. This is the
    // union point with the graph executor -- one instruction gives the
    // register machine the graph's whole vocabulary, and its derivative
    // is the kernel's own backward rather than a transcribed rule. The
    // kernels compute on doubles, so only run_program<double> can execute
    // one; the carver keeps a CALL-bearing island only when the generated
    // adjoint exists, so the var replay never meets it.
  };
  struct Instr {
    Code code = CONST;
    int32_t dst = 0, a = 0, b = 0, c = 0;
    int32_t len = 0;
  };

  // A CALL's payload: which kernel, and which register ranges stand in
  // for its slots. `scratch` is a range inside the register file, so the
  // partials the forward stashes are retained for the backward the same
  // way every value is. `bwd_in`/`bwd_out` are where the VALUES live at
  // backward time -- the same registers, unless the adjoint generator
  // had to checkpoint them (some kernel backwards re-read their inputs;
  // backward_ignores_input_values is a whitelist, not a guarantee).
  struct Call {
    uint16_t opcode = 0;
    uint8_t variant = 0;
    int8_t n_in = 0;
    int32_t in[6] = {0, 0, 0, 0, 0, 0};
    int32_t in_len[6] = {0, 0, 0, 0, 0, 0};
    int32_t out = 0;
    int32_t out_len = 0;
    int32_t scratch = 0;
    int32_t scratch_len = 0;
    int32_t bwd_in[6] = {0, 0, 0, 0, 0, 0};
    int32_t bwd_out = 0;
    std::vector<int> idata;
  };

  std::vector<Instr> code;
  std::vector<Call> calls;   // CALL payloads, indexed by Instr::a
  std::vector<double> pool;  // CONSTR data
  int n_regs = 0;
  std::vector<int> out_regs;  // the values the caller reads back
};

struct ProgramOpSpec {
  const char* name;
  uint16_t flags;

  constexpr bool has(ProgramOpFlag flag) const {
    return (flags & static_cast<uint16_t>(flag)) != 0;
  }
};

inline constexpr ProgramOpSpec kProgramOpSpecs[] = {
#define STANLI_PROGRAM_SPEC(name, flags) {#name, static_cast<uint16_t>(flags)},
    STANLI_PROGRAM_CODE_LIST(STANLI_PROGRAM_SPEC)
#undef STANLI_PROGRAM_SPEC
};
#undef STANLI_PROGRAM_CODE_LIST

inline constexpr size_t program_code_count() {
  return sizeof(kProgramOpSpecs) / sizeof(kProgramOpSpecs[0]);
}

inline constexpr const ProgramOpSpec& program_code_spec(Program::Code code) {
  return kProgramOpSpecs[static_cast<size_t>(code)];
}

inline constexpr int program_output_len(const Program::Instr& instr) {
  const ProgramOpSpec& spec = program_code_spec(instr.code);
  return spec.has(kProgramNoOutput)      ? 0
         : spec.has(kProgramRangeOutput) ? instr.len
                                         : 1;
}

static_assert(program_code_count() == static_cast<size_t>(Program::CALL) + 1,
              "every Program::Code needs exactly one ProgramOpSpec");

// Assemble the forward context for `call` over the register file `reg`.
// Backward-only fields are left null; run_adjoint fills its own.
KernelCtx call_fwd_ctx(const Program::Call& call, double* reg);

// Run one CALL forward. Out of line: KernelCtx lives in graph.hpp and
// the kernel table in the executor, neither of which this header needs
// for anything else.
void run_call(const Program::Call& call, double* reg);

// Run `p` over `reg`, which the caller has seeded and sized to at least
// p.n_regs. The compilers guarantee every register is written before it is
// read, so a reused file never leaks a previous call's values.
template <typename T>
void run_program(const Program& p, T* reg) {
  using VecT = Eigen::Matrix<T, Eigen::Dynamic, 1>;
  const int64_t n = (int64_t)p.code.size();
  for (int64_t pc = 0; pc < n; ++pc) {
    const Program::Instr& I = p.code[(size_t)pc];
    // `dst` is a register for everything but the jumps, where it is an
    // instruction index -- so it is only dereferenced in the cases that
    // actually write a register.
    auto d = [&]() -> T& { return reg[(size_t)I.dst]; };
    auto ra = [&]() -> const T& { return reg[(size_t)I.a]; };
    auto rb = [&]() -> const T& { return reg[(size_t)I.b]; };
    switch (I.code) {
      // Scalar constants get their own opcode: a right-hand side is mostly
      // scalars, and going through the ranged form cost the ODE models 3-4%
      // for the loop setup the compiler cannot see is one iteration.
      case Program::CONST:
        d() = T(p.pool[(size_t)I.a]);
        break;
      case Program::CONSTR:
        for (int32_t i = 0; i < I.len; ++i)
          reg[(size_t)(I.dst + i)] = T(p.pool[(size_t)(I.a + i)]);
        break;
      case Program::MOV:
        d() = ra();
        break;
      case Program::MOVR:
        for (int32_t i = 0; i < I.len; ++i)
          reg[(size_t)(I.dst + i)] = reg[(size_t)(I.a + i)];
        break;
      case Program::ADD:
        d() = ra() + rb();
        break;
      case Program::SUB:
        d() = ra() - rb();
        break;
      case Program::MUL:
        d() = ra() * rb();
        break;
      case Program::DIV:
        d() = ra() / rb();
        break;
      case Program::POW:
        d() = stan::math::pow(ra(), rb());
        break;
      case Program::FMAX:
        d() = stan::math::fmax(ra(), rb());
        break;
      case Program::FMIN:
        d() = stan::math::fmin(ra(), rb());
        break;
      case Program::NEG:
        d() = -ra();
        break;
      case Program::EXP:
        d() = stan::math::exp(ra());
        break;
      case Program::LOG:
        d() = stan::math::log(ra());
        break;
      case Program::SQRT:
        d() = stan::math::sqrt(ra());
        break;
      case Program::SQUARE:
        d() = stan::math::square(ra());
        break;
      case Program::INV:
        d() = stan::math::inv(ra());
        break;
      case Program::FABS:
        d() = stan::math::fabs(ra());
        break;
      case Program::INV_LOGIT:
        d() = stan::math::inv_logit(ra());
        break;
      case Program::LOG1M:
        d() = stan::math::log1m(ra());
        break;
      case Program::TANH:
        d() = stan::math::tanh(ra());
        break;
      case Program::GT:
        d() = T(stan::math::value_of(ra()) > stan::math::value_of(rb()));
        break;
      case Program::GE:
        d() = T(stan::math::value_of(ra()) >= stan::math::value_of(rb()));
        break;
      case Program::LT:
        d() = T(stan::math::value_of(ra()) < stan::math::value_of(rb()));
        break;
      case Program::LE:
        d() = T(stan::math::value_of(ra()) <= stan::math::value_of(rb()));
        break;
      case Program::EQ:
        d() = T(stan::math::value_of(ra()) == stan::math::value_of(rb()));
        break;
      case Program::NE:
        d() = T(stan::math::value_of(ra()) != stan::math::value_of(rb()));
        break;
      case Program::JZ:
        if (stan::math::value_of(ra()) == 0.0) pc = I.dst - 1;
        break;
      case Program::JMP:
        pc = I.dst - 1;
        break;
      case Program::LOG_RANGE:
        for (int32_t i = 0; i < I.len; ++i)
          reg[(size_t)(I.dst + i)] = stan::math::log(reg[(size_t)(I.a + i)]);
        break;
      case Program::EXP_RANGE:
        for (int32_t i = 0; i < I.len; ++i)
          reg[(size_t)(I.dst + i)] = stan::math::exp(reg[(size_t)(I.a + i)]);
        break;
      case Program::DOT: {
        Eigen::Map<const VecT> a(&reg[(size_t)I.a], I.len);
        Eigen::Map<const VecT> b(&reg[(size_t)I.b], I.len);
        if constexpr (std::is_same_v<T, double>) {
          // Bitwise-match OP_DOT's kernel: array product, Eigen redux.
          d() = (a.array() * b.array()).sum();
        } else {
          d() = stan::math::dot_product(a, b);
        }
        break;
      }
      case Program::LSE_RANGE: {
        Eigen::Map<const VecT> a(&reg[(size_t)I.a], I.len);
        d() = stan::math::log_sum_exp(a);
        break;
      }
      case Program::SOFTMAX: {
        Eigen::Map<const VecT> a(&reg[(size_t)I.a], I.len);
        const VecT s = stan::math::softmax(a);
        for (int32_t i = 0; i < I.len; ++i) reg[(size_t)(I.dst + i)] = s(i);
        break;
      }
      case Program::LSE2:
        d() = stan::math::log_sum_exp(ra(), rb());
        break;
      case Program::LOG_MIX:
        d() = stan::math::log_mix(ra(), rb(), reg[(size_t)I.c]);
        break;
      case Program::FMA:
        d() = stan::math::fma(ra(), rb(), reg[(size_t)I.c]);
        break;
        // One call for every scalar continuous density the runtime has;
      // program_density.cpp holds the switch, so the 27 instantiations
      // are paid in one translation unit instead of in every one that
      // runs a program.
      case Program::CALL:
        if constexpr (std::is_same_v<T, double>) {
          run_call(p.calls[(size_t)I.a], reg);
        } else {
          // Kernels are double machinery; a program that reaches here
          // under var was carved wrong, and saying so beats corrupting
          // a gradient.
          throw std::logic_error("CALL instruction in a var replay");
        }
        break;
      case Program::DENSITY: {
        const int ar = program_density_arity(I.len);
        if (ar > 3) {
          d() = program_density<T>(I.len, &reg[(size_t)I.a]);
          break;
        }
        T args[3];
        args[0] = ra();
        if (ar > 1) args[1] = rb();
        if (ar > 2) args[2] = reg[(size_t)I.c];
        d() = program_density<T>(I.len, args);
        break;
      }
    }
  }
}

template <typename T>
inline void run_program(const Program& p, std::vector<T>& reg) {
  run_program(p, reg.data());
}

}  // namespace stanli

#endif
