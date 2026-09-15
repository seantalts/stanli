// Compaction over a compiled register program (program.hpp).
//
// The MIR spells an initialized local as its language-level default fill
// followed by a copy of the initializer, and copies values again through
// return temporaries. Native C++ optimization removes that bookkeeping. A
// register program otherwise pays it on every call, and an island pays for it
// twice: once in the forward instruction stream, once in the register file the
// generated backward (adjoint.cpp) sizes and reads.
//
// A fill whose registers are all overwritten before the next read or
// control-flow edge goes away. A copy whose destination has no other writer
// and whose source is never written again goes away, and later reads of the
// destination are pointed at the source -- the same test gen_adjoint applies
// before letting the two registers share an adjoint cell. What that leaves
// unreferenced is renumbered out of the register file.
//
// Ranges are the constraint throughout: several opcodes read or write runs of
// consecutive registers, and every such run has to stay consecutive. So a copy
// is refused when its block starts or ends strictly inside a range, and the
// renumbering only drops registers nothing names.
#include <stanli/program.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stanli {

namespace {

struct Span {
  int reg;
  int len;
};

template <typename F>
void each_write(const Program& p, const Program::Instr& I, F fn) {
  if (I.code == Program::CALL) {
    const Program::Call& c = p.calls[(size_t)I.a];
    fn(Span{c.out, c.out_len});
    fn(Span{c.scratch, c.scratch_len});
    return;
  }
  if (I.code == Program::TRANSFORM) {
    const Program::Transform& tr = p.transforms[(size_t)I.a];
    fn(Span{tr.out, tr.out_len});
    fn(Span{tr.jac, 1});
    return;
  }
  const int len = program_output_len(I);
  if (len > 0) fn(Span{I.dst, len});
}

template <typename F>
void each_read(const Program& p, const Program::Instr& I, F fn) {
  if (I.code == Program::CALL) {
    const Program::Call& c = p.calls[(size_t)I.a];
    for (int j = 0; j < c.n_in; ++j) fn(Span{c.in[j], c.in_len[j]});
    return;
  }
  if (I.code == Program::TRANSFORM) {
    const Program::Transform& tr = p.transforms[(size_t)I.a];
    for (int k = 0; k < tr.n_in; ++k) fn(Span{tr.in[k], tr.in_len[k]});
    return;
  }
  if (I.code == Program::DENSITY) {
    const int arity = program_density_arity(I.len);
    if (arity > 3) {
      fn(Span{I.a, arity});
      return;
    }
    fn(Span{I.a, 1});
    if (arity > 1) fn(Span{I.b, 1});
    if (arity > 2) fn(Span{I.c, 1});
    return;
  }
  if (I.code == Program::DENSITY_VEC) {
    const Program::VecDensity& v = p.vec_densities[(size_t)I.a];
    for (int k = 0; k < v.arity; ++k)
      fn(Span{v.arg_reg[k], ((v.container_mask >> k) & 1u) ? v.len : 1});
    return;
  }
  if (I.code == Program::PRINT || I.code == Program::REJECT) {
    const Program::Message& message = p.messages[(size_t)I.a];
    for (size_t k = 0; k < message.value_reg.size(); ++k)
      fn(Span{message.value_reg[k], message.value_len[k]});
    return;
  }
  const ProgramOpSpec& spec = program_spec_of(I);
  if (spec.has(kProgramNoInputs)) return;
  fn(Span{I.a, program_input_len(I, 0)});
  if (spec.has(kProgramReadB)) fn(Span{I.b, program_input_len(I, 1)});
  if (spec.has(kProgramReadC)) fn(Span{I.c, program_input_len(I, 2)});
}

void remap(Program::Call& c, const std::vector<int>& m) {
  for (int j = 0; j < c.n_in; ++j)
    if (c.in_len[j] > 0) c.in[j] = m[(size_t)c.in[j]];
  if (c.out_len > 0) c.out = m[(size_t)c.out];
  if (c.scratch_len > 0) c.scratch = m[(size_t)c.scratch];
}

void remap(Program::Transform& tr, const std::vector<int>& m) {
  for (int k = 0; k < tr.n_in; ++k)
    if (tr.in_len[k] > 0) tr.in[k] = m[(size_t)tr.in[k]];
  if (tr.out_len > 0) tr.out = m[(size_t)tr.out];
  tr.jac = m[(size_t)tr.jac];
}

void remap(Program::VecDensity& v, const std::vector<int>& m) {
  for (int k = 0; k < v.arity; ++k) v.arg_reg[k] = m[(size_t)v.arg_reg[k]];
}

void remap(Program::Message& message, const std::vector<int>& m) {
  for (size_t k = 0; k < message.value_reg.size(); ++k)
    if (message.value_len[k] > 0)
      message.value_reg[k] = m[(size_t)message.value_reg[k]];
}

void remap(Program::Instr& I, const std::vector<int>& m) {
  if (I.code == Program::CALL || I.code == Program::TRANSFORM) return;
  const ProgramOpSpec& spec = program_spec_of(I);
  if (program_output_len(I) > 0) I.dst = m[(size_t)I.dst];
  // DENSITY_VEC's `a` is a vec_densities index, not a register; its own
  // registers are remapped separately, by the table-level overload above.
  if (I.code == Program::DENSITY_VEC) return;
  if (spec.has(kProgramNoInputs)) return;
  I.a = m[(size_t)I.a];
  if (I.code == Program::DENSITY) {
    const int arity = program_density_arity(I.len);
    if (arity > 1) I.b = m[(size_t)I.b];
    if (arity > 2) I.c = m[(size_t)I.c];
    return;
  }
  if (spec.has(kProgramReadB)) I.b = m[(size_t)I.b];
  if (spec.has(kProgramReadC)) I.c = m[(size_t)I.c];
}

bool branches(Program::Code code) {
  return code == Program::JZ || code == Program::JMP;
}

bool overlaps(Span lhs, Span rhs) {
  return lhs.reg < rhs.reg + rhs.len && rhs.reg < lhs.reg + lhs.len;
}

bool forwardable_producer(const Program::Instr& I) {
  // Copies already have a dedicated aliasing pass below. CALL payloads carry
  // output and scratch ranges outside Instr. Instructions without a generated
  // adjoint are outside this experiment as well: the important saving is the
  // producer/copy pair in both the forward and generated reverse streams.
  const Program::Code code = I.code;
  if (code == Program::MOV || code == Program::MOVR || code == Program::CALL ||
      code == Program::TRANSFORM)
    return false;
  const ProgramOpSpec& spec = program_spec_of(I);
  return !spec.has(kProgramNoOutput) && !spec.has(kProgramNoAdjoint);
}

// Turn
//
//   producer temporary <- ...
//   MOV[/R] destination <- temporary
//
// into a producer that writes destination directly. This is ordinary,
// model-independent copy coalescing, but it complements the forward aliasing
// pass below: that pass must keep copies into the interior of a ranged value,
// while redirecting the producer preserves the range as a range.
//
// The producer and copy must be adjacent, the temporary must be private to
// the pair, and the destination must not overlap any producer input. The last
// condition keeps a previously out-of-place ranged operation out of place and
// avoids relying on opcode-specific in-place semantics. Branch-bearing
// programs retain the old path for now so erasing the copy cannot change a
// jump target.
bool forward_adjacent_copy_destinations(
    Program& p, const std::vector<std::pair<int, int>>& seeded, bool enabled) {
  if (!enabled || std::getenv("STANLI_NO_PROGRAM_DEST_FORWARD") ||
      p.code.size() < 2)
    return false;
  for (const Program::Instr& I : p.code)
    if (branches(I.code)) return false;

  const size_t n_regs = static_cast<size_t>(p.n_regs);
  std::vector<int> reads(n_regs, 0), writes(n_regs, 0);
  std::vector<size_t> last_write(n_regs, p.code.size());
  for (size_t i = 0; i < p.code.size(); ++i) {
    const Program::Instr& I = p.code[i];
    each_read(p, I, [&](Span span) {
      for (int k = 0; k < span.len; ++k) ++reads[(size_t)(span.reg + k)];
    });
    each_write(p, I, [&](Span span) {
      for (int k = 0; k < span.len; ++k) {
        const size_t reg = (size_t)(span.reg + k);
        ++writes[reg];
        last_write[reg] = i;
      }
    });
  }

  std::vector<char> externally_named(n_regs, 0);
  for (const auto& seed : seeded)
    for (int k = 0; k < seed.second; ++k)
      externally_named[(size_t)(seed.first + k)] = 1;
  for (int reg : p.out_regs) externally_named[(size_t)reg] = 1;

  std::vector<char> remove(p.code.size(), 0);
  for (size_t i = 0; i + 1 < p.code.size(); ++i) {
    Program::Instr& producer = p.code[i];
    const Program::Instr& copy = p.code[i + 1];
    if (!forwardable_producer(producer) ||
        (copy.code != Program::MOV && copy.code != Program::MOVR))
      continue;

    const int output_len = program_output_len(producer);
    const int copy_len = copy.code == Program::MOV ? 1 : copy.len;
    if (output_len <= 0 || copy_len != output_len || copy.a != producer.dst)
      continue;
    const Span temporary{producer.dst, output_len};
    const Span destination{copy.dst, copy_len};
    if (overlaps(temporary, destination)) continue;

    bool safe = true;
    // When the reverse rule needs the producer's output value, it must survive
    // in the destination until reverse. Earlier writes do not matter (the
    // original copy overwrote them), but a later write would require a
    // checkpoint copy and give the instruction straight back. Rules without
    // SaveOut consume the destination adjoint directly and may forward into a
    // repeatedly written state cell.
    const bool saves_output = program_spec_of(producer).has(kProgramSaveOut);
    for (int k = 0; k < output_len && safe; ++k) {
      const size_t src = (size_t)(temporary.reg + k);
      const size_t dst = (size_t)(destination.reg + k);
      safe = reads[src] == 1 && writes[src] == 1 && !externally_named[src] &&
             (last_write[dst] == i + 1 || !saves_output);
    }
    each_read(p, producer, [&](Span input) {
      if (overlaps(destination, input)) safe = false;
    });
    if (!safe) continue;

    producer.dst = copy.dst;
    remove[i + 1] = 1;
    ++i;  // A copy cannot also begin another producer/copy pair.
  }

  size_t kept = 0;
  for (char drop : remove)
    if (!drop) ++kept;
  if (kept == p.code.size()) return false;
  std::vector<Program::Instr> code;
  code.reserve(kept);
  for (size_t i = 0; i < p.code.size(); ++i)
    if (!remove[i]) code.push_back(p.code[i]);
  p.code = std::move(code);
  return true;
}

// inv_logit over a container, as the OP_INV_LOGIT kernel spells stan-math's
// Matrix<var> overload: no sign branch, an inf guard.
template <typename T>
T inv_logit_range(const T& x) {
  if constexpr (std::is_same_v<T, double>) {
    const double e = std::exp(x);
    return std::isinf(e) ? 1.0 : e / (1.0 + e);
  } else {
    const double v = inv_logit_range(stan::math::value_of(x));
    T arg = x;
    return stan::math::make_callback_var(v, [arg, v](auto&& vi) mutable {
      arg.adj() += vi.adj() * v * (1.0 - v);
    });
  }
}

template <int32_t SA, int32_t SB, int32_t SC, typename T, typename Rule>
void strided(const Program::Instr& I, T* reg, Rule rule) {
  for (int32_t k = 0; k < I.len; ++k)
    rule(reg[(size_t)(I.dst + k)], reg[(size_t)(I.a + SA * k)],
         reg[(size_t)(I.b + SB * k)], reg[(size_t)(I.c + SC * k)]);
}

// The rule over the elements. Unit and zero strides are compile-time on the
// double path, so the loops vectorize. The var replay takes one loop,
// descending, so its tape unwinds a broadcast operand's adjoint ascending,
// the order the graph kernels and stan-math's container overloads
// accumulate in.
template <int Arity, typename T, typename Rule>
void each(const Program::Instr& I, T* reg, int32_t sa, int32_t sb, int32_t sc,
          Rule rule) {
  if constexpr (!std::is_same_v<T, double>) {
    for (int32_t k = I.len; k-- > 0;)
      rule(reg[(size_t)(I.dst + k)], reg[(size_t)(I.a + sa * k)],
           reg[(size_t)(I.b + sb * k)], reg[(size_t)(I.c + sc * k)]);
  } else if constexpr (Arity == 1) {
    if (sa)
      strided<1, 0, 0>(I, reg, rule);
    else
      strided<0, 0, 0>(I, reg, rule);
  } else if constexpr (Arity == 2) {
    switch (sa * 2 + sb) {
      case 0:
        return strided<0, 0, 0>(I, reg, rule);
      case 1:
        return strided<0, 1, 0>(I, reg, rule);
      case 2:
        return strided<1, 0, 0>(I, reg, rule);
      default:
        return strided<1, 1, 0>(I, reg, rule);
    }
  } else {
    switch (sa * 4 + sb * 2 + sc) {
      case 0:
        return strided<0, 0, 0>(I, reg, rule);
      case 1:
        return strided<0, 0, 1>(I, reg, rule);
      case 2:
        return strided<0, 1, 0>(I, reg, rule);
      case 3:
        return strided<0, 1, 1>(I, reg, rule);
      case 4:
        return strided<1, 0, 0>(I, reg, rule);
      case 5:
        return strided<1, 0, 1>(I, reg, rule);
      case 6:
        return strided<1, 1, 0>(I, reg, rule);
      default:
        return strided<1, 1, 1>(I, reg, rule);
    }
  }
}

template <typename T>
void run_range(const Program::Instr& I, T* reg) {
  const int32_t sa = program_input_len(I, 0) > 1 ? 1 : 0;
  const int32_t sb = program_reads(I, 1) && program_input_len(I, 1) > 1 ? 1 : 0;
  const int32_t sc = program_reads(I, 2) && program_input_len(I, 2) > 1 ? 1 : 0;
  const uint8_t law = I.law;
  auto unary = [&](auto rule) { each<1>(I, reg, sa, sb, sc, rule); };
  auto binary = [&](auto rule) { each<2>(I, reg, sa, sb, sc, rule); };
  auto ternary = [&](auto rule) { each<3>(I, reg, sa, sb, sc, rule); };
  switch (program_rule(I)) {
    case Program::ADD:
      binary([](T& o, const T& a, const T& b, const T&) { o = a + b; });
      break;
    case Program::SUB:
      binary([](T& o, const T& a, const T& b, const T&) { o = a - b; });
      break;
    case Program::MUL:
      binary([](T& o, const T& a, const T& b, const T&) { o = a * b; });
      break;
    case Program::DIV:
      binary([](T& o, const T& a, const T& b, const T&) { o = a / b; });
      break;
    case Program::POW:
      binary([law](T& o, const T& a, const T& b, const T&) {
        o = program_pow(law, a, b);
      });
      break;
    case Program::FMAX:
      binary([law](T& o, const T& a, const T& b, const T&) {
        o = program_extremum(true, law, a, b);
      });
      break;
    case Program::FMIN:
      binary([law](T& o, const T& a, const T& b, const T&) {
        o = program_extremum(false, law, a, b);
      });
      break;
    case Program::NEG:
      unary([](T& o, const T& a, const T&, const T&) { o = -a; });
      break;
    case Program::EXP:
      unary(
          [](T& o, const T& a, const T&, const T&) { o = stan::math::exp(a); });
      break;
    case Program::LOG:
      unary(
          [](T& o, const T& a, const T&, const T&) { o = stan::math::log(a); });
      break;
    case Program::SQRT:
      unary([](T& o, const T& a, const T&, const T&) {
        o = stan::math::sqrt(a);
      });
      break;
    case Program::SQUARE:
      unary([](T& o, const T& a, const T&, const T&) {
        o = stan::math::square(a);
      });
      break;
    case Program::INV:
      unary(
          [](T& o, const T& a, const T&, const T&) { o = stan::math::inv(a); });
      break;
    case Program::FABS:
      unary([](T& o, const T& a, const T&, const T&) {
        o = stan::math::fabs(a);
      });
      break;
    case Program::INV_LOGIT:
      unary(
          [](T& o, const T& a, const T&, const T&) { o = inv_logit_range(a); });
      break;
    case Program::LOG1M:
      unary([](T& o, const T& a, const T&, const T&) {
        o = stan::math::log1m(a);
      });
      break;
    case Program::LOG1P_EXP:
      unary([](T& o, const T& a, const T&, const T&) {
        o = stan::math::log1p_exp(a);
      });
      break;
    case Program::TANH:
      unary([](T& o, const T& a, const T&, const T&) {
        o = stan::math::tanh(a);
      });
      break;
    case Program::LSE2:
      binary([](T& o, const T& a, const T& b, const T&) {
        o = stan::math::log_sum_exp(a, b);
      });
      break;
    case Program::LOG_DIFF_EXP:
      binary([](T& o, const T& a, const T& b, const T&) {
        o = stan::math::log_diff_exp(a, b);
      });
      break;
    case Program::LOG_MIX:
      ternary([](T& o, const T& a, const T& b, const T& c) {
        o = stan::math::log_mix(a, b, c);
      });
      break;
    case Program::FMA:
      ternary([](T& o, const T& a, const T& b, const T& c) {
        o = stan::math::fma(a, b, c);
      });
      break;
    default:
      throw std::logic_error("run_range: unknown sub-opcode");
  }
}

}  // namespace

