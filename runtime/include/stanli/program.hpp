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
// small, and a program that absorbs a data array needs the pool
// anyway.
#ifndef STANLI_PROGRAM_HPP
#define STANLI_PROGRAM_HPP

#include <stanli/callable_transform.hpp>
#include <stanli/extrema_grouping.hpp>
#include <stanli/kernel_types.hpp>
#include <stanli/message.hpp>
#include <stanli/optable.hpp>
#include <stanli/program_density.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace stanli {

using KernelFn = void (*)(KernelCtx&);

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
  kProgramReadB = 1u << 10,
  kProgramReadC = 1u << 11,
};

// EXTREMA_RANGE's `c` immediate. The register file has already materialized
// every input contiguously, but the source expression's Eigen traversal is
// still observable for NaNs, signed zero, ties, and adjoint selection. Keep
// that grouping beside the integer-surface marker instead of guessing from
// the materialized address at execution time.
inline constexpr int32_t kProgramExtremaInteger = 1 << 0;
inline constexpr int32_t kProgramExtremaScalar = 1 << 1;
inline constexpr int32_t kProgramExtremaPhased = 1 << 2;
inline constexpr int32_t kProgramExtremaPhaseShift = 3;

// `a` is an operand wherever kProgramNoInputs is absent; b and c are not,
// and the ones that are not hold register zero rather than nothing, so
// which registers a program actually reads needs saying. DENSITY's arity
// decides its own (program_density.hpp) and CALL's payload decides its own.
#define STANLI_PROGRAM_CODE_LIST(X)                                           \
  X(CONST, kProgramNoInputs)                                                  \
  X(CONSTR, kProgramNoInputs | kProgramRangeOutput)                           \
  X(MOV, 0)                                                                   \
  X(MOVR, kProgramRangeA | kProgramRangeOutput)                               \
  X(ADD, kProgramReadB)                                                       \
  X(SUB, kProgramReadB)                                                       \
  X(MUL, kProgramReadB | kProgramSaveA | kProgramSaveB)                       \
  X(DIV, kProgramReadB | kProgramSaveA | kProgramSaveB | kProgramSaveOut)     \
  X(IDIV, kProgramReadB | kProgramNoAdjoint)                                  \
  /* len holds the PowZeroBaseLaw; a RANGE's law field carries it instead. */ \
  X(POW, kProgramReadB | kProgramSaveA | kProgramSaveB | kProgramSaveOut)     \
  X(FMAX, kProgramReadB | kProgramSaveA | kProgramSaveB)                      \
  X(FMIN, kProgramReadB | kProgramSaveA | kProgramSaveB)                      \
  X(NEG, 0)                                                                   \
  X(EXP, kProgramSaveOut)                                                     \
  X(LOG, kProgramSaveA)                                                       \
  X(SQRT, kProgramSaveOut)                                                    \
  X(SQUARE, kProgramSaveA)                                                    \
  X(INV, kProgramSaveA)                                                       \
  X(FABS, kProgramSaveA)                                                      \
  X(INV_LOGIT, kProgramSaveOut)                                               \
  X(LOG1M, kProgramSaveA)                                                     \
  X(LOG1P_EXP, kProgramSaveA)                                                 \
  X(TANH, kProgramSaveA)                                                      \
  X(GT, kProgramReadB)                                                        \
  X(GE, kProgramReadB)                                                        \
  X(LT, kProgramReadB)                                                        \
  X(LE, kProgramReadB)                                                        \
  X(EQ, kProgramReadB)                                                        \
  X(NE, kProgramReadB)                                                        \
  X(DYN_INDEX, kProgramReadB | kProgramNoAdjoint)                             \
  /* b selects max (1) or min (0); c stores kProgramExtrema* metadata. */     \
  X(EXTREMA_RANGE, kProgramRangeA | kProgramNoAdjoint)                        \
  X(JZ, kProgramNoAdjoint | kProgramNoOutput)                                 \
  X(JMP, kProgramNoInputs | kProgramNoAdjoint | kProgramNoOutput)             \
  X(LOG_RANGE, kProgramRangeA | kProgramSaveA | kProgramRangeOutput)          \
  X(EXP_RANGE, kProgramRangeA | kProgramSaveOut | kProgramRangeOutput)        \
  X(DOT, kProgramRangeA | kProgramRangeB | kProgramReadB | kProgramSaveA |    \
             kProgramSaveB)                                                   \
  X(LSE_RANGE, kProgramRangeA | kProgramSaveA | kProgramSaveOut)              \
  X(SOFTMAX, kProgramRangeA | kProgramSaveOut | kProgramRangeOutput)          \
  X(LSE2, kProgramReadB | kProgramSaveA | kProgramSaveB)                      \
  X(LOG_DIFF_EXP, kProgramReadB | kProgramSaveA | kProgramSaveB)              \
  X(LOG_MIX, kProgramReadB | kProgramReadC | kProgramSaveA | kProgramSaveB |  \
                 kProgramSaveC)                                               \
  X(FMA, kProgramReadB | kProgramReadC | kProgramSaveA | kProgramSaveB)       \
  X(DIAG_PRE_MULTIPLY, kProgramReadB | kProgramNoAdjoint)                     \
  X(DIAG_POST_MULTIPLY, kProgramReadB | kProgramNoAdjoint)                    \
  X(MDIVIDE_LEFT, kProgramRangeA | kProgramRangeB | kProgramReadB |           \
                      kProgramRangeOutput | kProgramNoAdjoint)                \
  X(MDIVIDE_RIGHT_SPD, kProgramRangeA | kProgramRangeB | kProgramReadB |      \
                           kProgramRangeOutput | kProgramNoAdjoint)           \
  X(DENSITY, 0)                                                               \
  X(CALL, 0)                                                                  \
  X(TRANSFORM, kProgramNoInputs | kProgramNoAdjoint | kProgramNoOutput)       \
  X(PRINT, kProgramNoInputs | kProgramNoAdjoint | kProgramNoOutput)           \
  X(REJECT, kProgramNoInputs | kProgramNoAdjoint | kProgramNoOutput)          \
  X(DENSITY_VEC, kProgramNoAdjoint)                                           \
  /* An elementwise rule (sub) over len elements; see Instr. */               \
  X(RANGE, 0)

