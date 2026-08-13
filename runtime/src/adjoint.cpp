// gen_adjoint: reverse-mode source transformation over Program (adjoint.hpp),
// and the interpreter that runs the result.
//
// Two things are worth knowing before editing either half.
//
// **The rules are stan-math's, transcribed.** Each case below is the
// expression from the corresponding stan/math/rev file, with the same
// grouping and the same operand order. `square` is `t * 2.0 * x`, left
// associated, because rev/fun/square.hpp writes it that way; `log1m` divides
// by `x - 1.0`, not by `-(1.0 - x)`; `tanh` recomputes cosh rather than using
// `1 - t^2`. These are not stylistic choices -- the pass is verified BITWISE
// against the var replay (tests/test_adjoint.cpp), and every one of them is a
// last-bit difference. If a rule ever needs changing, change it to match
// stan-math, and let the test say whether it did.
//
// **Densities keep the reuse.** A density's adjoint is one recorder call
// (rvar, recorder.hpp) plus a multiply-accumulate: stan-math computes the
// value and the partials in doubles with no tape, exactly as the scalar
// density ops already do. Nothing here differentiates a density by hand.
// recorder.hpp FIRST, before anything drags in stan-math proper: it is what
// registers rvar's traits and its value_of overloads, and stan-math's
// templates are only allowed to find them if they are declared by the time
// those templates are parsed. The density shards open with the same line for
// the same reason.
#include <stanli/recorder.hpp>

#include <stanli/adjoint.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include <stanli/program_density.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace stanli {
namespace {

// How many registers an instruction writes, starting at dst.
int out_len(const Program::Instr& I) {
  switch (I.code) {
    case Program::CONSTR:
    case Program::MOVR:
    case Program::LOG_RANGE:
    case Program::EXP_RANGE:
    case Program::SOFTMAX:
      return I.len;
    case Program::JZ:
    case Program::JMP:
      return 0;
    default:
      return 1;
  }
}

// Opcodes whose adjoint this pass knows. The jumps are the deliberate
// omission: reversing control flow needs the structured form (the if/else
// nesting the flat list has already lost), and inserting checkpoint saves
// would move the jump targets besides. Regions carrying them -- the ones
// lowering emits for parameter-dependent control flow -- keep the replay.
bool supported(Program::Code c) {
  switch (c) {
    case Program::JZ:
    case Program::JMP:
      return false;
    default:
      return true;
  }
}

}  // namespace

