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
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stanli {

bool gen_adjoint(IslandProg& p) {
  Program& fwd = p;
  const std::vector<Program::Instr> orig = fwd.code;
  const int n0 = fwd.n_regs;
  // Reversing flat jumps needs the structured if/else form the instruction
  // stream has already lost. Such programs keep the var replay.
  for (const auto& I : orig) {
    if (program_spec_of(I).has(kProgramNoAdjoint)) return false;
    if (I.code != Program::CALL) continue;
    if (I.a < 0 || (size_t)I.a >= fwd.calls.size()) return false;
    const Program::Call& call = fwd.calls[(size_t)I.a];
    // A forward-only call (including a malformed manually built payload)
    // cannot acquire a generated derivative. Check before any checkpoint or
    // call metadata is written so refusal leaves the program untouched.
    if (call.n_in < 0 || call.n_in > 6 || call.forward == nullptr ||
        call.backward == nullptr)
      return false;
  }

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
    const int wl = program_output_len(orig[i]);
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

  // An input range may coincide with a range output (`x = exp(x)`), but a
  // partial overlap would read a cell the adjoint loop has already cleared.
  auto overlaps = [](int x, int nx, int y, int ny) {
    return nx > 0 && ny > 0 && x < y + ny && y < x + nx;
  };
  // Every register an adjoint rule will READ, checked once here rather than
  // trusted per rule. The write side is bounded by the loop above; nothing
  // bounded the operands, and they index the same vectors.
  auto in_range = [&](int r, int len) { return r >= 0 && r + len <= n0; };
  for (const auto& I : orig) {
    const ProgramOpSpec& spec = program_spec_of(I);
    const int reads = spec.has(kProgramNoInputs) ? 0 : 3;
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
    const int wl = program_output_len(I);
    const bool coincident_range =
        spec.has(kProgramRangeOutput) || I.code == Program::RANGE;
    const int32_t operand[3] = {I.a, I.b, I.c};
    for (int k = 0; k < 3 && !ranged_density; ++k) {
      if (!program_reads(I, k)) continue;
      const int len = program_input_len(I, k);
      if (!in_range(operand[k], len)) return false;
      // A broadcast operand must not overlap a wider output.
      if (len == 1 && wl > 1 && I.code == Program::RANGE &&
          overlaps(I.dst, wl, operand[k], 1))
        return false;
      if (len > 1 && overlaps(I.dst, wl, operand[k], len) &&
          !(coincident_range && I.dst == operand[k]))
        return false;
    }
  }

  // Which registers carry a parameter: seeded from the live-ins the carver
  // marked active, grown forwards. A density argument outside that set gets
  // no partial (program_density.hpp) -- its adjoint cell reaches nothing the
  // executor reads. Read at the density rather than after the sweep, because
  // registers are cells: one holding data here may hold a parameter later.
  std::vector<uint8_t> dmask(orig.size(), 0xf);
  {
    std::vector<char> active((size_t)n0, 0);
    for (const auto& li : p.ins)
      if (li.active)
        for (int k = 0; k < li.len; ++k) active[(size_t)(li.reg + k)] = 1;
    bool any = false;
    auto read = [&](int r, int len) {
      for (int k = 0; k < len && !any; ++k)
        if (active[(size_t)(r + k)]) any = true;
    };
    auto write = [&](int r, int len) {
      for (int k = 0; k < len; ++k) active[(size_t)(r + k)] = 1;
    };
    for (size_t i = 0; i < orig.size(); ++i) {
      const Program::Instr& I = orig[i];
      any = false;
      if (I.code == Program::CALL) {
        const Program::Call& call = fwd.calls[(size_t)I.a];
        for (int j = 0; j < call.n_in; ++j) read(call.in[j], call.in_len[j]);
        if (!any) continue;
        write(call.out, call.out_len);
        write(call.scratch, call.scratch_len);
        continue;
      }
      const ProgramOpSpec& spec = program_spec_of(I);
      if (spec.has(kProgramNoInputs)) continue;
      if (I.code == Program::DENSITY) {
        const int ar = program_density_arity(I.len);
        unsigned m = 0;
        for (int k = 0; k < ar; ++k) {
          const int r =
              ar > 3 ? I.a + k : (k == 0 ? I.a : (k == 1 ? I.b : I.c));
          if (active[(size_t)r]) m |= 1u << k;
        }
        dmask[i] = (uint8_t)m;
        any = m != 0;
      } else {
        read(I.a, program_input_len(I, 0));
        if (spec.has(kProgramReadB)) read(I.b, program_input_len(I, 1));
        if (spec.has(kProgramReadC)) read(I.c, program_input_len(I, 2));
      }
      if (any) write(I.dst, program_output_len(I));
    }
  }
  // STANLI_NO_DENSITY_MASK=1 binds every density argument as a recorder
  // scalar again, which is the comparison the masks have to survive.
  if (std::getenv("STANLI_NO_DENSITY_MASK"))
    std::fill(dmask.begin(), dmask.end(), (uint8_t)0xf);

  std::vector<Program::Instr> ncode;
  ncode.reserve(orig.size());
  std::vector<Program::Call> bound_calls;
  bound_calls.reserve(fwd.calls.size());
  AdjProgram ap;
  ap.code.reserve(orig.size());
  ap.adj_reg.resize((size_t)n0);
  for (int r = 0; r < n0; ++r) ap.adj_reg[(size_t)r] = r;

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

  // Discover every shared cell before emitting the adjoint instructions.
  // Besides making their indices final for map1/mapn below, this lets the
  // file store one double per equivalence class rather than retaining holes
  // at the copied registers' original ids. Representatives are packed in
  // numeric order: if an old mapped range was base+k, every integer in that
  // interval is present, so rank compression preserves base'+k. That is the
  // contiguity contract the ranged rules and CALL backwards rely on.
  for (int i = 0; i < (int)orig.size(); ++i) {
    const Program::Instr& I = orig[(size_t)i];
    if (!aliasable(I, i)) continue;
    const int len = I.code == Program::MOV ? 1 : I.len;
    for (int k = 0; k < len; ++k)
      ap.adj_reg[(size_t)(I.dst + k)] = ap.adj_reg[(size_t)(I.a + k)];
  }
  std::vector<char> used_adj((size_t)n0, 0);
  for (int32_t r : ap.adj_reg) {
    if (r < 0 || r >= n0) return false;
    used_adj[(size_t)r] = 1;
  }
  std::vector<int32_t> compact_adj((size_t)n0, -1);
  for (int r = 0; r < n0; ++r)
    if (used_adj[(size_t)r]) compact_adj[(size_t)r] = ap.n_regs++;
  for (int32_t& r : ap.adj_reg) r = compact_adj[(size_t)r];

  bool mapped_ranges_ok = true;
  auto map1 = [&](int32_t r) { return ap.adj_reg[(size_t)r]; };
  auto mapn = [&](int32_t r, int len) {
    if (len == 0) return int32_t{0};
    const int32_t base = ap.adj_reg[(size_t)r];
    for (int k = 1; k < len; ++k)
      if (ap.adj_reg[(size_t)(r + k)] != base + k) mapped_ranges_ok = false;
    return base;
  };

  int n_regs = n0;
  auto checkpoint = [&](int r, int len, bool needed) {
    if (!needed) return r;
    const int ck = n_regs;
    n_regs += len;
    Program::Instr save;
    save.code = len == 1 ? Program::MOV : Program::MOVR;
    save.dst = ck;
    save.a = r;
    save.len = len;
    ncode.push_back(save);
    return ck;
  };

  // A range needs a checkpoint when any element is overwritten strictly
  // after i; the copy is one MOVR and the backward reads it instead.
  auto save_range = [&](int r, int len, int i) {
    bool need = false;
    for (int k = 0; k < len && !need; ++k)
      need = last_write[(size_t)(r + k)] > i;
    return checkpoint(r, len, need);
  };

  for (int i = 0; i < (int)orig.size(); ++i) {
    const Program::Instr& I = orig[i];
    if (I.code == Program::CALL) {
      Program::Call call = fwd.calls[(size_t)I.a];
      // Metadata describes the registered implementation, not an arbitrary
      // CALL payload.  A private/replaced function therefore keeps the old
      // save-everything behavior even when it borrows a known opcode.
      const Kernel* registered = find_kernel(call.opcode);
      const bool canonical = registered && registered->backward &&
                             call.backward == registered->backward &&
                             registered->primal_reads;
      const BackwardPrimalReads reads =
          canonical ? backward_primal_reads(registered, call.variant)
                    : BackwardPrimalReads{};
      for (int j = 0; j < call.n_in; ++j) {
        call.bwd_adj_in[j] = mapn(call.in[j], call.in_len[j]);
        call.bwd_value_in[j] = reads.input(j)
                                   ? save_range(call.in[j], call.in_len[j], i)
                                   : call.in[j];
      }
      call.bwd_adj_out = mapn(call.out, call.out_len);
      if (!mapped_ranges_ok) return false;
      Program::Instr F = I;
      F.a = static_cast<int32_t>(bound_calls.size());
      ncode.push_back(F);
      call.bwd_value_out =
          reads.output() ? save_range(call.out, call.out_len, i) : call.out;
      AdjInstr A;
      A.code = Program::CALL;
      A.a = static_cast<int32_t>(bound_calls.size());
      bound_calls.push_back(std::move(call));
      ap.code.push_back(A);
      continue;
    }
    if (aliasable(I, i)) {
      ncode.push_back(I);
      continue;  // no adjoint instruction: the cells are already shared
    }
    AdjInstr A;
    A.code = I.code;
    A.mask = dmask[(size_t)i];
    A.dst = I.dst;
    A.a = I.a;
    A.b = I.b;
    A.c = I.c;
    A.len = I.len;
    A.va = I.a;
    A.vb = I.b;
    A.vc = I.c;
    A.vd = I.dst;
    A.sub = I.sub;
    A.bcast = I.bcast;
    if (I.code == Program::RANGE) A.mask = I.law;
    const int wl = program_output_len(I);

    // An operand value is needed as it stood on ENTRY to this instruction,
    // so it must be saved when this instruction overwrites it (`d = d * b`
    // destroys the very value its own derivative reads) or when any later
    // one does.
    auto save_before = [&](int r, int len) {
      bool need = r < I.dst + wl && I.dst < r + len;
      for (int k = 0; k < len && !need; ++k)
        need = last_write[(size_t)(r + k)] > i;
      return checkpoint(r, len, need);
    };
    const ProgramOpSpec& spec = program_spec_of(I);
    if (I.code == Program::DENSITY) {
      const int ar = program_density_arity(I.len);
      if (ar > 3) {
        A.va = save_before(I.a, ar);
      } else {
        A.va = save_before(I.a, 1);
        if (ar > 1) A.vb = save_before(I.b, 1);
        if (ar > 2) A.vc = save_before(I.c, 1);
      }
    } else {
      if (spec.has(kProgramSaveA))
        A.va = save_before(I.a, program_input_len(I, 0));
      if (spec.has(kProgramSaveB))
        A.vb = save_before(I.b, program_input_len(I, 1));
      if (spec.has(kProgramSaveC))
        A.vc = save_before(I.c, program_input_len(I, 2));
    }

    ncode.push_back(I);

    // Nothing is written, and compact_program leaves such an instruction's
    // `dst` in the numbering it had before compaction.
    if (wl == 0) continue;

    // An output value is needed as this instruction LEFT it, so only a
    // later overwrite can lose it.
    if (spec.has(kProgramSaveOut)) A.vd = save_range(I.dst, wl, i);

    // Adjoint operands go through the sharing map; value operands do not.
    // A range has to map to a range: aliasing builds contiguous maps from
    // contiguous copies, so this holds, and refusing is cheaper than
    // scattering the interpreter's loops.
    A.dst = wl > 1 ? mapn(I.dst, wl) : map1(I.dst);
    if (I.code == Program::DENSITY) {
      const int ar = program_density_arity(I.len);
      if (ar > 3) {
        A.a = mapn(I.a, ar);
      } else {
        A.a = map1(I.a);
        A.b = map1(I.b);
        A.c = map1(I.c);
      }
    } else if (!spec.has(kProgramNoInputs)) {
      A.a = mapn(I.a, program_input_len(I, 0));
      A.b = mapn(I.b, program_reads(I, 1) ? program_input_len(I, 1) : 1);
      A.c = mapn(I.c, program_reads(I, 2) ? program_input_len(I, 2) : 1);
    }
    if (!mapped_ranges_ok) return false;
    ap.code.push_back(A);
  }

  std::reverse(ap.code.begin(), ap.code.end());
  fwd.code = std::move(ncode);
  fwd.calls = std::move(bound_calls);
  fwd.n_regs = n_regs;
  p.adj = std::move(ap);
  return true;
}

// Ranged arms below map their operand and output ranges. The validator in
// gen_adjoint admits only disjoint ranges or exactly coincident ones, and the
// coincident case keeps the scalar loop: reading before clearing is what an
// in-place `x = exp(x)` needs.
using AdjA = Eigen::Map<Eigen::ArrayXd>;
using CAdjA = Eigen::Map<const Eigen::ArrayXd>;

// The rules with more to them than one expression, shared by the scalar
// sweep and the ranged one. `t` is the output adjoint, already consumed
// from its cell.
static void pow_rule(uint8_t law, double t, double va, double vb, double vd,
                     double& adj_a, double& adj_b) {
  if (va == 0.0) {
    adj_a += pow_zero_base_partial(law, t, va, vb);
    return;
  }
  const double m = t * vd;
  adj_a += m * vb / va;
  adj_b += m * std::log(va);
}

// fmax/fmin build no node at all: they return whichever operand won,
// so the whole adjoint routes to it. Which operand wins a tie is an
// instantiation property: the var,var overloads compare `a > b`
// (ties to b) where var,double compares `a >= b` (ties to the var),
// and a mixed call whose constant side wins returns a fresh constant
// that carries no adjoint at all. `law` holds the operands' activity
// from lowering (bit 0: a, bit 1: b; 0 is the legacy all-var form),
// matching program_extremum's replay. NaN needs saying separately --
// `a > b` is false when either is NaN, so the plain comparison would
// hand fmax(x, NaN) to the NaN, where stan-math returns x. A local
// declared and never assigned is NaN (mir_prog.hpp), so this is
// reachable and not hypothetical.
static void extremum_rule(bool maximum, uint8_t law, double t, double x,
                          double y, double& adj_a, double& adj_b) {
  const bool a_active = law == 0 || (law & 0x1u) != 0;
  const bool b_active = law == 0 || (law & 0x2u) != 0;
  if (std::isnan(x) && std::isnan(y)) {
    if (a_active) adj_a = std::numeric_limits<double>::quiet_NaN();
    if (b_active) adj_b = std::numeric_limits<double>::quiet_NaN();
  } else if (std::isnan(y)) {
    if (a_active) adj_a += t;
  } else if (std::isnan(x)) {
    if (b_active) adj_b += t;
  } else {
    const bool a_wins = a_active && !b_active ? (maximum ? x >= y : x <= y)
                                              : (maximum ? x > y : x < y);
    if (a_wins) {
      if (a_active) adj_a += t;
    } else if (b_active) {
      adj_b += t;
    }
  }
}

// At exactly zero stan-math returns a fresh node with no operand, so the
// derivative is dropped rather than being either sign; at NaN it poisons
// the operand's adjoint outright, which is what makes a sampler reject the
// draw rather than accept a finite gradient computed from nothing.
static void fabs_rule(double t, double x, double& adj_a) {
  if (std::isnan(x))
    adj_a = std::numeric_limits<double>::quiet_NaN();
  else if (x > 0.0)
    adj_a += t;
  else if (x < 0.0)
    adj_a -= t;
}

static void lse2_rule(double t, double va, double vb, double& adj_a,
                      double& adj_b) {
  adj_a += t * stan::math::inv_logit(va - vb);
  adj_b += t * stan::math::inv_logit(vb - va);
}

// Match rev/fun/log_diff_exp.hpp exactly. Besides being stable when the
// arguments are close, expm1 has observably different rounding from
// spelling either denominator with exp.
static void log_diff_exp_rule(double t, double va, double vb, double& adj_a,
                              double& adj_b) {
  adj_a -= t / stan::math::expm1(vb - va);
  adj_b -= t / stan::math::expm1(va - vb);
}

// rev/fun/log_mix.hpp: partials through the helper, with the arms swapped
// when lambda1 <= lambda2 so the exponential cannot overflow. Transcribed
// rather than reused because log_mix's partials live in the rev overload,
// which rvar cannot select.
static void log_mix_rule(double t, double va, double vb, double vc,
                         double& adj_a, double& adj_b, double& adj_c) {
  double theta_d = va;
  const double lam1 = vb, lam2 = vc;
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
  // Descending operand order, as the propagator's per-edge tape entries
  // unwind.
  adj_c += t * (one_m_t_prod * one_d);
  adj_b += t * (theta_d * one_d);
  adj_a += t * (one_m_exp * one_d);
}

template <int32_t SA, int32_t SB, int32_t SC, typename Body>
static void strided(int32_t n, Body body) {
  for (int32_t k = 0; k < n; ++k) body(k, SA * k, SB * k, SC * k);
}

// Unit and zero strides as compile-time constants, so the loops vectorize.
template <int Arity, typename Body>
static void each(int32_t n, int32_t sa, int32_t sb, int32_t sc, Body body) {
  if constexpr (Arity == 1) {
    if (sa)
      strided<1, 0, 0>(n, body);
    else
      strided<0, 0, 0>(n, body);
  } else if constexpr (Arity == 2) {
    switch (sa * 2 + sb) {
      case 0:
        return strided<0, 0, 0>(n, body);
      case 1:
        return strided<0, 1, 0>(n, body);
      case 2:
        return strided<1, 0, 0>(n, body);
      default:
        return strided<1, 1, 0>(n, body);
    }
  } else {
    switch (sa * 4 + sb * 2 + sc) {
      case 0:
        return strided<0, 0, 0>(n, body);
      case 1:
        return strided<0, 0, 1>(n, body);
      case 2:
        return strided<0, 1, 0>(n, body);
      case 3:
        return strided<0, 1, 1>(n, body);
      case 4:
        return strided<1, 0, 0>(n, body);
      case 5:
        return strided<1, 0, 1>(n, body);
      case 6:
        return strided<1, 1, 0>(n, body);
      default:
        return strided<1, 1, 1>(n, body);
    }
  }
}

// A RANGE instruction: the scalar rule over every element, ascending, the
// order the graph kernels accumulate a broadcast operand's adjoint in. Each
// element consumes its own output cell first, so an in-place range comes
// out as it does for a scalar. Out of line so the scalar sweep's loop stays
// as it was.
__attribute__((noinline)) static void ranged_step(const AdjInstr& I,
                                                  const double* val,
                                                  double* adj) {
  const auto rule = static_cast<Program::Code>(I.sub);
  const ProgramOpSpec& spec = program_code_spec(rule);
  const int32_t sa = (I.bcast & 1u) ? 0 : 1;
  const int32_t sb = spec.has(kProgramReadB) && !(I.bcast & 2u) ? 1 : 0;
  const int32_t sc = spec.has(kProgramReadC) && !(I.bcast & 4u) ? 1 : 0;
  const int32_t n = I.len;
  auto take = [&](int32_t k) {
    const double u = adj[I.dst + k];
    adj[I.dst + k] = 0.0;
    return u;
  };
  const uint8_t law = I.mask;
  auto unary = [&](auto body) { each<1>(n, sa, sb, sc, body); };
  auto binary = [&](auto body) { each<2>(n, sa, sb, sc, body); };
  auto ternary = [&](auto body) { each<3>(n, sa, sb, sc, body); };
  switch (rule) {
    case Program::ADD:
      binary([&](int32_t k, int32_t ka, int32_t kb, int32_t) {
        const double u = take(k);
        adj[I.a + ka] += u;
        adj[I.b + kb] += u;
      });
      break;
    case Program::SUB:
      binary([&](int32_t k, int32_t ka, int32_t kb, int32_t) {
        const double u = take(k);
        adj[I.a + ka] += u;
        adj[I.b + kb] -= u;
      });
      break;
    case Program::MUL:
      binary([&](int32_t k, int32_t ka, int32_t kb, int32_t) {
        const double u = take(k);
        adj[I.a + ka] += val[I.vb + kb] * u;
        adj[I.b + kb] += val[I.va + ka] * u;
      });
      break;
    case Program::DIV:
      binary([&](int32_t k, int32_t ka, int32_t kb, int32_t) {
        const double u = take(k);
        double da, db;
        if (law == kDivSafeGrouping)
          div_partials(u, val[I.vb + kb], val[I.vd + k], &da, &db);
        else
          div_partials_replay(u, val[I.va + ka], val[I.vb + kb], &da, &db);
        adj[I.a + ka] += da;
        adj[I.b + kb] += db;
      });
      break;
    case Program::FMA:
      ternary([&](int32_t k, int32_t ka, int32_t kb, int32_t kc) {
        const double u = take(k);
        adj[I.a + ka] += val[I.vb + kb] * u;
        adj[I.b + kb] += val[I.va + ka] * u;
        adj[I.c + kc] += u;
      });
      break;
    case Program::POW:
      binary([&](int32_t k, int32_t ka, int32_t kb, int32_t) {
        pow_rule(law, take(k), val[I.va + ka], val[I.vb + kb], val[I.vd + k],
                 adj[I.a + ka], adj[I.b + kb]);
      });
      break;
    case Program::FMAX:
    case Program::FMIN:
      binary([&](int32_t k, int32_t ka, int32_t kb, int32_t) {
        extremum_rule(rule == Program::FMAX, law, take(k), val[I.va + ka],
                      val[I.vb + kb], adj[I.a + ka], adj[I.b + kb]);
      });
      break;
    case Program::NEG:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        adj[I.a + ka] -= take(k);
      });
      break;
    case Program::EXP:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        adj[I.a + ka] += take(k) * val[I.vd + k];
      });
      break;
    case Program::LOG:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        adj[I.a + ka] += take(k) / val[I.va + ka];
      });
      break;
    case Program::SQRT:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        const double u = take(k);
        if (val[I.vd + k] != 0.0) adj[I.a + ka] += u / (2.0 * val[I.vd + k]);
      });
      break;
    case Program::SQUARE:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        adj[I.a + ka] += take(k) * 2.0 * val[I.va + ka];
      });
      break;
    case Program::INV:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        adj[I.a + ka] -= take(k) / (val[I.va + ka] * val[I.va + ka]);
      });
      break;
    case Program::FABS:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        fabs_rule(take(k), val[I.va + ka], adj[I.a + ka]);
      });
      break;
    case Program::INV_LOGIT:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        adj[I.a + ka] += take(k) * val[I.vd + k] * (1.0 - val[I.vd + k]);
      });
      break;
    case Program::LOG1M:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        adj[I.a + ka] += take(k) / (val[I.va + ka] - 1.0);
      });
      break;
    case Program::LOG1P_EXP:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        adj[I.a + ka] += take(k) * stan::math::inv_logit(val[I.va + ka]);
      });
      break;
    case Program::TANH:
      unary([&](int32_t k, int32_t ka, int32_t, int32_t) {
        const double u = take(k);
        const double ch = std::cosh(val[I.va + ka]);
        adj[I.a + ka] += u / (ch * ch);
      });
      break;
    case Program::LSE2:
      binary([&](int32_t k, int32_t ka, int32_t kb, int32_t) {
        lse2_rule(take(k), val[I.va + ka], val[I.vb + kb], adj[I.a + ka],
                  adj[I.b + kb]);
      });
      break;
    case Program::LOG_DIFF_EXP:
      binary([&](int32_t k, int32_t ka, int32_t kb, int32_t) {
        log_diff_exp_rule(take(k), val[I.va + ka], val[I.vb + kb],
                          adj[I.a + ka], adj[I.b + kb]);
      });
      break;
    case Program::LOG_MIX:
      ternary([&](int32_t k, int32_t ka, int32_t kb, int32_t kc) {
        log_mix_rule(take(k), val[I.va + ka], val[I.vb + kb], val[I.vc + kc],
                     adj[I.a + ka], adj[I.b + kb], adj[I.c + kc]);
      });
      break;
    default:
      throw std::logic_error("ranged_step: unknown sub-opcode");
  }
}