struct Program {
  enum Code : uint8_t {
#define STANLI_PROGRAM_ENUM(name, flags) name,
    STANLI_PROGRAM_CODE_LIST(STANLI_PROGRAM_ENUM)
#undef STANLI_PROGRAM_ENUM
    // CONST/CONSTR, MOV/MOVR, arithmetic, comparisons, jumps, ranged
    // arithmetic, densities, and CALL appear above in that order. Their
    // exact execution semantics live in run_program below.
    // Any scalar continuous density or distribution function: `len` selects
    // which (program_density.hpp). One opcode rather than one per function is
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
    // value exactly. The backward is where activity matters, and it is
    // the generated adjoint that carries the per-argument mask
    // (adjoint.hpp).
    // Any graph kernel, by opcode: the payload is calls[a]. This is the
    // union point with the graph executor -- one instruction gives the
    // register machine the graph's whole vocabulary, and its derivative
    // is the kernel's own backward rather than a transcribed rule. The
    // kernels compute their values and partials on doubles. A generated
    // adjoint invokes the backward directly; var replay uses a small adapter
    // that exposes its output adjoints to the same backward implementation.
  };
  struct Instr {
    Code code = CONST;
    // RANGE only: the elementwise rule applied to each of `len` elements,
    // which operands stay at one register, and the rule's law where it has
    // one (POW, FMAX, FMIN).
    uint8_t sub = 0;
    uint8_t bcast = 0;
    uint8_t law = 0;
    int32_t dst = 0, a = 0, b = 0, c = 0;
    int32_t len = 0;
    Instr() = default;
    Instr(Code code_, int32_t dst_, int32_t a_ = 0, int32_t b_ = 0,
          int32_t c_ = 0, int32_t len_ = 0)
        : code(code_), dst(dst_), a(a_), b(b_), c(c_), len(len_) {}
  };