bool gen_adjoint(IslandProg& p) {
  Program& fwd = p;
  const std::vector<Program::Instr> orig = fwd.code;
  const int n0 = fwd.n_regs;
  for (const auto& I : orig)
    if (!supported(I.code)) return false;

  // Where each register was first and last written. A value the backward
  // needs survives in place exactly when no later instruction overwrites it;
  // a register written exactly once is what the copy aliasing below needs.
  std::vector<int> first_write((size_t)n0, -1), last_write((size_t)n0, -1);
  auto mark_write = [&](int i, int r) {
    if (r < 0 || r >= n0) return false;
    if (first_write[(size_t)r] < 0) first_write[(size_t)r] = i;
    last_write[(size_t)r] = i;
    return true;
  };
  for (int i = 0; i < (int)orig.size(); ++i) {
    if (orig[i].code == Program::CALL) {
      // A CALL's writes come from its payload: the output range and the
      // scratch its kernel stashes partials in.
      const Program::Call& call = fwd.calls[(size_t)orig[i].a];
      for (int k = 0; k < call.out_len; ++k)
        if (!mark_write(i, call.out + k)) return false;
      for (int k = 0; k < call.scratch_len; ++k)
        if (!mark_write(i, call.scratch + k)) return false;
      continue;
    }
    const int wl = out_len(orig[i]);
    for (int k = 0; k < wl; ++k)
      if (!mark_write(i, orig[i].dst + k)) return false;
  }

  // Registers whose adjoint cell has to stay their own. Two kinds:
  // live-ins, which are harvested by register id; and a density's
  // argument run, whose adjoints the backward walks as a range -- let a
  // copy alias one element of that run onto some unrelated cell and the
  // range stops being a range.
  std::vector<char> no_alias((size_t)n0, 0);
  for (const auto& li : p.ins)
    for (int k = 0; k < li.len; ++k) {
      if (li.reg + k >= n0) return false;
      no_alias[(size_t)(li.reg + k)] = 1;
    }
  for (const auto& I : orig) {
    if (I.code == Program::DENSITY && program_density_arity(I.len) > 3)
      for (int k = 0; k < program_density_arity(I.len); ++k)
        if (I.a + k >= 0 && I.a + k < n0) no_alias[(size_t)(I.a + k)] = 1;
    if (I.code == Program::CALL) {
      // The kernel's backward accumulates adjoints over whole ranges, so
      // every CALL range keeps identity adjoint cells.
      const Program::Call& call = fwd.calls[(size_t)I.a];
      for (int j = 0; j < call.n_in; ++j)
        for (int k = 0; k < call.in_len[j]; ++k)
          no_alias[(size_t)(call.in[j] + k)] = 1;
      for (int k = 0; k < call.out_len; ++k)
        no_alias[(size_t)(call.out + k)] = 1;
    }
  }

  // Two ranges that overlap without coinciding would have the elementwise
  // adjoint loop read a cell it has already zeroed. Coinciding is fine and
  // common (an in-place `x = exp(x)`); partial overlap is not emitted by
  // either front end, so refuse rather than reason about it.
  auto bad_overlap = [](int x, int y, int len) {
    return x != y && x < y + len && y < x + len;
  };
  // Every register an adjoint rule will READ, checked once here rather than
  // trusted per rule. The write side is bounded by the loop above; nothing
  // bounded the operands, and they index the same vectors.
  auto in_range = [&](int r, int len) { return r >= 0 && r + len <= n0; };
  for (const auto& I : orig) {
    const int reads =
        I.code == Program::CONST || I.code == Program::CONSTR ? 0 : 3;
    if (I.code == Program::DENSITY && program_density_arity(I.len) > 3 &&
        !in_range(I.a, program_density_arity(I.len)))
      return false;
    if (I.code == Program::CALL) {
      const Program::Call& call = fwd.calls[(size_t)I.a];
      for (int j = 0; j < call.n_in; ++j)
        if (!in_range(call.in[j], call.in_len[j])) return false;
      if (!in_range(call.out, call.out_len)) return false;
      if (call.scratch_len && !in_range(call.scratch, call.scratch_len))
        return false;
      continue;
    }
    const bool ranged_density =
        I.code == Program::DENSITY && program_density_arity(I.len) > 3;
    if (!ranged_density) {
      if (reads > 0 && !in_range(I.a, 1)) return false;
      if (reads > 1 && !in_range(I.b, 1)) return false;
      if (reads > 2 && !in_range(I.c, 1)) return false;
    }
    switch (I.code) {
      case Program::MOVR:
      case Program::LOG_RANGE:
      case Program::EXP_RANGE:
      case Program::SOFTMAX:
      case Program::LSE_RANGE:
        if (!in_range(I.a, I.len)) return false;
        break;
      case Program::DOT:
        if (!in_range(I.a, I.len) || !in_range(I.b, I.len)) return false;
        break;
      default:
        break;
    }
    switch (I.code) {
      case Program::MOVR:
      case Program::LOG_RANGE:
      case Program::EXP_RANGE:
      case Program::SOFTMAX:
        if (bad_overlap(I.dst, I.a, I.len)) return false;
        break;
      case Program::LSE_RANGE:
        if (I.dst >= I.a && I.dst < I.a + I.len) return false;
        break;
      case Program::DOT:
        if (I.dst >= I.a && I.dst < I.a + I.len) return false;
        if (I.dst >= I.b && I.dst < I.b + I.len) return false;
        break;
      default:
        break;
    }
  }

  std::vector<Program::Instr> ncode;
  ncode.reserve(orig.size());
  AdjProgram ap;
  ap.code.reserve(orig.size());
  ap.adj_reg.resize((size_t)n0);
  for (int r = 0; r < n0; ++r) ap.adj_reg[(size_t)r] = r;
  int n_regs = n0;

  // A copy the forward never rewrites shares its source's adjoint cell.
  // This is the replay's vari sharing, written down: `reg[d] = reg[a]` on
  // vars copies a POINTER, so every later read of either register lands on
  // one adjoint in tape order. Giving the copy its own cell and adding the
  // total back at the copy would be the same derivative grouped
  // differently, which shows up as a last-bit disagreement on exactly the
  // models islands were built for (iohmm_reg copies a 1,500-element state
  // vector per step).
  auto aliasable = [&](const Program::Instr& I, int i) {
    if (I.code != Program::MOV && I.code != Program::MOVR) return false;
    const int len = I.code == Program::MOV ? 1 : I.len;
    if (len <= 0) return false;
    if (I.dst < 0 || I.dst + len > n0 || I.a < 0 || I.a + len > n0)
      return false;
    for (int k = 0; k < len; ++k) {
      // Written once, by this instruction: any other writer would clear a
      // cell that now belongs to the source as well.
      if (first_write[(size_t)(I.dst + k)] != i) return false;
      if (last_write[(size_t)(I.dst + k)] != i) return false;
      // The source must not be rewritten later, or the two registers stop
      // holding the same value while sharing one adjoint.
      if (last_write[(size_t)(I.a + k)] > i) return false;
      if (no_alias[(size_t)(I.dst + k)]) return false;
    }
    return true;
  };

  // A range needs a checkpoint when any element is overwritten strictly
  // after i; the copy is one MOVR and the backward reads it instead.
  auto save_range = [&](int r, int len, int i) {
    bool need = false;
    for (int k = 0; k < len && !need; ++k)
      need = last_write[(size_t)(r + k)] > i;
    if (!need) return r;
    const int ck = n_regs;
    n_regs += len;
    Program::Instr S;
    S.code = len == 1 ? Program::MOV : Program::MOVR;
    S.dst = ck;
    S.a = r;
    S.len = len;
    ncode.push_back(S);
    return ck;
  };

  for (int i = 0; i < (int)orig.size(); ++i) {
    const Program::Instr& I = orig[i];
    if (I.code == Program::CALL) {
      // The kernel's backward may read its input VALUES, not just its
      // scratch (backward_ignores_input_values is a whitelist, not a
      // guarantee), and some read their output values too -- so both are
      // checkpointed whenever a later instruction overwrites them.
      Program::Call& call = fwd.calls[(size_t)I.a];
      for (int j = 0; j < call.n_in; ++j)
        call.bwd_in[j] = save_range(call.in[j], call.in_len[j], i);
      ncode.push_back(I);
      call.bwd_out = save_range(call.out, call.out_len, i);
      AdjInstr A;
      A.code = Program::CALL;
      A.a = I.a;
      ap.code.push_back(A);
      continue;
    }
    if (aliasable(I, i)) {
      const int len = I.code == Program::MOV ? 1 : I.len;
      for (int k = 0; k < len; ++k)
        ap.adj_reg[(size_t)(I.dst + k)] = ap.adj_reg[(size_t)(I.a + k)];
      ncode.push_back(I);
      continue;  // no adjoint instruction: the cells are already shared
    }
    AdjInstr A;
    A.code = I.code;
    A.dst = I.dst;
    A.a = I.a;
    A.b = I.b;
    A.c = I.c;
    A.len = I.len;
    A.va = I.a;
    A.vb = I.b;
    A.vc = I.c;
    A.vd = I.dst;
    const int wl = out_len(I);

    // An operand value is needed as it stood on ENTRY to this instruction,
    // so it must be saved when this instruction overwrites it (`d = d * b`
    // destroys the very value its own derivative reads) or when any later
    // one does.
    auto save_before = [&](int r, int len) {
      bool need = r < I.dst + wl && I.dst < r + len;
      for (int k = 0; k < len && !need; ++k)
        need = last_write[(size_t)(r + k)] > i;
      if (!need) return r;
      const int ck = n_regs;
      n_regs += len;
      Program::Instr S;
      S.code = len == 1 ? Program::MOV : Program::MOVR;
      S.dst = ck;
      S.a = r;
      S.len = len;
      ncode.push_back(S);
      return ck;
    };
    switch (I.code) {
      case Program::MUL:
      case Program::DIV:
      case Program::POW:
      case Program::FMAX:
      case Program::FMIN:
      case Program::LSE2:
        A.va = save_before(I.a, 1);
        A.vb = save_before(I.b, 1);
        break;
      case Program::LOG:
      case Program::SQUARE:
      case Program::INV:
      case Program::FABS:
      case Program::LOG1M:
      case Program::TANH:
        A.va = save_before(I.a, 1);
        break;
      case Program::LOG_MIX:
        A.va = save_before(I.a, 1);
        A.vb = save_before(I.b, 1);
        A.vc = save_before(I.c, 1);
        break;
      case Program::FMA:
        // d(a*b+c)/da = b, /db = a, /dc = 1: only a and b are re-read.
        A.va = save_before(I.a, 1);
        A.vb = save_before(I.b, 1);
        break;
      case Program::LOG_RANGE:
      case Program::LSE_RANGE:
        A.va = save_before(I.a, I.len);
        break;
      case Program::DOT:
        A.va = save_before(I.a, I.len);
        A.vb = save_before(I.b, I.len);
        break;
      case Program::DENSITY: {
        const int ar = program_density_arity(I.len);
        if (ar > 3) {
          A.va = save_before(I.a, ar);
        } else {
          A.va = save_before(I.a, 1);
          if (ar > 1) A.vb = save_before(I.b, 1);
          if (ar > 2) A.vc = save_before(I.c, 1);
        }
        break;
      }
      default:
        break;
    }

    ncode.push_back(I);

    // An output value is needed as this instruction LEFT it, so only a
    // later overwrite can lose it.
    auto save_after = [&](int r, int len) {
      bool need = false;
      for (int k = 0; k < len && !need; ++k)
        need = last_write[(size_t)(r + k)] > i;
      if (!need) return r;
      const int ck = n_regs;
      n_regs += len;
      Program::Instr S;
      S.code = len == 1 ? Program::MOV : Program::MOVR;
      S.dst = ck;
      S.a = r;
      S.len = len;
      ncode.push_back(S);
      return ck;
    };
    switch (I.code) {
      case Program::EXP:
      case Program::SQRT:
      case Program::INV_LOGIT:
      case Program::POW:
      case Program::LSE_RANGE:
        A.vd = save_after(I.dst, 1);
        break;
      case Program::EXP_RANGE:
      case Program::SOFTMAX:
        A.vd = save_after(I.dst, I.len);
        break;
      default:
        break;
    }

    // Adjoint operands go through the sharing map; value operands do not.
    // A range has to map to a range: aliasing builds contiguous maps from
    // contiguous copies, so this holds, and refusing is cheaper than
    // scattering the interpreter's loops.
    bool ok = true;
    auto map1 = [&](int32_t r) { return ap.adj_reg[(size_t)r]; };
    auto mapn = [&](int32_t r, int len) {
      const int32_t base = ap.adj_reg[(size_t)r];
      for (int k = 1; k < len; ++k)
        if (ap.adj_reg[(size_t)(r + k)] != base + k) ok = false;
      return base;
    };
    const int wlen = out_len(I);
    A.dst = wlen > 1 ? mapn(I.dst, wlen) : map1(I.dst);
    switch (I.code) {
      case Program::MOVR:
      case Program::LOG_RANGE:
      case Program::EXP_RANGE:
      case Program::SOFTMAX:
        A.a = mapn(I.a, I.len);
        break;
      case Program::LSE_RANGE:
        A.a = mapn(I.a, I.len);
        break;
      case Program::DOT:
        A.a = mapn(I.a, I.len);
        A.b = mapn(I.b, I.len);
        break;
      case Program::DENSITY: {
        const int ar = program_density_arity(I.len);
        if (ar > 3) {
          A.a = mapn(I.a, ar);
        } else {
          A.a = map1(I.a);
          A.b = map1(I.b);
          A.c = map1(I.c);
        }
        break;
      }
      case Program::CONST:
      case Program::CONSTR:
        break;  // no operand adjoints
      default:
        A.a = map1(I.a);
        A.b = map1(I.b);
        A.c = map1(I.c);
        break;
    }
    if (!ok) return false;
    ap.code.push_back(A);
  }

  std::reverse(ap.code.begin(), ap.code.end());
  fwd.code = std::move(ncode);
  fwd.n_regs = n_regs;
  p.adj = std::move(ap);
  return true;
}