void run_elementwise_range(const Program::Instr& I, double* reg) {
  run_range(I, reg);
}

void run_elementwise_range(const Program::Instr& I, stan::math::var* reg) {
  run_range(I, reg);
}

void compact_program(Program& p, std::vector<std::pair<int, int>>& seeded) {
  (void)compact_program_gated(p, seeded, true);
}

bool compact_program_gated(Program& p, std::vector<std::pair<int, int>>& seeded,
                           bool enable_destination_forwarding) {
  const int n_regs = p.n_regs;
  if (n_regs <= 0) return false;
  // DYN_INDEX's `c` is an offset into a run rather than a register.
  for (const auto& I : p.code) {
    if (I.code == Program::DYN_INDEX || I.code == Program::DIAG_PRE_MULTIPLY ||
        I.code == Program::DIAG_POST_MULTIPLY)
      return false;
    if (I.code == Program::CALL && (I.a < 0 || (size_t)I.a >= p.calls.size()))
      return false;
    if (I.code == Program::TRANSFORM &&
        (I.a < 0 || (size_t)I.a >= p.transforms.size()))
      return false;
    if (I.code == Program::DENSITY_VEC &&
        (I.a < 0 || (size_t)I.a >= p.vec_densities.size()))
      return false;
    if ((I.code == Program::PRINT || I.code == Program::REJECT) &&
        (I.a < 0 || (size_t)I.a >= p.messages.size()))
      return false;
  }

  auto in_file = [&](Span s) { return s.reg >= 0 && s.reg + s.len <= n_regs; };
  for (const auto& I : p.code) {
    bool ok = true;
    each_write(p, I, [&](Span s) { ok = ok && in_file(s); });
    each_read(p, I, [&](Span s) { ok = ok && in_file(s); });
    if (!ok) return false;
  }
  for (const auto& s : seeded)
    if (s.second > 0 && !in_file(Span{s.first, s.second})) return false;
  for (int reg : p.out_regs)
    if (reg < 0 || reg >= n_regs) return false;

  const bool destination_forwarded = forward_adjacent_copy_destinations(
      p, seeded, enable_destination_forwarding);
  const size_t n = p.code.size();

  std::vector<char> pinned((size_t)n_regs, 0);
  std::vector<char> interior((size_t)n_regs + 1, 0);
  auto mark_range = [&](Span s) {
    for (int k = 1; k < s.len; ++k) interior[(size_t)(s.reg + k)] = 1;
  };
  auto mark_pinned = [&](Span s) {
    for (int k = 0; k < s.len; ++k) pinned[(size_t)(s.reg + k)] = 1;
  };
  for (const auto& s : seeded) {
    mark_pinned(Span{s.first, s.second});
    mark_range(Span{s.first, s.second});
  }
  for (const auto& I : p.code) {
    each_write(p, I, mark_range);
    each_read(p, I, mark_range);
    // The adjoint rules that walk a run of adjoint cells need those cells to
    // stay where the run is.
    if (I.code == Program::CALL) {
      const Program::Call& c = p.calls[(size_t)I.a];
      for (int j = 0; j < c.n_in; ++j) mark_pinned(Span{c.in[j], c.in_len[j]});
      mark_pinned(Span{c.out, c.out_len});
    }
    if (I.code == Program::DENSITY && program_density_arity(I.len) > 3)
      mark_pinned(Span{I.a, program_density_arity(I.len)});
    if (I.code == Program::DENSITY_VEC) {
      const Program::VecDensity& v = p.vec_densities[(size_t)I.a];
      for (int k = 0; k < v.arity; ++k)
        mark_pinned(
            Span{v.arg_reg[k], ((v.container_mask >> k) & 1u) ? v.len : 1});
    }
  }

  // Sweeping backwards keeps the "next event on this register" tables to two
  // arrays. A fill is dead when every register it writes is overwritten
  // before it is read and before the next branch.
  std::vector<char> dead_fill(n, 0);
  const int never = std::numeric_limits<int>::max();
  std::vector<int> next_read((size_t)n_regs, never),
      next_write((size_t)n_regs, never);
  int next_branch = never;
  for (size_t i = n; i-- > 0;) {
    const Program::Instr& I = p.code[i];
    if (I.code == Program::CONST || I.code == Program::CONSTR) {
      bool dead = true;
      each_write(p, I, [&](Span s) {
        for (int k = 0; k < s.len; ++k) {
          const size_t reg = (size_t)(s.reg + k);
          if (next_write[reg] >= next_read[reg] ||
              next_write[reg] >= next_branch)
            dead = false;
        }
      });
      if (dead) dead_fill[i] = 1;
    }
    each_write(p, I, [&](Span s) {
      for (int k = 0; k < s.len; ++k) next_write[(size_t)(s.reg + k)] = (int)i;
    });
    each_read(p, I, [&](Span s) {
      for (int k = 0; k < s.len; ++k) next_read[(size_t)(s.reg + k)] = (int)i;
    });
    if (branches(I.code)) next_branch = (int)i;
  }

  std::vector<int> declared((size_t)n_regs, -1);
  for (size_t i = 0; i < n; ++i)
    each_write(p, p.code[i], [&](Span s) {
      for (int k = 0; k < s.len; ++k)
        if (declared[(size_t)(s.reg + k)] < 0)
          declared[(size_t)(s.reg + k)] = (int)i;
    });

  // Dropping a fill makes the copy after it the destination's only writer,
  // which is the test gen_adjoint applies too -- so a copy this pass leaves
  // standing has to keep its fill, or the generated backward starts sharing
  // cells the uncompacted one kept apart. Settling the two together takes a
  // couple of rounds; the fill set only ever shrinks.
  std::vector<char> remove(n, 0);
  std::vector<int> alias((size_t)n_regs);
  std::vector<int> first_write((size_t)n_regs), last_write((size_t)n_regs);
  for (int round = 0;; ++round) {
    first_write.assign((size_t)n_regs, -1);
    last_write.assign((size_t)n_regs, -1);
    for (size_t i = 0; i < n; ++i) {
      if (dead_fill[i]) continue;
      each_write(p, p.code[i], [&](Span s) {
        for (int k = 0; k < s.len; ++k) {
          const size_t reg = (size_t)(s.reg + k);
          if (first_write[reg] < 0) first_write[reg] = (int)i;
          last_write[reg] = (int)i;
        }
      });
    }
    for (int reg = 0; reg < n_regs; ++reg) alias[(size_t)reg] = reg;
    remove = dead_fill;
    for (size_t i = 0; i < n; ++i) {
      const Program::Instr& I = p.code[i];
      if (I.code != Program::MOV && I.code != Program::MOVR) continue;
      const int len = I.code == Program::MOV ? 1 : I.len;
      if (len <= 0) continue;
      if (interior[(size_t)I.dst] || interior[(size_t)(I.dst + len)]) continue;
      bool ok = true;
      for (int k = 0; k < len && ok; ++k) {
        const size_t dst = (size_t)(I.dst + k), src = (size_t)(I.a + k);
        ok = first_write[dst] == (int)i && last_write[dst] == (int)i &&
             last_write[src] <= (int)i && !pinned[dst] &&
             alias[src] == alias[(size_t)I.a] + k;
      }
      if (!ok) continue;
      for (int k = 0; k < len; ++k)
        alias[(size_t)(I.dst + k)] = alias[(size_t)I.a] + k;
      remove[i] = 1;
    }
    bool settled = true;
    for (size_t i = 0; i < n; ++i) {
      const Program::Instr& I = p.code[i];
      if (remove[i] || (I.code != Program::MOV && I.code != Program::MOVR))
        continue;
      const int len = I.code == Program::MOV ? 1 : I.len;
      bool shares = len > 0;
      for (int k = 0; k < len && shares; ++k) {
        const size_t dst = (size_t)(I.dst + k), src = (size_t)(I.a + k);
        shares = first_write[dst] == (int)i && last_write[dst] == (int)i &&
                 last_write[src] <= (int)i && !pinned[dst];
      }
      for (int k = 0; k < len && shares; ++k) {
        const int fill = declared[(size_t)(I.dst + k)];
        if (fill < 0 || fill == (int)i || !dead_fill[(size_t)fill]) continue;
        dead_fill[(size_t)fill] = 0;
        settled = false;
      }
    }
    if (settled) break;
    if (round >= 2) dead_fill.assign(n, 0);
  }

  std::vector<char> used((size_t)n_regs, 0);
  auto use = [&](Span s) {
    for (int k = 0; k < s.len; ++k)
      used[(size_t)alias[(size_t)(s.reg + k)]] = 1;
  };
  bool contiguous = true;
  auto check = [&](Span s) {
    for (int k = 1; k < s.len; ++k)
      if (alias[(size_t)(s.reg + k)] != alias[(size_t)s.reg] + k)
        contiguous = false;
    use(s);
  };
  for (const auto& s : seeded) check(Span{s.first, s.second});
  for (size_t i = 0; i < n; ++i) {
    if (remove[i]) continue;
    each_write(p, p.code[i], check);
    each_read(p, p.code[i], check);
  }
  for (int reg : p.out_regs) used[(size_t)alias[(size_t)reg]] = 1;
  if (!contiguous) return destination_forwarded;

  std::vector<int> renumber((size_t)n_regs, -1);
  int at = 0;
  for (int reg = 0; reg < n_regs; ++reg)
    if (used[(size_t)reg]) renumber[(size_t)reg] = at++;
  std::vector<int> map((size_t)n_regs);
  for (int reg = 0; reg < n_regs; ++reg)
    map[(size_t)reg] = renumber[(size_t)alias[(size_t)reg]];

  std::vector<int> new_pc(n + 1, 0);
  int pc = 0;
  for (size_t i = 0; i < n; ++i) {
    new_pc[i] = pc;
    if (!remove[i]) ++pc;
  }
  new_pc[n] = pc;
  std::vector<Program::Instr> code;
  code.reserve((size_t)pc);
  for (size_t i = 0; i < n; ++i) {
    if (remove[i]) continue;
    Program::Instr I = p.code[i];
    if (I.code == Program::CALL) remap(p.calls[(size_t)I.a], map);
    if (I.code == Program::TRANSFORM) remap(p.transforms[(size_t)I.a], map);
    if (I.code == Program::DENSITY_VEC)
      remap(p.vec_densities[(size_t)I.a], map);
    if (I.code == Program::PRINT || I.code == Program::REJECT)
      remap(p.messages[(size_t)I.a], map);
    remap(I, map);
    if (branches(I.code)) I.dst = new_pc[(size_t)I.dst];
    code.push_back(I);
  }
  p.code = std::move(code);
  for (int& reg : p.out_regs) reg = map[(size_t)reg];
  for (auto& s : seeded)
    if (s.second > 0) s.first = map[(size_t)s.first];
  p.n_regs = at;
  return destination_forwarded;
}

}  // namespace stanli