  // A CALL's payload: which kernel, and which register ranges stand in
  // for its slots. `scratch` is a range inside the register file, so the
  // partials the forward stashes are retained for the backward the same
  // way every value is. The adjoint generator normalizes each CALL
  // instruction to its own payload and binds checkpointed value and compact
  // adjoint ranges below (adjoint.hpp).
  struct Call {
    uint16_t opcode = 0;
    uint8_t variant = 0;
    // Inputs that receive derivatives when CALL is replayed over var. Integer
    // lanes are values in the register file but are never autodiff operands.
    uint8_t input_adjoint_mask = 0x3f;
    int8_t n_in = 0;
    // Resolved once when the call site is built. A registered kernel's
    // function identity is stable, so repeated table lookup during program
    // execution can add no information. The generated adjoint uses the same
    // bound `backward` pointer.
    KernelFn forward = nullptr;
    KernelFn backward = nullptr;
    int32_t in[6] = {0, 0, 0, 0, 0, 0};
    int32_t in_len[6] = {0, 0, 0, 0, 0, 0};
    int32_t out = 0;
    int32_t out_len = 0;
    int32_t scratch = 0;
    int32_t scratch_len = 0;
    std::vector<int> idata;
    // Optional graph-kernel metadata. The shared owner lets a CALL retain the
    // same solver/callback specification an ordinary graph Op points at.
    std::shared_ptr<void> udata_owner;
    // Generated reverse binding. gen_adjoint normalizes CALL instructions to
    // one payload each, then caches their checkpointed value ranges and
    // compact adjoint ranges here. Forward-only Programs leave these zero.
    int32_t bwd_value_in[6] = {0, 0, 0, 0, 0, 0};
    int32_t bwd_adj_in[6] = {0, 0, 0, 0, 0, 0};
    int32_t bwd_value_out = 0;
    int32_t bwd_adj_out = 0;
  };

  // A PRINT or REJECT payload: the shared literal template plus the Program-
  // specific register ranges supplying its runtime values. A var replay skips
  // PRINT because it must not repeat an observable effect; REJECT never gets
  // a replay because its double forward already threw.
  struct Message {
    MessageSpec spec;
    std::vector<int32_t> value_reg;
    std::vector<int32_t> value_len;
  };

  // A callable constraint transform. Unlike CALL this is scalar-templated:
  // runtime-control programs execute it for both double and var, while its
  // kind comes from the same descriptor graph lowering uses.
  struct Transform {
    CallableTransformKind kind = CallableTransformKind::Ordered;
    TransformDirection direction = TransformDirection::Constrain;
    int8_t n_in = 0;
    int32_t in[3] = {0, 0, 0};
    int32_t in_len[3] = {0, 0, 0};
    int32_t out = 0;
    int32_t out_len = 0;
    int32_t jac = 0;
    int32_t batch = 1;
    int32_t inner_raw = 0;
    int32_t out_rows = 0;
    int32_t out_cols = 0;
  };

  // A DENSITY_VEC's payload: same density id DENSITY uses, but one or more
  // arguments is a same-length container rather than a scalar, evaluated
  // with one propto-OFF call the way CmdStan's generated code would (its
  // Eigen broadcasting, not N scalar calls summed by hand -- see
  // program_density_vec). `container_mask` bit k says argument k is `len`
  // consecutive registers starting at `arg_reg[k]`; a clear bit says it is
  // the one register at `arg_reg[k]`, same as DENSITY's scalar argument.
  // No `scratch`: unlike CALL, this runs under both double and var by
  // re-evaluating the same stan-math call, so there is no forward-computed
  // partial to carry to a hand-written backward.
  struct VecDensity {
    uint16_t density_id = 0;
    uint8_t arity = 0;
    uint8_t container_mask = 0;
    int32_t len = 0;
    int32_t arg_reg[4] = {0, 0, 0, 0};
  };

  std::vector<Instr> code;
  std::vector<Call> calls;            // CALL payloads, indexed by Instr::a
  std::vector<Transform> transforms;  // TRANSFORM payloads, indexed by Instr::a
  std::vector<Message> messages;      // PRINT/REJECT payloads, by Instr::a
  std::vector<double> pool;           // CONSTR data
  // DENSITY_VEC payloads, indexed by Instr::a.
  std::vector<VecDensity> vec_densities;
  int n_regs = 0;
  std::vector<int> out_regs;  // the values the caller reads back
};