void run_adjoint(const Program& fwd, const AdjProgram& ap, const double* val,
                 double* adj) {
  for (const AdjInstr& I : ap.code) {
    if (I.code == Program::CALL) {
      // The kernel's own backward is the rule: values from the (possibly
      // checkpointed) forward registers, partials from the scratch the
      // forward stashed inside the file, adjoints accumulated straight
      // into the adjoint file at the same ranges the values occupy --
      // every CALL range is excluded from cell sharing, so range index
      // and cell index agree. Then the output's cells are cleared, the
      // same consume-and-clear every other rule performs.
      const Program::Call& call = fwd.calls[(size_t)I.a];
      KernelCtx ctx;
      ctx.n_in = call.n_in;
      for (int k = 0; k < call.n_in; ++k) {
        ctx.in[k] =
            Desc{const_cast<double*>(val) + call.bwd_in[k], call.in_len[k]};
        ctx.in_adj[k] = Desc{adj + call.in[k], call.in_len[k]};
      }
      ctx.out = Desc{const_cast<double*>(val) + call.bwd_out, call.out_len};
      ctx.out_adj_vec = Desc{adj + call.out, call.out_len};
      if (call.out_len == 1) ctx.out_adj = adj[call.out];
      ctx.variant = call.variant;
      ctx.scratch = const_cast<double*>(val) + call.scratch;
      ctx.idata = call.idata.data();
      ctx.n_idata = (int64_t)call.idata.size();
      const Kernel* k = find_kernel(call.opcode);
      k->backward(ctx);
      for (int j = 0; j < call.out_len; ++j) adj[call.out + j] = 0.0;
      continue;
    }
    // Every instruction consumes its output's adjoint and clears it: the
    // register is a cell, and whatever it held before this instruction wrote
    // it is a different value with a different adjoint. Reading into `t`
    // before clearing is what makes an in-place `d = f(d, b)` come out right.
    // `dst` is always a register here -- the one opcode class where it is an
    // instruction index instead, the jumps, is what gen_adjoint refuses.
    const double t = adj[I.dst];
    switch (I.code) {
      case Program::CONST:
        adj[I.dst] = 0.0;
        break;
      case Program::CONSTR:
        for (int32_t k = 0; k < I.len; ++k) adj[I.dst + k] = 0.0;
        break;
      case Program::MOV:
        adj[I.dst] = 0.0;
        adj[I.a] += t;
        break;
      case Program::MOVR:
        for (int32_t k = 0; k < I.len; ++k) {
          const double u = adj[I.dst + k];
          adj[I.dst + k] = 0.0;
          adj[I.a + k] += u;
        }
        break;
      case Program::ADD:
        adj[I.dst] = 0.0;
        adj[I.a] += t;
        adj[I.b] += t;
        break;
      case Program::SUB:
        adj[I.dst] = 0.0;
        adj[I.a] += t;
        adj[I.b] -= t;
        break;
      case Program::MUL:
        adj[I.dst] = 0.0;
        adj[I.a] += val[I.vb] * t;
        adj[I.b] += val[I.va] * t;
        break;
      case Program::FMA:
        adj[I.dst] = 0.0;
        adj[I.a] += val[I.vb] * t;
        adj[I.b] += val[I.va] * t;
        adj[I.c] += t;
        break;
      case Program::DIV:
        adj[I.dst] = 0.0;
        adj[I.a] += t / val[I.vb];
        adj[I.b] -= t * val[I.va] / (val[I.vb] * val[I.vb]);
        break;
      case Program::POW: {
        adj[I.dst] = 0.0;
        if (val[I.va] == 0.0) break;
        const double m = t * val[I.vd];
        adj[I.a] += m * val[I.vb] / val[I.va];
        adj[I.b] += m * std::log(val[I.va]);
        break;
      }
      // fmax/fmin build no node at all: they return whichever operand won,
      // so the whole adjoint routes to it. Ties go to b. NaN needs saying
      // separately -- `a > b` is false when either is NaN, so the plain
      // comparison would hand fmax(x, NaN) to the NaN, where stan-math
      // returns x. A local declared and never assigned is NaN
      // (mir_prog.hpp), so this is reachable and not hypothetical.
      case Program::FMAX:
      case Program::FMIN: {
        adj[I.dst] = 0.0;
        const double x = val[I.va], y = val[I.vb];
        if (std::isnan(x) && std::isnan(y)) {
          adj[I.a] = std::numeric_limits<double>::quiet_NaN();
          adj[I.b] = std::numeric_limits<double>::quiet_NaN();
        } else if (std::isnan(y)) {
          adj[I.a] += t;
        } else if (std::isnan(x)) {
          adj[I.b] += t;
        } else if (I.code == Program::FMAX ? x > y : x < y) {
          adj[I.a] += t;
        } else {
          adj[I.b] += t;
        }
        break;
      }
      case Program::NEG:
        adj[I.dst] = 0.0;
        adj[I.a] -= t;
        break;
      case Program::EXP:
        adj[I.dst] = 0.0;
        adj[I.a] += t * val[I.vd];
        break;
      case Program::LOG:
        adj[I.dst] = 0.0;
        adj[I.a] += t / val[I.va];
        break;
      case Program::SQRT:
        adj[I.dst] = 0.0;
        if (val[I.vd] != 0.0) adj[I.a] += t / (2.0 * val[I.vd]);
        break;
      case Program::SQUARE:
        adj[I.dst] = 0.0;
        adj[I.a] += t * 2.0 * val[I.va];
        break;
      case Program::INV:
        adj[I.dst] = 0.0;
        adj[I.a] -= t / (val[I.va] * val[I.va]);
        break;
      case Program::FABS:
        adj[I.dst] = 0.0;
        // At exactly zero stan-math returns a fresh node with no operand,
        // so the derivative is dropped rather than being either sign; at
        // NaN it poisons the operand's adjoint outright, which is what
        // makes a sampler reject the draw rather than accept a finite
        // gradient computed from nothing.
        if (std::isnan(val[I.va]))
          adj[I.a] = std::numeric_limits<double>::quiet_NaN();
        else if (val[I.va] > 0.0)
          adj[I.a] += t;
        else if (val[I.va] < 0.0)
          adj[I.a] -= t;
        break;
      case Program::INV_LOGIT:
        adj[I.dst] = 0.0;
        adj[I.a] += t * val[I.vd] * (1.0 - val[I.vd]);
        break;
      case Program::LOG1M:
        adj[I.dst] = 0.0;
        adj[I.a] += t / (val[I.va] - 1.0);
        break;
      case Program::TANH: {
        adj[I.dst] = 0.0;
        const double ch = std::cosh(val[I.va]);
        adj[I.a] += t / (ch * ch);
        break;
      }
      // Comparisons produce a plain 0/1 the forward already computed; they
      // have no derivative, but they did write the register.
      case Program::GT:
      case Program::GE:
      case Program::LT:
      case Program::LE:
      case Program::EQ:
      case Program::NE:
        adj[I.dst] = 0.0;
        break;
      case Program::LOG_RANGE:
        for (int32_t k = 0; k < I.len; ++k) {
          const double u = adj[I.dst + k];
          adj[I.dst + k] = 0.0;
          adj[I.a + k] += u / val[I.va + k];
        }
        break;
      case Program::EXP_RANGE:
        for (int32_t k = 0; k < I.len; ++k) {
          const double u = adj[I.dst + k];
          adj[I.dst + k] = 0.0;
          adj[I.a + k] += u * val[I.vd + k];
        }
        break;
      case Program::DOT:
        adj[I.dst] = 0.0;
        // Ascending, both operands inside one iteration: dot_product's own
        // loop, so a self-dot accumulates in the same order it does.
        for (int32_t k = 0; k < I.len; ++k) {
          adj[I.a + k] += t * val[I.vb + k];
          adj[I.b + k] += t * val[I.va + k];
        }
        break;
      case Program::LSE_RANGE:
        adj[I.dst] = 0.0;
        for (int32_t k = 0; k < I.len; ++k)
          adj[I.a + k] += t * std::exp(val[I.va + k] - val[I.vd]);
        break;
      case Program::SOFTMAX: {
        // adj_i += p_i * (out_adj_i - p . out_adj), the reduction taken once,
        // as rev/fun/softmax.hpp does. The fold is written out rather than
        // handed to Eigen on purpose: stan-math's `res.val().dot(res.adj())`
        // reduces two var EXPRESSIONS, which have no packet access, so Eigen
        // takes the plain ascending path. Mapping our contiguous doubles and
        // calling .dot() would vectorize it and land a few ulp away.
        const double* p = val + I.vd;
        const double* oa = adj + I.dst;
        double d = p[0] * oa[0];
        for (int32_t k = 1; k < I.len; ++k) d += p[k] * oa[k];
        // Clear each output adjoint as its contribution is consumed, not in
        // a second pass: an in-place `x = softmax(x)` has dst and a as the
        // same cells, and a trailing clear would erase what this loop just
        // accumulated. The two stores are disjoint when the ranges are, so
        // this is the same arithmetic in that case.
        for (int32_t k = 0; k < I.len; ++k) {
          const double o = oa[k];
          adj[I.dst + k] = 0.0;
          adj[I.a + k] += p[k] * (o - d);
        }
        break;
      }
      case Program::LSE2:
        adj[I.dst] = 0.0;
        adj[I.a] += t * stan::math::inv_logit(val[I.va] - val[I.vb]);
        adj[I.b] += t * stan::math::inv_logit(val[I.vb] - val[I.va]);
        break;
      case Program::LOG_MIX: {
        adj[I.dst] = 0.0;
        // rev/fun/log_mix.hpp: partials through the helper, with the arms
        // swapped when lambda1 <= lambda2 so the exponential cannot
        // overflow. Transcribed rather than reused because log_mix's
        // partials live in the rev overload, which rvar cannot select.
        double theta_d = val[I.va];
        const double lam1 = val[I.vb], lam2 = val[I.vc];
        double one_m_exp, one_m_t_prod, one_d;
        auto helper = [&](double th, double la, double lb) {
          const double e = std::exp(lb - la);
          one_m_exp = 1.0 - e;
          const double one_m_t = 1.0 - th;
          one_m_t_prod = one_m_t * e;
          one_d = 1.0 / (th + one_m_t_prod);
        };
        if (lam1 > lam2) {
          helper(theta_d, lam1, lam2);
        } else {
          helper(1.0 - theta_d, lam2, lam1);
          one_m_exp = -one_m_exp;
          const double swapped = one_m_t_prod;
          one_m_t_prod = 1.0 - theta_d;
          theta_d = swapped;
        }
        // Descending operand order, as the propagator's per-edge tape
        // entries unwind.
        adj[I.c] += t * (one_m_t_prod * one_d);
        adj[I.b] += t * (theta_d * one_d);
        adj[I.a] += t * (one_m_exp * one_d);
        break;
      }
      case Program::DENSITY: {
        // stan-math computes the partials in doubles through the recorder
        // (program_density.cpp); this only scales and accumulates them.
        // Descending, because the propagator pushes one tape entry per
        // operand in argument order and the reverse sweep runs them
        // backwards -- which shows only when two arguments share a
        // register, and then it is the difference between matching the
        // replay and nearly matching it.
        adj[I.dst] = 0.0;
        const int ar = program_density_arity(I.len);
        double part[kMaxDensityArgs] = {0, 0, 0, 0};
        if (ar > 3) {
          if (program_density_partials(I.len, val + I.va, part))
            for (int k = ar; k-- > 0;) adj[I.a + k] += t * part[k];
          break;
        }
        const double args[3] = {val[I.va], val[I.vb], val[I.vc]};
        if (!program_density_partials(I.len, args, part)) break;
        if (ar > 2) adj[I.c] += t * part[2];
        if (ar > 1) adj[I.b] += t * part[1];
        adj[I.a] += t * part[0];
        break;
      }
      case Program::JZ:
      case Program::JMP:
        break;  // gen_adjoint refuses these; unreachable
    }
  }
}

}  // namespace stanli