__attribute__((aligned(64))) void run_adjoint(const Program& fwd,
                                              const AdjProgram& ap,
                                              const double* val, double* adj) {
  // A per-invocation local, not a shared static: island_bwd_native calls
  // back into run_adjoint for an island's own adjoint program, and that
  // program can itself contain a CALL. A static (or thread_local) ctx here
  // aliased the outer and inner frames, so the recursive call clobbered
  // in/in_adj/out fields the outer frame still needed after it returned.
  KernelCtx worker_call_ctx;
  KernelCtx* call_ctx = fwd.calls.empty() ? nullptr : &worker_call_ctx;
  for (const AdjInstr& I : ap.code) {
    if (I.code == Program::CALL) {
      // The kernel's own backward is the rule: values from the (possibly
      // checkpointed) forward registers, partials from the scratch the
      // forward stashed inside the file, adjoints accumulated into the
      // compact ranges named by adj_reg -- every CALL range is excluded
      // from cell sharing, and numeric-order compaction preserves its
      // contiguity. Then the output's cells are cleared, the same
      // consume-and-clear every other rule performs.
      const Program::Call& call = fwd.calls[(size_t)I.a];
      KernelCtx& ctx = *call_ctx;
      ctx.n_in = call.n_in;
      for (int k = 0; k < call.n_in; ++k) {
        ctx.in[k] = Desc{const_cast<double*>(val) + call.bwd_value_in[k],
                         call.in_len[k]};
        ctx.in_adj[k] = (call.input_adjoint_mask & (uint8_t)(1u << k))
                            ? Desc{adj + call.bwd_adj_in[k], call.in_len[k]}
                            : Desc{nullptr, call.in_len[k]};
      }
      ctx.out =
          Desc{const_cast<double*>(val) + call.bwd_value_out, call.out_len};
      ctx.out_adj_vec = Desc{adj + call.bwd_adj_out, call.out_len};
      ctx.out_adj = call.out_len == 1 ? adj[call.bwd_adj_out] : 0.0;
      ctx.variant = call.variant;
      ctx.scratch = const_cast<double*>(val) + call.scratch;
      ctx.idata = call.idata.data();
      ctx.n_idata = (int64_t)call.idata.size();
      ctx.udata = call.udata_owner.get();
      call.backward(ctx);
      for (int j = 0; j < call.out_len; ++j) adj[call.bwd_adj_out + j] = 0.0;
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
        AdjA(adj + I.dst, I.len).setZero();
        break;
      case Program::MOV:
        adj[I.dst] = 0.0;
        adj[I.a] += t;
        break;
      case Program::MOVR:
        if (I.a == I.dst) {
          for (int32_t k = 0; k < I.len; ++k) {
            const double u = adj[I.dst + k];
            adj[I.dst + k] = 0.0;
            adj[I.a + k] += u;
          }
          break;
        }
        AdjA(adj + I.a, I.len) += AdjA(adj + I.dst, I.len);
        AdjA(adj + I.dst, I.len).setZero();
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
      case Program::DIV: {
        adj[I.dst] = 0.0;
        double da, db;
        if (static_cast<uint8_t>(I.len) == kDivSafeGrouping)
          div_partials(t, val[I.vb], val[I.vd], &da, &db);
        else
          div_partials_replay(t, val[I.va], val[I.vb], &da, &db);
        adj[I.a] += da;
        adj[I.b] += db;
        break;
      }
      case Program::POW:
        adj[I.dst] = 0.0;
        pow_rule(static_cast<uint8_t>(I.len), t, val[I.va], val[I.vb],
                 val[I.vd], adj[I.a], adj[I.b]);
        break;
      case Program::FMAX:
      case Program::FMIN:
        adj[I.dst] = 0.0;
        extremum_rule(I.code == Program::FMAX, static_cast<uint8_t>(I.len), t,
                      val[I.va], val[I.vb], adj[I.a], adj[I.b]);
        break;
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
        fabs_rule(t, val[I.va], adj[I.a]);
        break;
      case Program::INV_LOGIT:
        adj[I.dst] = 0.0;
        adj[I.a] += t * val[I.vd] * (1.0 - val[I.vd]);
        break;
      case Program::LOG1M:
        adj[I.dst] = 0.0;
        adj[I.a] += t / (val[I.va] - 1.0);
        break;
      // The derivative stan-math precomputes for its own reverse rule, and
      // the one OP_LOG1P_EXP carries on the graph side: the two paths have
      // to agree to the bit, and one expression is how that stays true.
      case Program::LOG1P_EXP:
        adj[I.dst] = 0.0;
        adj[I.a] += t * stan::math::inv_logit(val[I.va]);
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
        if (I.a == I.dst) {
          for (int32_t k = 0; k < I.len; ++k) {
            const double u = adj[I.dst + k];
            adj[I.dst + k] = 0.0;
            adj[I.a + k] += u / val[I.va + k];
          }
          break;
        }
        AdjA(adj + I.a, I.len) +=
            AdjA(adj + I.dst, I.len) / CAdjA(val + I.va, I.len);
        AdjA(adj + I.dst, I.len).setZero();
        break;
      case Program::EXP_RANGE:
        if (I.a == I.dst) {
          for (int32_t k = 0; k < I.len; ++k) {
            const double u = adj[I.dst + k];
            adj[I.dst + k] = 0.0;
            adj[I.a + k] += u * val[I.vd + k];
          }
          break;
        }
        AdjA(adj + I.a, I.len) +=
            AdjA(adj + I.dst, I.len) * CAdjA(val + I.vd, I.len);
        AdjA(adj + I.dst, I.len).setZero();
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
        lse2_rule(t, val[I.va], val[I.vb], adj[I.a], adj[I.b]);
        break;
      case Program::LOG_DIFF_EXP:
        adj[I.dst] = 0.0;
        log_diff_exp_rule(t, val[I.va], val[I.vb], adj[I.a], adj[I.b]);
        break;
      case Program::LOG_MIX:
        adj[I.dst] = 0.0;
        log_mix_rule(t, val[I.va], val[I.vb], val[I.vc], adj[I.a], adj[I.b],
                     adj[I.c]);
        break;
      case Program::DENSITY: {
        // stan-math computes the partials in doubles through the recorder
        // (program_density.cpp); this only scales and accumulates them.
        // Descending, because the propagator pushes one tape entry per
        // operand in argument order and the reverse sweep runs them
        // backwards -- which shows only when two arguments share a
        // register, and then it is the difference between matching the
        // replay and nearly matching it.
        adj[I.dst] = 0.0;
        if (I.mask == 0) break;
        const int ar = program_density_arity(I.len);
        double part[kMaxDensityArgs] = {0, 0, 0, 0};
        if (ar > 3) {
          if (program_density_partials(I.len, I.mask, val + I.va, part))
            for (int k = ar; k-- > 0;)
              if ((I.mask >> k) & 1u) adj[I.a + k] += t * part[k];
          break;
        }
        const double args[3] = {val[I.va], val[I.vb], val[I.vc]};
        if (!program_density_partials(I.len, I.mask, args, part)) break;
        if (ar > 2 && (I.mask & 4u)) adj[I.c] += t * part[2];
        if (ar > 1 && (I.mask & 2u)) adj[I.b] += t * part[1];
        if (I.mask & 1u) adj[I.a] += t * part[0];
        break;
      }
      case Program::RANGE:
        ranged_step(I, val, adj);
        break;
      case Program::DYN_INDEX:
      case Program::IDIV:
      case Program::EXTREMA_RANGE:
      case Program::JZ:
      case Program::JMP:
      case Program::DIAG_PRE_MULTIPLY:
      case Program::DIAG_POST_MULTIPLY:
      case Program::MDIVIDE_LEFT:
      case Program::MDIVIDE_RIGHT_SPD:
      case Program::TRANSFORM:
      case Program::PRINT:
      case Program::REJECT:
      case Program::DENSITY_VEC:
        break;  // gen_adjoint refuses these; unreachable
      case Program::CALL:
        break;  // handled before this switch
    }
  }
}

}  // namespace stanli