template <typename T>
std::string render_program_message(const Program::Message& message,
                                   const T* reg) {
  return render_message(
      message.spec, message.value_reg.size(),
      [&](std::size_t k) { return static_cast<int64_t>(message.value_len[k]); },
      [&](std::size_t k, int64_t i) {
        return stan::math::value_of(reg[message.value_reg[k] + i]);
      });
}

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

// The rule an instruction applies: itself, or a RANGE's per-element one.
inline constexpr Program::Code program_rule(const Program::Instr& instr) {
  return instr.code == Program::RANGE ? static_cast<Program::Code>(instr.sub)
                                      : instr.code;
}

inline constexpr const ProgramOpSpec& program_spec_of(
    const Program::Instr& instr) {
  return program_code_spec(program_rule(instr));
}

inline constexpr int program_output_len(const Program::Instr& instr) {
  if (instr.code == Program::RANGE) return instr.len;
  if (instr.code == Program::DIAG_PRE_MULTIPLY ||
      instr.code == Program::DIAG_POST_MULTIPLY)
    return static_cast<int>(static_cast<int64_t>(instr.c) * instr.len);
  const ProgramOpSpec& spec = program_code_spec(instr.code);
  return spec.has(kProgramNoOutput)      ? 0
         : spec.has(kProgramRangeOutput) ? instr.len
                                         : 1;
}

// Whether operand k (a, b, c) is read.
inline constexpr bool program_reads(const Program::Instr& instr, int k) {
  const ProgramOpSpec& spec = program_spec_of(instr);
  if (k == 0) return !spec.has(kProgramNoInputs);
  if (k == 1) return spec.has(kProgramReadB);
  return spec.has(kProgramReadC);
}

inline constexpr int program_input_len(const Program::Instr& instr, int k) {
  if (instr.code == Program::RANGE)
    return ((instr.bcast >> k) & 1u) ? 1 : instr.len;
  const ProgramOpSpec& spec = program_code_spec(instr.code);
  if (k == 0) return spec.has(kProgramRangeA) ? instr.len : 1;
  if (k == 1) return spec.has(kProgramRangeB) ? instr.len : 1;
  return 1;
}

static_assert(program_code_count() == static_cast<size_t>(Program::RANGE) + 1,
              "every Program::Code needs exactly one ProgramOpSpec");

// Drop the initializer fills and the copies the MIR spells out, then
// renumber away whatever registers that leaves unreferenced (program.cpp).
// `seeded` names the register ranges the caller writes before the program
// runs -- an ODE argument region, an island live-in -- and comes back in the
// new numbering along with the program.
void compact_program(Program& p, std::vector<std::pair<int, int>>& seeded);

// Explicitly gate producer-destination forwarding and report whether it
// changed the program. The original entry point above remains the public
// default (and preserves its ABI); islands use this helper to price their
// established compacted form before optimizing an accepted region.
bool compact_program_gated(Program& p, std::vector<std::pair<int, int>>& seeded,
                           bool enable_destination_forwarding);

// Assemble the forward context for `call` over the register file `reg`.
// Backward-only fields are left null; run_adjoint fills its own.
KernelCtx call_fwd_ctx(const Program::Call& call, double* reg);

// Resolve a manually constructed call site once. Production carvers already
// have the Kernel in hand and bind its pointers directly. False leaves the
// call unbound, so malformed or unavailable opcodes fail closed.
bool bind_call(Program::Call& call);

// A kernel's scratch_size takes an Op/Slot pair, not a call site, so a
// caller assembling a Program::Call by hand has to reconstruct that shape
// first. Null `scratch_size` means zero.
int64_t kernel_call_scratch(int64_t (*scratch_size)(const Op&, const Slot*),
                            uint16_t opcode, uint8_t variant, int8_t n_in,
                            const int32_t* in_len, int32_t out_len,
                            const int* idata, int64_t n_idata,
                            const void* udata);

// Replay a graph-kernel call on a var register file. The kernel still owns its
// value and pullback; this adapter only gathers/scatters the non-contiguous
// vari pointers used by a runtime-control program.
void run_call_var(const Program::Call& call, stan::math::var* reg);

