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
#include <vector>

namespace stanli {

struct Program {
  enum Code : uint8_t {
    CONST,   // dst = pool[a]
    CONSTR,  // dst[0..len) = pool[a + i]
    MOV,     // dst = r[a]
    MOVR,    // dst[0..len) = r[a + i]
    ADD,
    SUB,
    MUL,
    DIV,  // dst = r[a] op r[b]
    POW,
    FMAX,
    FMIN,
    NEG,
    EXP,
    LOG,
    SQRT,
    SQUARE,  // dst = op(r[a])
    INV,
    FABS,
    INV_LOGIT,
    LOG1M,
    TANH,
    // Comparisons produce a plain 0/1 with no derivative, matching how
    // generated C++ evaluates them on values.
    GT,
    GE,
    LT,
    LE,
    EQ,
    NE,
    JZ,   // jump to `dst` when r[a] is zero
    JMP,  // jump to `dst`
    LOG_RANGE,
    EXP_RANGE,  // dst[i] = op(r[a+i]), i < len
    DOT,        // dst = sum_i r[a+i] * r[b+i]      (Eigen redux, as OP_DOT)
    LSE_RANGE,  // dst = log_sum_exp(r[a..a+len))
    SOFTMAX,    // dst[0..len) = softmax(r[a..a+len))
    LSE2,       // dst = log_sum_exp(r[a], r[b])
    LOG_MIX,    // dst = log_mix(r[a], r[b], r[c])
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
    DENSITY,
  };
  struct Instr {
    Code code = CONST;
    int32_t dst = 0, a = 0, b = 0, c = 0;
    int32_t len = 0;
  };

  std::vector<Instr> code;
  std::vector<double> pool;  // CONSTR data
  int n_regs = 0;
  std::vector<int> out_regs;  // the values the caller reads back
};

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
        // One call for every scalar continuous density the runtime has;
      // program_density.cpp holds the switch, so the 27 instantiations
      // are paid in one translation unit instead of in every one that
      // runs a program.
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