// Run one CALL forward through its pre-resolved function. `state` is the
// caller's evaluation state, which is how a generated-quantities region
// reaches the draw stream OP_RNG needs; null for every other caller, and
// the RNG kernel rejects a null one rather than inventing a stream.
void run_call(const Program::Call& call, double* reg,
              EvalState* state = nullptr);

// Reuse a caller-owned transient context. Its pointer fields are rebound per
// site, while construction/defaulting of the full packet is not paid again.
void run_call(const Program::Call& call, double* reg, KernelCtx& ctx,
              EvalState* state);

// Run `p` over `reg`, which the caller has seeded and sized to at least
// p.n_regs. The compilers guarantee every register is written before it is
// read, so a reused file never leaks a previous call's values.
template <bool ReuseCallCtx>
struct ProgramCallCtx {};

template <>
struct ProgramCallCtx<true> {
  KernelCtx ctx;
};

void run_program_transform(const Program::Transform& tr, double* reg);
void run_program_transform(const Program::Transform& tr, stan::math::var* reg);

// A RANGE instruction: its rule over every element, broadcast operands held
// at one register (program.cpp).
void run_elementwise_range(const Program::Instr& I, double* reg);
void run_elementwise_range(const Program::Instr& I, stan::math::var* reg);

// fmax/fmin through the overload the operands' data-only classification
// selects. `law` carries operand activity (bit 0: a, bit 1: b; 0 is the
// legacy all-var form of a manually built payload). stan-math's rule is
// one and consistent -- a tie prefers the autodiff argument, the second
// when both are, and a double side never carries an adjoint (so a winning
// or NaN-poisoned constant routes nothing) -- but the rule is expressed
// over the operands' STATIC types, so an all-var replay of a mixed call
// answers differently than the mixed instantiation CmdStan's generated
// code compiles. value_of detaches the data side to run the same one.
template <typename T>
inline T program_extremum(bool maximum, uint8_t law, const T& a, const T& b) {
  const auto call = [maximum](const auto& x, const auto& y) -> T {
    return maximum ? T(stan::math::fmax(x, y)) : T(stan::math::fmin(x, y));
  };
  if constexpr (std::is_same_v<T, double>) {
    return call(a, b);
  } else {
    if (law == 0x1) return call(a, stan::math::value_of(b));
    if (law == 0x2) return call(stan::math::value_of(a), b);
    return call(a, b);
  }
}

// pow through the overload the exponent's static type selects, keeping the
// value std::pow gives so the double forward and the replay stay bitwise.
template <typename T>
inline T program_pow(uint8_t law, const T& a, const T& b) {
  if constexpr (std::is_same_v<T, double>) {
    return stan::math::pow(a, b);
  } else {
    if (law == kPowZeroBaseGuarded) return stan::math::pow(a, b);
    T base = a;
    const double av = stan::math::value_of(a);
    const double exponent = stan::math::value_of(b);
    return stan::math::make_callback_var(
        std::pow(av, exponent), [base, av, exponent, law](auto&& vi) mutable {
          if (av == 0.0) {
            base.adj() += pow_zero_base_partial(law, vi.adj(), av, exponent);
            return;
          }
          base.adj() += vi.adj() * vi.val() * exponent / av;
        });
  }
}

template <bool ReuseCallCtx, typename T>
__attribute__((aligned(64))) void run_program_impl(const Program& p, T* reg,
                                                   EvalState* state = nullptr) {
  using VecT = Eigen::Matrix<T, Eigen::Dynamic, 1>;
  ProgramCallCtx<ReuseCallCtx> call_ctx;
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
      case Program::IDIV:
        d() =
            T(stan::math::divide(static_cast<int>(stan::math::value_of(ra())),
                                 static_cast<int>(stan::math::value_of(rb()))));
        break;
      case Program::POW:
        d() = program_pow(static_cast<uint8_t>(I.len), ra(), rb());
        break;
      case Program::FMAX:
        d() = program_extremum(true, static_cast<uint8_t>(I.len), ra(), rb());
        break;
      case Program::FMIN:
        d() = program_extremum(false, static_cast<uint8_t>(I.len), ra(), rb());
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
      case Program::LOG1P_EXP:
        d() = stan::math::log1p_exp(ra());
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
      case Program::DYN_INDEX: {
        const double raw = stan::math::value_of(rb());
        if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < 1.0 ||
            raw > static_cast<double>(I.len))
          throw std::out_of_range("register-program index out of range");
        d() = reg[(size_t)(I.a + I.c + static_cast<int32_t>(raw) - 1)];
        break;
      }
      case Program::EXTREMA_RANGE: {
        const bool maximum = I.b != 0;
        const bool integer = (I.c & kProgramExtremaInteger) != 0;
        const bool scalar = (I.c & kProgramExtremaScalar) != 0;
        const bool phased = (I.c & kProgramExtremaPhased) != 0;
        if (integer && I.len == 0) {
          // The register file stores integers as doubles, so call the actual
          // integer overload solely to preserve Stan Math's empty-container
          // exception instead of returning a floating-point infinity.
          const std::vector<int> empty;
          if (maximum)
            (void)stan::math::max(empty);
          else
            (void)stan::math::min(empty);
        }
        if (I.len == 0) {
          d() = T(maximum ? -std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::infinity());
          break;
        }
        if (scalar) {
          T selected = reg[(size_t)I.a];
          for (int32_t i = 1; i < I.len; ++i) {
            const T& candidate = reg[(size_t)(I.a + i)];
            if (maximum ? stan::math::value_of(selected) <
                              stan::math::value_of(candidate)
                        : stan::math::value_of(candidate) <
                              stan::math::value_of(selected))
              selected = candidate;
          }
          d() = selected;
          break;
        }
        const int64_t phase =
            static_cast<int64_t>(I.c) >> kProgramExtremaPhaseShift;
        if constexpr (std::is_same_v<T, double>) {
          if (phased) {
            d() = extrema_phased(&reg[(size_t)I.a], I.len, phase, maximum);
          } else {
            const Eigen::Map<const Eigen::VectorXd> input(&reg[(size_t)I.a],
                                                          I.len);
            const auto owning_grouping = input.unaryExpr(
                Eigen::internal::core_cast_op<double, double>());
            d() = maximum ? stan::math::max(owning_grouping)
                          : stan::math::min(owning_grouping);
          }
        } else {
          // Packet/phased instructions are parameter-free: an active source
          // is classified scalar by ProgramCompiler. Recreate the double
          // value grouping during replay and keep the result constant, just
          // as generated Stan computes the extrema before promoting it into
          // any downstream var expression.
          std::vector<double> values((size_t)I.len);
          for (int32_t i = 0; i < I.len; ++i)
            values[(size_t)i] = stan::math::value_of(reg[(size_t)(I.a + i)]);
          if (phased) {
            d() = T(extrema_phased(values.data(), I.len, phase, maximum));
          } else {
            const Eigen::Map<const Eigen::VectorXd> input(values.data(), I.len);
            const auto owning_grouping = input.unaryExpr(
                Eigen::internal::core_cast_op<double, double>());
            d() = T(maximum ? stan::math::max(owning_grouping)
                            : stan::math::min(owning_grouping));
          }
        }
        break;
      }
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
      case Program::LOG_DIFF_EXP:
        d() = stan::math::log_diff_exp(ra(), rb());
        break;
      case Program::LOG_MIX:
        d() = stan::math::log_mix(ra(), rb(), reg[(size_t)I.c]);
        break;
      case Program::FMA:
        d() = stan::math::fma(ra(), rb(), reg[(size_t)I.c]);
        break;
      case Program::DIAG_PRE_MULTIPLY:
      case Program::DIAG_POST_MULTIPLY: {
        using MatT = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>;
        const int32_t rows = I.c, cols = I.len;
        // A zero-size result contributes neither a value nor an adjoint.
        // Avoid forming Maps from a null register file in the 0x0 case.
        if (rows == 0 || cols == 0) break;
        const int32_t vlen = I.code == Program::DIAG_PRE_MULTIPLY ? rows : cols;
        Eigen::Map<const VecT> v(reg + I.a, vlen);
        Eigen::Map<const MatT> m(reg + I.b, rows, cols);
        Eigen::Map<MatT> out(reg + I.dst, rows, cols);
        if (I.code == Program::DIAG_PRE_MULTIPLY)
          out = stan::math::diag_pre_multiply(v, m);
        else
          out = stan::math::diag_post_multiply(m, v);
        break;
      }
      case Program::MDIVIDE_LEFT: {
        using MatT = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>;
        using VecT2 = Eigen::Matrix<T, Eigen::Dynamic, 1>;
        const int32_t nrow = std::abs(I.c);
        if (nrow == 0 || I.len == 0) break;
        Eigen::Map<const MatT> divisor(reg + I.a, nrow, nrow);
        if (I.c < 0) {
          Eigen::Map<const VecT2> rhs(reg + I.b, nrow);
          Eigen::Map<VecT2> output(reg + I.dst, nrow);
          output = stan::math::mdivide_left(divisor, rhs);
        } else {
          const int32_t ncol = nrow == 0 ? 0 : I.len / nrow;
          Eigen::Map<const MatT> rhs(reg + I.b, nrow, ncol);
          Eigen::Map<MatT> output(reg + I.dst, nrow, ncol);
          output = stan::math::mdivide_left(divisor, rhs);
        }
        break;
      }
      case Program::MDIVIDE_RIGHT_SPD: {
        using MatT = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>;
        using RowT = Eigen::Matrix<T, 1, Eigen::Dynamic>;
        const int32_t ncol = std::abs(I.c);
        if (ncol == 0 || I.len == 0) break;
        Eigen::Map<const MatT> divisor(reg + I.a, ncol, ncol);
        if (I.c < 0) {
          Eigen::Map<const RowT> lhs(reg + I.b, ncol);
          Eigen::Map<RowT> output(reg + I.dst, ncol);
          output = stan::math::mdivide_right_spd(lhs, divisor);
        } else {
          const int32_t nrow = ncol == 0 ? 0 : I.len / ncol;
          Eigen::Map<const MatT> lhs(reg + I.b, nrow, ncol);
          Eigen::Map<MatT> output(reg + I.dst, nrow, ncol);
          output = stan::math::mdivide_right_spd(lhs, divisor);
        }
        break;
      }
      // One call for every scalar continuous probability function the runtime
      // has; program_density.cpp holds the switch, so the instantiations are
      // paid in one translation unit instead of in every one that runs a
      // program.
      case Program::CALL:
        if constexpr (std::is_same_v<T, double>) {
          if constexpr (ReuseCallCtx)
            run_call(p.calls[(size_t)I.a], reg, call_ctx.ctx, state);
          else
            run_call(p.calls[(size_t)I.a], reg, state);
        } else {
          run_call_var(p.calls[(size_t)I.a], reg);
        }
        break;
      case Program::TRANSFORM:
        run_program_transform(p.transforms[(size_t)I.a], reg);
        break;
      case Program::PRINT:
        if constexpr (std::is_same_v<T, double>)
          execute_message(MessageAction::Print,
                          render_program_message(p.messages[(size_t)I.a], reg));
        break;
      // reject(): the same exception CmdStan's generated code throws from
      // the same place, so the sampler counts it as a rejected proposal
      // rather than a failure. No adjoint reaches this -- the forward
      // already threw -- so there is nothing to do under var either.
      case Program::REJECT:
        execute_message(MessageAction::Reject,
                        render_program_message(p.messages[(size_t)I.a], reg));
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
      case Program::RANGE:
        run_elementwise_range(I, reg);
        break;
      case Program::DENSITY_VEC: {
        const Program::VecDensity& v = p.vec_densities[(size_t)I.a];
        d() = program_density_vec<T>(v.density_id, v.container_mask, v.len, reg,
                                     v.arg_reg);
        break;
      }
    }
  }
}

template <typename T>
void run_program(const Program& p, T* reg, EvalState* state = nullptr) {
  if constexpr (std::is_same_v<T, double>) {
    if (!p.calls.empty()) {
      run_program_impl<true>(p, reg, state);
      return;
    }
  }
  run_program_impl<false>(p, reg, state);
}

template <typename T>
inline void run_program(const Program& p, std::vector<T>& reg,
                        EvalState* state = nullptr) {
  run_program(p, reg.data(), state);
}

}  // namespace stanli

#endif
