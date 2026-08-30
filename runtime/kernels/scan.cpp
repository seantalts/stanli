// OP_SCAN: execute and reverse an immutable one-transition loop plan while
// retaining only carry checkpoints and one reusable step register file.
#include <stanli/adjoint.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>
#include <stanli/scan.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stanli {

int64_t choose_scan_checkpoint_block(int64_t count, int64_t carry_cells,
                                     int64_t full_boundary_budget_bytes) {
  if (count <= 1 || carry_cells <= 0) return 1;

  // Test the byte budget with division so even adversarial dimensions cannot
  // overflow `(count + 1) * carry_cells * sizeof(double)`.
  bool full_boundaries_fit = false;
  if (full_boundary_budget_bytes >= 0 &&
      count < std::numeric_limits<int64_t>::max()) {
    const int64_t boundary_count = count + 1;
    const int64_t budget_cells =
        full_boundary_budget_bytes / static_cast<int64_t>(sizeof(double));
    full_boundaries_fit = boundary_count <= budget_cells &&
                          carry_cells <= budget_cells / boundary_count;
  }
  if (full_boundaries_fit) return 1;
  return std::max<int64_t>(1, static_cast<int64_t>(std::sqrt(double(count))));
}

namespace {

constexpr uint64_t kHashSeed = UINT64_C(1469598103934665603);
constexpr uint64_t kHashPrime = UINT64_C(1099511628211);

int64_t checked_add(int64_t a, int64_t b, const char* what) {
  if (a < 0 || b < 0 || a > std::numeric_limits<int64_t>::max() - b)
    throw std::length_error(what);
  return a + b;
}

int64_t checked_mul(int64_t a, int64_t b, const char* what) {
  if (a < 0 || b < 0 || (a != 0 && b > std::numeric_limits<int64_t>::max() / a))
    throw std::length_error(what);
  return a * b;
}

int64_t ceil_div(int64_t n, int64_t d) { return n / d + (n % d != 0); }

uint64_t mix(uint64_t h, uint64_t word) {
  h ^= word;
  h *= kHashPrime;
  return h;
}

uint64_t bits(double x) {
  static_assert(sizeof(double) == sizeof(uint64_t), "double is not 64-bit");
  uint64_t u;
  std::memcpy(&u, &x, sizeof u);
  return u;
}

void store_hash(double* p, uint64_t h) { std::memcpy(p, &h, sizeof h); }

uint64_t load_hash(const double* p) {
  uint64_t h;
  std::memcpy(&h, p, sizeof h);
  return h;
}

// Match lowering's six-input target tree inside the scan instead of letting
// a long recurrence degrade into one left-associated sum.  A level holds at
// most five completed values; the sixth is reduced in source order and
// carried to the next level.  int64_t iteration counts need fewer than 32
// base-six levels.
struct TargetAccumulator {
  static constexpr size_t kLevels = 32;
  std::array<std::array<double, 6>, kLevels> value{};
  std::array<uint8_t, kLevels> size{};

  void add_at(size_t level, double x) {
    if (level == kLevels)
      throw std::length_error("scan target reduction overflow");
    auto& n = size[level];
    value[level][n++] = x;
    if (n != 6) return;
    double reduced = value[level][0];
    for (size_t k = 1; k < 6; ++k) reduced += value[level][k];
    n = 0;
    add_at(level + 1, reduced);
  }

  void add(double x) { add_at(0, x); }

  double finish() const {
    bool have = false;
    double carry = 0.0;
    for (size_t level = 0; level < kLevels; ++level) {
      if (size[level] == 0) continue;
      double reduced = value[level][0];
      for (size_t k = 1; k < size[level]; ++k) reduced += value[level][k];
      // Complete groups precede the partial group carried from the lower
      // level, exactly as reduce_terms appends its final short chunk.
      if (have) reduced += carry;
      carry = reduced;
      have = true;
    }
    return have ? carry : 0.0;
  }
};

struct Layout {
  int64_t values = 0;
  int64_t adjoints = 0;
  int64_t block = 1;
  int64_t blocks = 0;
  int64_t boundaries = 1;
  int64_t temporary_boundaries = 0;
  int64_t temporary_hashes = 0;
  int64_t invariant_cache = 0;
  int64_t invariant_valid = 0;
  int64_t prepared_retention = 0;
  int64_t values_off = 0;
  int64_t adjoints_off = 0;
  int64_t carry_off = 0;
  int64_t carry_adj_off = 0;
  int64_t boundaries_off = 0;
  int64_t block_hashes_off = 0;
  int64_t temporary_off = 0;
  int64_t temporary_hashes_off = 0;
  int64_t invariant_cache_off = 0;
  int64_t invariant_valid_off = 0;
  int64_t prepared_retention_off = 0;
  int64_t total = 0;
};

Layout layout_from_spec(const ScanSpec& s) {
  Layout l;
  for (const auto& tm : s.templates) {
    l.values = std::max(l.values, (int64_t)tm.step.n_regs);
    l.adjoints = std::max(l.adjoints, (int64_t)tm.step.adj.n_regs);
  }
  l.block = s.count == 0 ? 1 : std::min(s.checkpoint_block, s.count);
  l.blocks = s.count == 0 ? 0 : ceil_div(s.count, l.block);
  l.boundaries = checked_add(l.blocks, 1, "scan boundary count overflow");
  l.temporary_boundaries =
      l.block == 1 ? 0 : checked_add(l.block, 1, "scan block overflow");
  l.temporary_hashes = l.block == 1 ? 0 : l.block;
  for (const auto& tm : s.templates)
    l.invariant_cache = checked_add(l.invariant_cache, tm.invariant_cache_cells,
                                    "scan invariant cache overflow");
  l.invariant_valid =
      l.invariant_cache == 0 ? 0 : static_cast<int64_t>(s.templates.size());

  int64_t at = 0;
  l.values_off = at;
  at = checked_add(at, l.values, "scan scratch overflow");
  l.adjoints_off = at;
  at = checked_add(at, l.adjoints, "scan scratch overflow");
  l.carry_off = at;
  at = checked_add(at, s.carry_cells, "scan scratch overflow");
  l.carry_adj_off = at;
  at = checked_add(at, s.carry_cells, "scan scratch overflow");
  l.boundaries_off = at;
  at = checked_add(at,
                   checked_mul(l.boundaries, s.carry_cells,
                               "scan boundary storage overflow"),
                   "scan scratch overflow");
  l.block_hashes_off = at;
  at = checked_add(at, l.blocks, "scan scratch overflow");
  l.temporary_off = at;
  at = checked_add(at,
                   checked_mul(l.temporary_boundaries, s.carry_cells,
                               "scan temporary storage overflow"),
                   "scan scratch overflow");
  l.temporary_hashes_off = at;
  at = checked_add(at, l.temporary_hashes, "scan scratch overflow");
  l.invariant_cache_off = at;
  at = checked_add(at, l.invariant_cache, "scan scratch overflow");
  l.invariant_valid_off = at;
  at = checked_add(at, l.invariant_valid, "scan scratch overflow");
  l.prepared_retention = s.prepared_retention_cells;
  l.prepared_retention_off = at;
  at = checked_add(at, l.prepared_retention, "scan scratch overflow");
  l.total = at;
  return l;
}

struct InvariantAnalysis {
  std::vector<ScanSpec::Template::InvariantCall> calls;
  std::vector<Program::Instr> repeated_code;
  int64_t cache_cells = 0;
};

bool same_instruction(const Program::Instr& a, const Program::Instr& b) {
  return a.code == b.code && a.dst == b.dst && a.a == b.a && a.b == b.b &&
         a.c == b.c && a.len == b.len;
}

bool overlap(int r, int len, int s, int slen) {
  return len > 0 && slen > 0 && r < s + slen && s < r + len;
}

template <typename F>
bool visit_reads(const Program& p, const Program::Instr& I, F&& read) {
  if (I.code == Program::CALL) {
    if (I.a < 0 || static_cast<size_t>(I.a) >= p.calls.size()) return false;
    const Program::Call& call = p.calls[static_cast<size_t>(I.a)];
    for (int k = 0; k < call.n_in; ++k)
      if (!read(call.in[k], call.in_len[k])) return false;
    return true;
  }
  const ProgramOpSpec& op = program_code_spec(I.code);
  if (op.has(kProgramNoInputs)) return true;
  if (I.code == Program::DENSITY) {
    const int arity = program_density_arity(I.len);
    if (arity > 3) return read(I.a, arity);
    if (!read(I.a, 1)) return false;
    if (arity > 1 && !read(I.b, 1)) return false;
    return arity <= 2 || read(I.c, 1);
  }
  const int alen = op.has(kProgramRangeA) ? I.len : 1;
  if (!read(I.a, alen)) return false;
  if (op.has(kProgramReadB) && !read(I.b, op.has(kProgramRangeB) ? I.len : 1))
    return false;
  return !op.has(kProgramReadC) || read(I.c, 1);
}

InvariantAnalysis analyze_invariants(const ScanSpec::Template& tm) {
  InvariantAnalysis result;
  const IslandProg& p = tm.step;
  if (p.n_regs < 0 || p.adj.adj_reg.empty()) return result;
  // repeated_code retains the canonical instruction indices. Removing a
  // CALL from a branched program would require retargeting every jump, and a
  // CALL inside a varying arm is not loop-invariant merely because its data
  // operands are. Decline the whole optimization until a CFG-aware analysis
  // and rewriter can prove both properties.
  for (const Program::Instr& I : p.code)
    if (I.code == Program::JZ || I.code == Program::JMP) return result;
  const size_t nr = static_cast<size_t>(p.n_regs);
  std::vector<uint8_t> varying(nr, 0), forward_read(nr, 0), named(nr, 0);
  std::vector<int64_t> first_read(nr, -1);
  std::vector<int32_t> writes(nr, 0);
  const auto range_ok = [&](int reg, int len) {
    return reg >= 0 && len >= 0 && static_cast<int64_t>(reg) + len <= p.n_regs;
  };
  const auto mark = [&](std::vector<uint8_t>& dst, int reg, int len,
                        uint8_t value) {
    if (!range_ok(reg, len)) return false;
    std::fill(dst.begin() + reg, dst.begin() + reg + len, value);
    return true;
  };
  const auto mark_named = [&](int reg, int len) {
    return reg < 0 || mark(named, reg, len, 1);
  };
  for (const auto& input : tm.inputs) {
    if (!mark(varying, input.step_reg, static_cast<int>(input.len),
              input.iteration_stride == 0 ? 0 : 1) ||
        !mark_named(input.step_reg, static_cast<int>(input.len)))
      return {};
  }
  for (const auto& carry : tm.carry) {
    if (carry.entry_reg >= 0 &&
        (!mark(varying, carry.entry_reg, static_cast<int>(carry.len), 1) ||
         !mark_named(carry.entry_reg, static_cast<int>(carry.len))))
      return {};
    if (!mark_named(carry.exit_reg, static_cast<int>(carry.len))) return {};
  }
  for (const auto& sink : tm.sinks)
    if (!mark_named(sink.step_reg, static_cast<int>(sink.len))) return {};
  if (!mark_named(tm.target_reg, tm.target_reg < 0 ? 0 : 1)) return {};
  for (int reg : p.out_regs)
    if (!mark_named(reg, 1)) return {};

  for (size_t instruction = 0; instruction < p.code.size(); ++instruction) {
    const Program::Instr& I = p.code[instruction];
    if (!visit_reads(p, I, [&](int reg, int len) {
          if (!range_ok(reg, len) || !mark(forward_read, reg, len, 1))
            return false;
          for (int k = 0; k < len; ++k) {
            int64_t& first = first_read[static_cast<size_t>(reg + k)];
            if (first < 0) first = static_cast<int64_t>(instruction);
          }
          return true;
        }))
      return {};
    const auto count_write = [&](int reg, int len) {
      if (!range_ok(reg, len)) return false;
      for (int k = 0; k < len; ++k) ++writes[static_cast<size_t>(reg + k)];
      return true;
    };
    if (I.code == Program::CALL) {
      const Program::Call& call = p.calls[static_cast<size_t>(I.a)];
      if (!count_write(call.out, call.out_len) ||
          !count_write(call.scratch, call.scratch_len))
        return {};
    } else if (!count_write(I.dst, program_output_len(I))) {
      return {};
    }
  }

  std::vector<uint8_t> call_has_adjoint(p.calls.size(), 0);
  for (const AdjInstr& A : p.adj.code) {
    if (A.code != Program::CALL) continue;
    if (A.a < 0 || static_cast<size_t>(A.a) >= p.calls.size()) return {};
    call_has_adjoint[static_cast<size_t>(A.a)] = 1;
  }
  std::vector<uint8_t> hoist_code(p.code.size(), 0);
  for (size_t i = 0; i < p.code.size(); ++i) {
    const Program::Instr& I = p.code[i];
    bool any_varying = false;
    if (!visit_reads(p, I, [&](int reg, int len) {
          if (!range_ok(reg, len)) return false;
          for (int k = 0; k < len; ++k)
            any_varying = any_varying || varying[static_cast<size_t>(reg + k)];
          return true;
        }))
      return {};

    if (I.code == Program::CALL) {
      const Program::Call& call = p.calls[static_cast<size_t>(I.a)];
      bool entry_overlap = false;
      for (const auto& input : tm.inputs)
        entry_overlap =
            entry_overlap || overlap(call.out, call.out_len, input.step_reg,
                                     static_cast<int>(input.len));
      for (const auto& carry : tm.carry)
        if (carry.entry_reg >= 0)
          entry_overlap =
              entry_overlap || overlap(call.out, call.out_len, carry.entry_reg,
                                       static_cast<int>(carry.len));
      bool single_writer = call.out_len > 0 && !entry_overlap;
      for (int k = 0; k < call.out_len && single_writer; ++k)
        single_writer = writes[static_cast<size_t>(call.out + k)] == 1;
      bool written_before_read = true;
      for (int k = 0; k < call.out_len && written_before_read; ++k) {
        const int64_t first = first_read[static_cast<size_t>(call.out + k)];
        // A read by this CALL itself is an input/output alias: CALL reads its
        // inputs before writing its result, so it is not safe to restore the
        // cached result at step entry and remove that CALL.
        written_before_read = first < 0 || first > static_cast<int64_t>(i);
      }
      bool private_scratch = true;
      for (int k = 0; k < call.scratch_len && private_scratch; ++k) {
        const size_t reg = static_cast<size_t>(call.scratch + k);
        private_scratch = forward_read[reg] == 0 && named[reg] == 0;
      }
      const bool candidate = !any_varying && single_writer &&
                             written_before_read && private_scratch &&
                             !call_has_adjoint[static_cast<size_t>(I.a)];
      if (candidate) {
        hoist_code[i] = 1;
        result.calls.push_back(ScanSpec::Template::InvariantCall{
            I.a, call.out, call.out_len, result.cache_cells});
        result.cache_cells = checked_add(result.cache_cells, call.out_len,
                                         "scan invariant cache overflow");
      }
      if (!mark(varying, call.out, call.out_len, any_varying ? 1 : 0) ||
          !mark(varying, call.scratch, call.scratch_len, any_varying ? 1 : 0))
        return {};
      continue;
    }
    if (!mark(varying, I.dst, program_output_len(I), any_varying ? 1 : 0))
      return {};
  }
  if (result.calls.empty()) return result;
  result.repeated_code.reserve(p.code.size() - result.calls.size());
  for (size_t i = 0; i < p.code.size(); ++i)
    if (!hoist_code[i]) result.repeated_code.push_back(p.code[i]);
  return result;
}

int64_t template_cache_offset(const ScanSpec& s, size_t index) {
  int64_t offset = 0;
  for (size_t i = 0; i < index; ++i)
    offset += s.templates[i].invariant_cache_cells;
  return offset;
}

const ScanSpec::Template& template_at(const ScanSpec& s, int64_t iteration) {
  const uint32_t index = s.template_for_iteration.empty()
                             ? 0
                             : s.template_for_iteration[(size_t)iteration];
  return s.templates[(size_t)index];
}

struct PreparedRetentionAnalysis {
  std::vector<std::vector<ScanSpec::Template::PreparedSolveRetention>> plans;
  std::vector<int64_t> iteration_offsets;
  int64_t cells = 0;
  size_t calls = 0;
};

bool program_range(const Program& p, int32_t reg, int64_t len) {
  return reg >= 0 && len >= 0 && static_cast<int64_t>(reg) <= p.n_regs &&
         len <= static_cast<int64_t>(p.n_regs) - reg;
}

// Derive, but do not publish, the exact two-level nesting plan. This is used
// both by the compile-time selector and by validation, so a later program
// rewrite cannot leave scratch offsets silently stale.
bool analyze_prepared_retention(const ScanSpec& s,
                                PreparedRetentionAnalysis* result) {
  if (result == nullptr || s.count < 0 || s.templates.empty()) return false;
  if (s.template_for_iteration.empty()) {
    if (s.templates.size() != 1) return false;
  } else if (static_cast<int64_t>(s.template_for_iteration.size()) != s.count) {
    return false;
  }
  if (static_cast<uint64_t>(s.count) + 1 >
      static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return false;

  PreparedRetentionAnalysis candidate;
  candidate.plans.resize(s.templates.size());
  for (size_t template_index = 0; template_index < s.templates.size();
       ++template_index) {
    const ScanSpec::Template& tm = s.templates[template_index];
    const Program& outer_program = tm.step;
    // The first prototype has no outer execution trace. Scheduled lowering
    // specializes data control into templates, so its intended shape is
    // straight-line here and the parameter branch lives in the nested island.
    // Refuse any residual outer branch rather than capturing stale island
    // trace from the preceding row when an OP_ISLAND CALL is skipped.
    for (const Program::Instr& instruction : outer_program.code)
      if (instruction.code == Program::JZ || instruction.code == Program::JMP)
        return false;
    std::vector<uint8_t> seen_outer(outer_program.calls.size(), 0);
    std::vector<std::pair<const Program*, int32_t>> targets;
    int64_t record_cells = 0;
    for (const Program::Instr& instruction : outer_program.code) {
      if (instruction.code != Program::CALL) continue;
      if (instruction.a < 0 ||
          static_cast<size_t>(instruction.a) >= outer_program.calls.size())
        return false;
      const Program::Call& outer =
          outer_program.calls[static_cast<size_t>(instruction.a)];
      if (outer.opcode != OP_ISLAND) continue;
      // One outer invocation per row is part of the packed-record contract.
      // Reusing one payload twice would need an occurrence id in the hook.
      if (seen_outer[static_cast<size_t>(instruction.a)] != 0) return false;
      seen_outer[static_cast<size_t>(instruction.a)] = 1;
      if (outer.variant != 0 || outer.udata == nullptr ||
          !program_range(outer_program, outer.scratch, outer.scratch_len))
        return false;

      const auto& inner_program = *static_cast<const IslandProg*>(outer.udata);
      if (!inner_program.native_adj || inner_program.adj.trace_bits <= 0 ||
          inner_program.trace_pc.size() != inner_program.code.size())
        continue;
      const int64_t trace_cells =
          (static_cast<int64_t>(inner_program.adj.trace_bits) + 63) / 64;
      if (inner_program.n_regs < 0 || trace_cells < 0 ||
          static_cast<int64_t>(outer.scratch_len) <
              static_cast<int64_t>(inner_program.n_regs) + trace_cells)
        return false;

      std::vector<uint8_t> seen_inner(inner_program.calls.size(), 0);
      for (size_t pc = 0; pc < inner_program.code.size(); ++pc) {
        const Program::Instr& inner_instruction = inner_program.code[pc];
        if (inner_instruction.code != Program::CALL) continue;
        if (inner_instruction.a < 0 ||
            static_cast<size_t>(inner_instruction.a) >=
                inner_program.calls.size())
          return false;
        const Program::Call& inner =
            inner_program.calls[static_cast<size_t>(inner_instruction.a)];
        if (inner.opcode != OP_MDIVIDE_LEFT_PREPARED_PRIM_LU) continue;
        if (seen_inner[static_cast<size_t>(inner_instruction.a)] != 0)
          return false;
        seen_inner[static_cast<size_t>(inner_instruction.a)] = 1;
        if ((inner.variant & 1u) == 0 || inner.n_in != 2 ||
            inner.idata.size() != 2 || inner.idata[0] <= 0 ||
            inner.idata[1] <= 0)
          return false;
        const int64_t n = inner.idata[0], k = inner.idata[1];
        if (n > (std::numeric_limits<int64_t>::max() - n - 1) / n) return false;
        const int64_t expected_scratch = n * n + n + 1;
        if (n > std::numeric_limits<int32_t>::max() / k) return false;
        const int64_t expected_output = (inner.variant & 2u) ? n : n * k;
        if (inner.out_len != expected_output ||
            inner.scratch_len != expected_scratch ||
            !program_range(inner_program, inner.out, inner.out_len) ||
            !program_range(inner_program, inner.bwd_value_out, inner.out_len) ||
            !program_range(inner_program, inner.scratch, inner.scratch_len))
          return false;
        const int32_t trace_bit = inner_program.trace_pc[pc];
        if (trace_bit < 0 || trace_bit >= inner_program.adj.trace_bits)
          return false;
        if (inner.bwd_value_out != inner.out) {
          if (pc + 1 >= inner_program.code.size()) return false;
          const Program::Instr& checkpoint = inner_program.code[pc + 1];
          const Program::Code expected_code =
              inner.out_len == 1 ? Program::MOV : Program::MOVR;
          if (checkpoint.code != expected_code ||
              checkpoint.dst != inner.bwd_value_out ||
              checkpoint.a != inner.out ||
              (inner.out_len != 1 && checkpoint.len != inner.out_len) ||
              inner_program.trace_pc[pc + 1] != -1)
            return false;
        }
        const auto target = std::make_pair(
            static_cast<const Program*>(&inner_program), inner_instruction.a);
        if (std::find(targets.begin(), targets.end(), target) != targets.end())
          return false;
        targets.push_back(target);
        const int64_t width =
            checked_add(1,
                        checked_add(inner.out_len, inner.scratch_len,
                                    "scan retention record overflow"),
                        "scan retention record overflow");
        candidate.plans[template_index].push_back(
            ScanSpec::Template::PreparedSolveRetention{
                instruction.a, inner_instruction.a, trace_bit, record_cells,
                inner.out_len, inner.scratch_len});
        record_cells =
            checked_add(record_cells, width, "scan retention record overflow");
        ++candidate.calls;
      }
    }
  }

  candidate.iteration_offsets.resize(static_cast<size_t>(s.count) + 1, 0);
  for (int64_t t = 0; t < s.count; ++t) {
    const uint32_t index =
        s.template_for_iteration.empty()
            ? 0
            : s.template_for_iteration[static_cast<size_t>(t)];
    if (static_cast<size_t>(index) >= s.templates.size()) return false;
    int64_t width = 0;
    for (const auto& plan : candidate.plans[static_cast<size_t>(index)])
      width =
          checked_add(width,
                      checked_add(1,
                                  checked_add(plan.output_len, plan.scratch_len,
                                              "scan retention size overflow"),
                                  "scan retention size overflow"),
                      "scan retention size overflow");
    candidate.cells =
        checked_add(candidate.cells, width, "scan retention size overflow");
    candidate.iteration_offsets[static_cast<size_t>(t) + 1] = candidate.cells;
  }
  *result = std::move(candidate);
  return true;
}

bool same_retention_plan(const ScanSpec::Template::PreparedSolveRetention& a,
                         const ScanSpec::Template::PreparedSolveRetention& b) {
  return a.outer_call_index == b.outer_call_index &&
         a.inner_call_index == b.inner_call_index &&
         a.inner_trace_bit == b.inner_trace_bit &&
         a.record_offset == b.record_offset && a.output_len == b.output_len &&
         a.scratch_len == b.scratch_len;
}

void require_reg_range(const IslandProg& p, int32_t reg, int64_t len,
                       const char* what) {
  const int64_t original = (int64_t)p.adj.adj_reg.size();
  if (reg < 0 || len < 0 || (int64_t)reg > original || len > original - reg)
    throw std::invalid_argument(what);
}

int64_t last_affine_end(int64_t offset, int64_t stride, int64_t len,
                        int64_t count, const char* what) {
  if (offset < 0 || stride < 0 || len < 0) throw std::invalid_argument(what);
  if (count == 0) return checked_add(offset, len, what);
  const int64_t advance = checked_mul(count - 1, stride, what);
  return checked_add(checked_add(offset, advance, what), len, what);
}

Layout validate_and_layout(const Op& op, const Slot* slots) {
  if (op.udata == nullptr) throw std::invalid_argument("scan has no ScanSpec");
  const auto& s = *static_cast<const ScanSpec*>(op.udata);
  if (s.count < 0 || s.carry_cells < 0 || s.output_cells < 1 ||
      s.checkpoint_block < 1)
    throw std::invalid_argument("scan has negative or zero dimensions");
  if (op.n_in < 0 || op.n_in > 6)
    throw std::invalid_argument("scan has an invalid input count");
  for (int k = 0; k < op.n_in; ++k)
    if (op.in[k] < 0)
      throw std::invalid_argument("scan names a missing graph input");
  if (s.templates.empty())
    throw std::invalid_argument("scan has no step template");
  if (!s.template_for_iteration.empty() &&
      (int64_t)s.template_for_iteration.size() != s.count)
    throw std::invalid_argument("scan schedule length does not match count");
  for (uint32_t index : s.template_for_iteration)
    if ((size_t)index >= s.templates.size())
      throw std::invalid_argument("scan schedule names a missing template");
  if (op.out < 0 || slots[op.out].len != s.output_cells)
    throw std::invalid_argument("scan output slot does not match ScanSpec");

  if (s.template_for_iteration.empty() && s.templates.size() != 1)
    throw std::invalid_argument(
        "scan with multiple templates needs an explicit schedule");
  if ((uint64_t)s.output_cells > (uint64_t)std::numeric_limits<size_t>::max())
    throw std::length_error("scan output is too large to validate");
  std::vector<uint8_t> output_owner((size_t)s.output_cells, 0);
  output_owner.back() = 1;  // target scalar
  auto claim_output = [&](int64_t offset, int64_t len) {
    for (int64_t k = 0; k < len; ++k) {
      uint8_t& owner = output_owner[(size_t)(offset + k)];
      if (owner) throw std::invalid_argument("scan output bindings overlap");
      owner = 1;
    }
  };
  int64_t canonical_carry = -1;
  const auto& schema = s.templates.front().carry;
  for (const auto& tm : s.templates) {
    std::vector<std::pair<int64_t, int64_t>> step_destinations;
    const auto claim_step_destination = [&](int64_t reg, int64_t len) {
      if (len == 0) return;
      const int64_t end = checked_add(reg, len, "scan step binding overflow");
      for (const auto& prior : step_destinations)
        if (reg < prior.second && prior.first < end)
          throw std::invalid_argument("scan step entry bindings overlap");
      step_destinations.push_back({reg, end});
    };
    if (tm.step.n_regs < 0 || tm.step.adj.n_regs < 0)
      throw std::invalid_argument("scan step has negative register storage");
    if (tm.step.adj.adj_reg.empty() &&
        (!tm.inputs.empty() || !tm.carry.empty() || !tm.sinks.empty() ||
         tm.target_reg >= 0))
      throw std::invalid_argument("scan step has no generated adjoint");
    for (const auto& instruction : tm.step.code) {
      if (program_code_spec(instruction.code).has(kProgramNoAdjoint))
        throw std::invalid_argument("scan step is not acyclic/differentiable");
      if (instruction.code != Program::CALL) continue;
      if (instruction.a < 0 || (size_t)instruction.a >= tm.step.calls.size())
        throw std::invalid_argument("scan step CALL payload is out of bounds");
      const Program::Call& call = tm.step.calls[(size_t)instruction.a];
      const uint16_t opcode = call.opcode;
      if (opcode == OP_SCAN || is_effectful_op(opcode))
        throw std::invalid_argument(
            "scan step contains an effectful/nested op");
      if (call.udata != nullptr &&
          std::none_of(tm.udata_pool.begin(), tm.udata_pool.end(),
                       [&](const std::shared_ptr<void>& owner) {
                         return owner.get() == call.udata;
                       }))
        throw std::invalid_argument("scan step CALL payload is not owned");
    }
    int64_t carry_cells = 0;
    if (tm.carry.size() != schema.size())
      throw std::invalid_argument(
          "scan templates have different carry schemas");
    for (size_t k = 0; k < tm.carry.size(); ++k) {
      const auto& c = tm.carry[k];
      const auto& base = schema[k];
      if (c.len != base.len || c.op_input != base.op_input ||
          c.input_offset != base.input_offset ||
          c.output_offset != base.output_offset)
        throw std::invalid_argument(
            "scan templates have different carry schemas");
      if (c.op_input < 0 || c.op_input >= op.n_in || c.input_offset < 0 ||
          c.len < 0 ||
          checked_add(c.input_offset, c.len, "scan carry input overflow") >
              slots[op.in[c.op_input]].len)
        throw std::invalid_argument("scan carry input is out of bounds");
      if (c.entry_reg >= 0) {
        require_reg_range(tm.step, c.entry_reg, c.len,
                          "scan carry entry register is out of bounds");
        claim_step_destination(c.entry_reg, c.len);
      }
      if (c.exit_reg >= 0) {
        require_reg_range(tm.step, c.exit_reg, c.len,
                          "scan carry exit register is out of bounds");
      }
      if (c.output_offset < 0 ||
          checked_add(c.output_offset, c.len, "scan carry output overflow") >
              s.output_cells - 1)
        throw std::invalid_argument("scan carry output is out of bounds");
      carry_cells = checked_add(carry_cells, c.len, "scan carry size overflow");
    }
    if (canonical_carry < 0) canonical_carry = carry_cells;
    if (carry_cells != canonical_carry || carry_cells != s.carry_cells)
      throw std::invalid_argument("scan carry_cells does not match bindings");

    for (const auto& b : tm.inputs) {
      if (b.op_input < 0 || b.op_input >= op.n_in)
        throw std::invalid_argument("scan input binding names a missing input");
      require_reg_range(tm.step, b.step_reg, b.len,
                        "scan input register is out of bounds");
      claim_step_destination(b.step_reg, b.len);
      if (!b.active && b.len > 0 && slots[op.in[b.op_input]].is_param)
        throw std::invalid_argument(
            "scan parameter input binding is marked inactive");
      if (last_affine_end(b.input_offset, b.iteration_stride, b.len, s.count,
                          "scan input binding overflow") >
          slots[op.in[b.op_input]].len)
        throw std::invalid_argument("scan input binding is out of bounds");
    }
    for (const auto& b : tm.sinks) {
      require_reg_range(tm.step, b.step_reg, b.len,
                        "scan sink register is out of bounds");
      if (s.count > 1 && b.iteration_stride < b.len)
        throw std::invalid_argument("scan sink rows overlap");
      if (last_affine_end(b.output_offset, b.iteration_stride, b.len, s.count,
                          "scan sink binding overflow") > s.output_cells - 1)
        throw std::invalid_argument("scan sink binding is out of bounds");
    }
    if (tm.target_reg >= 0)
      require_reg_range(tm.step, tm.target_reg, 1,
                        "scan target register is out of bounds");

    const bool has_invariant_plan = tm.invariant_cache_cells != 0 ||
                                    !tm.invariant_calls.empty() ||
                                    !tm.repeated_code.empty();
    if (has_invariant_plan) {
      const InvariantAnalysis expected = analyze_invariants(tm);
      if (expected.cache_cells != tm.invariant_cache_cells ||
          expected.calls.size() != tm.invariant_calls.size() ||
          expected.repeated_code.size() != tm.repeated_code.size())
        throw std::invalid_argument("scan invariant CALL plan is stale");
      for (size_t k = 0; k < expected.calls.size(); ++k) {
        const auto& a = expected.calls[k];
        const auto& b = tm.invariant_calls[k];
        if (a.call_index != b.call_index || a.step_reg != b.step_reg ||
            a.len != b.len || a.cache_offset != b.cache_offset)
          throw std::invalid_argument("scan invariant CALL plan is stale");
      }
      for (size_t k = 0; k < expected.repeated_code.size(); ++k)
        if (!same_instruction(expected.repeated_code[k], tm.repeated_code[k]))
          throw std::invalid_argument("scan invariant CALL code is stale");
    }
  }

  for (const auto& c : schema) claim_output(c.output_offset, c.len);
  for (int64_t t = 0; t < s.count; ++t)
    for (const auto& b : template_at(s, t).sinks)
      claim_output(b.output_offset + t * b.iteration_stride, b.len);
  if (std::find(output_owner.begin(), output_owner.end(), uint8_t{0}) !=
      output_owner.end())
    throw std::invalid_argument("scan output has an unseeded partial sink");

  const bool has_retention_plan =
      s.prepared_retention_cells != 0 ||
      !s.prepared_retention_iteration_offsets.empty() ||
      std::any_of(s.templates.begin(), s.templates.end(), [](const auto& tm) {
        return tm.prepared_retention_record_cells != 0 ||
               !tm.prepared_solve_retention.empty();
      });
  if (has_retention_plan) {
    PreparedRetentionAnalysis expected;
    if (!analyze_prepared_retention(s, &expected) || expected.calls == 0 ||
        expected.cells != s.prepared_retention_cells ||
        expected.iteration_offsets != s.prepared_retention_iteration_offsets ||
        expected.plans.size() != s.templates.size())
      throw std::invalid_argument("scan prepared retention plan is stale");
    for (size_t i = 0; i < s.templates.size(); ++i) {
      const auto& tm = s.templates[i];
      const auto& plans = expected.plans[i];
      int64_t record_cells = 0;
      for (const auto& plan : plans)
        record_cells = checked_add(
            record_cells,
            checked_add(1,
                        checked_add(plan.output_len, plan.scratch_len,
                                    "scan retention record overflow"),
                        "scan retention record overflow"),
            "scan retention record overflow");
      if (record_cells != tm.prepared_retention_record_cells ||
          plans.size() != tm.prepared_solve_retention.size())
        throw std::invalid_argument("scan prepared retention plan is stale");
      for (size_t k = 0; k < plans.size(); ++k)
        if (!same_retention_plan(plans[k], tm.prepared_solve_retention[k]))
          throw std::invalid_argument("scan prepared retention plan is stale");
    }
  }

  return layout_from_spec(s);
}

struct StepProfile {
  double bind_ns = 0.0;
  double program_ns = 0.0;
  double exit_ns = 0.0;
};

uint64_t run_step(const ScanSpec::Template& tm, const KernelCtx& ctx,
                  int64_t iteration, const double* carry_in, double* carry_out,
                  double* values, double* invariant_cache,
                  double* invariant_valid, StepProfile* profile = nullptr,
                  const ProgramCallHook* call_hook = nullptr) {
  using Clock = std::chrono::steady_clock;
  using Nanoseconds = std::chrono::duration<double, std::nano>;
  const auto bind_start = profile ? Clock::now() : Clock::time_point{};
  for (const auto& b : tm.inputs) {
    const int64_t off = b.input_offset + iteration * b.iteration_stride;
    std::memcpy(values + b.step_reg, ctx.in[b.op_input].data + off,
                (size_t)b.len * sizeof(double));
  }
  int64_t carry_at = 0;
  for (const auto& c : tm.carry) {
    if (c.entry_reg >= 0)
      std::memcpy(values + c.entry_reg, carry_in + carry_at,
                  (size_t)c.len * sizeof(double));
    carry_at += c.len;
  }
  if (profile)
    profile->bind_ns += Nanoseconds(Clock::now() - bind_start).count();

  const auto program_start = profile ? Clock::now() : Clock::time_point{};
  const auto run_full_program = [&] {
    if (call_hook == nullptr)
      run_program(tm.step, values);
    else
      run_program(tm.step, values, call_hook);
  };
  if (tm.invariant_calls.empty()) {
    run_full_program();
  } else if (*invariant_valid == 0.0) {
    run_full_program();
    for (const auto& call : tm.invariant_calls)
      std::memcpy(invariant_cache + call.cache_offset, values + call.step_reg,
                  static_cast<size_t>(call.len) * sizeof(double));
    *invariant_valid = 1.0;
  } else {
    for (const auto& call : tm.invariant_calls)
      std::memcpy(values + call.step_reg, invariant_cache + call.cache_offset,
                  static_cast<size_t>(call.len) * sizeof(double));
    if (call_hook == nullptr)
      run_program(tm.step, tm.repeated_code, values);
    else
      run_program(tm.step, tm.repeated_code, values, nullptr, nullptr,
                  call_hook);
  }
  if (profile)
    profile->program_ns += Nanoseconds(Clock::now() - program_start).count();

  const auto exit_start = profile ? Clock::now() : Clock::time_point{};
  uint64_t row_hash = kHashSeed;
  if (tm.target_reg >= 0) row_hash = mix(row_hash, bits(values[tm.target_reg]));
  carry_at = 0;
  for (const auto& c : tm.carry) {
    if (c.exit_reg >= 0) {
      std::memcpy(carry_out + carry_at, values + c.exit_reg,
                  (size_t)c.len * sizeof(double));
    } else if (carry_out + carry_at != carry_in + carry_at) {
      std::memcpy(carry_out + carry_at, carry_in + carry_at,
                  (size_t)c.len * sizeof(double));
    }
    for (int64_t k = 0; k < c.len; ++k)
      row_hash = mix(row_hash, bits(carry_out[carry_at + k]));
    carry_at += c.len;
  }
  for (const auto& b : tm.sinks)
    for (int64_t k = 0; k < b.len; ++k)
      row_hash = mix(row_hash, bits(values[b.step_reg + k]));
  if (profile)
    profile->exit_ns += Nanoseconds(Clock::now() - exit_start).count();
  return row_hash;
}

void capture_prepared_retention(const ScanSpec::Template& tm,
                                const double* values, double* record) {
  for (const auto& plan : tm.prepared_solve_retention) {
    double* const saved = record + plan.record_offset;
    saved[0] = 0.0;
    const Program::Call& outer =
        tm.step.calls[static_cast<size_t>(plan.outer_call_index)];
    const auto& inner_program = *static_cast<const IslandProg*>(outer.udata);
    const uint8_t* const trace = reinterpret_cast<const uint8_t*>(
        values + outer.scratch + inner_program.n_regs);
    if ((trace[static_cast<size_t>(plan.inner_trace_bit) >> 3] &
         static_cast<uint8_t>(1u << (plan.inner_trace_bit & 7))) == 0)
      continue;
    const Program::Call& inner =
        inner_program.calls[static_cast<size_t>(plan.inner_call_index)];
    std::memcpy(saved + 1, values + outer.scratch + inner.bwd_value_out,
                static_cast<size_t>(plan.output_len) * sizeof(double));
    std::memcpy(saved + 1 + plan.output_len,
                values + outer.scratch + inner.scratch,
                static_cast<size_t>(plan.scratch_len) * sizeof(double));
    saved[0] = 1.0;
  }
}

struct PreparedRetentionReplay {
  const ScanSpec::Template* tm = nullptr;
  const double* record = nullptr;
};

bool restore_prepared_retention(void* opaque, const Program& program,
                                int32_t call_index, double* reg) {
  const auto& replay = *static_cast<const PreparedRetentionReplay*>(opaque);
  for (const auto& plan : replay.tm->prepared_solve_retention) {
    const Program::Call& outer =
        replay.tm->step.calls[static_cast<size_t>(plan.outer_call_index)];
    const auto* const inner_program =
        static_cast<const IslandProg*>(outer.udata);
    if (static_cast<const Program*>(inner_program) != &program ||
        plan.inner_call_index != call_index)
      continue;
    const double* const saved = replay.record + plan.record_offset;
    // Invalid means the original forward did not execute this branch. If a
    // replay reaches it after all, run the ordinary kernel and let the scan's
    // carry/hash parity checks diagnose the changed control flow.
    if (saved[0] != 1.0) return false;
    const Program::Call& call = program.calls[static_cast<size_t>(call_index)];
    std::memcpy(reg + call.out, saved + 1,
                static_cast<size_t>(plan.output_len) * sizeof(double));
    std::memcpy(reg + call.scratch, saved + 1 + plan.output_len,
                static_cast<size_t>(plan.scratch_len) * sizeof(double));
    return true;
  }
  return false;
}

void seed_initial_carry(const ScanSpec& s, const KernelCtx& ctx,
                        double* carry) {
  int64_t at = 0;
  for (const auto& c : s.templates.front().carry) {
    std::memcpy(carry + at, ctx.in[c.op_input].data + c.input_offset,
                (size_t)c.len * sizeof(double));
    at += c.len;
  }
}

void scan_fwd(KernelCtx& ctx) {
  const auto& s = *static_cast<const ScanSpec*>(ctx.udata);
  const Layout l = layout_from_spec(s);
  double* const values = ctx.scratch + l.values_off;
  double* const carry = ctx.scratch + l.carry_off;
  double* const boundaries = ctx.scratch + l.boundaries_off;
  double* const block_hashes = ctx.scratch + l.block_hashes_off;
  double* const invariant_cache = ctx.scratch + l.invariant_cache_off;
  double* const invariant_valid = ctx.scratch + l.invariant_valid_off;
  double* const prepared_retention = ctx.scratch + l.prepared_retention_off;

  std::fill(ctx.out.data, ctx.out.data + ctx.out.len, 0.0);
  std::fill(invariant_valid, invariant_valid + l.invariant_valid, 0.0);
  seed_initial_carry(s, ctx, carry);
  if (s.carry_cells > 0)
    std::memcpy(boundaries, carry, (size_t)s.carry_cells * sizeof(double));

  TargetAccumulator target;
  int64_t boundary = 1;
  uint64_t block_hash = kHashSeed;
  for (int64_t t = 0; t < s.count; ++t) {
    const auto& tm = template_at(s, t);
    const size_t template_index = static_cast<size_t>(&tm - s.templates.data());
    double* const tm_cache =
        tm.invariant_calls.empty()
            ? nullptr
            : invariant_cache + template_cache_offset(s, template_index);
    double* const tm_valid =
        tm.invariant_calls.empty() ? nullptr : invariant_valid + template_index;
    uint64_t row_hash = 0;
    try {
      row_hash = run_step(tm, ctx, t, carry, carry, values, tm_cache, tm_valid);
    } catch (const std::exception& error) {
      if (std::getenv("STANLI_DEBUG_SCAN"))
        std::fprintf(stderr,
                     "scan forward failed iteration=%lld template=%zu: %s\n",
                     (long long)t, template_index, error.what());
      throw;
    }
    if (!values_only() && !tm.prepared_solve_retention.empty()) {
      const int64_t record_offset =
          s.prepared_retention_iteration_offsets[static_cast<size_t>(t)];
      capture_prepared_retention(tm, values,
                                 prepared_retention + record_offset);
    }
    block_hash = mix(block_hash, row_hash);
    for (const auto& b : tm.sinks) {
      const int64_t out = b.output_offset + t * b.iteration_stride;
      std::memcpy(ctx.out.data + out, values + b.step_reg,
                  (size_t)b.len * sizeof(double));
    }
    if (tm.target_reg >= 0) target.add(values[tm.target_reg]);

    if ((t + 1) % l.block == 0 || t + 1 == s.count) {
      if (s.carry_cells > 0)
        std::memcpy(boundaries + boundary * s.carry_cells, carry,
                    (size_t)s.carry_cells * sizeof(double));
      store_hash(block_hashes + boundary - 1, block_hash);
      ++boundary;
      block_hash = kHashSeed;
    }
  }

  int64_t carry_at = 0;
  for (const auto& c : s.templates.front().carry) {
    std::memcpy(ctx.out.data + c.output_offset, carry + carry_at,
                (size_t)c.len * sizeof(double));
    carry_at += c.len;
  }
  ctx.out.data[s.output_cells - 1] = target.finish();
}

void require_same(uint64_t got, uint64_t want, const char* what) {
  if (got != want) throw std::runtime_error(what);
}

void require_same_carry(const double* got, const double* want, int64_t len) {
  if (len > 0 && std::memcmp(got, want, (size_t)len * sizeof(double)) != 0)
    throw std::runtime_error("scan carry diverged during replay");
}

void scan_bwd(KernelCtx& ctx) {
  const auto& s = *static_cast<const ScanSpec*>(ctx.udata);
  const Layout l = layout_from_spec(s);
  double* const values = ctx.scratch + l.values_off;
  double* const adj = ctx.scratch + l.adjoints_off;
  double* const carry = ctx.scratch + l.carry_off;
  double* const carry_adj = ctx.scratch + l.carry_adj_off;
  const double* const boundaries = ctx.scratch + l.boundaries_off;
  const double* const block_hashes = ctx.scratch + l.block_hashes_off;
  double* const temporary = ctx.scratch + l.temporary_off;
  double* const temporary_hashes = ctx.scratch + l.temporary_hashes_off;
  double* const invariant_cache = ctx.scratch + l.invariant_cache_off;
  double* const invariant_valid = ctx.scratch + l.invariant_valid_off;
  const double* const prepared_retention =
      ctx.scratch + l.prepared_retention_off;

  std::fill(carry_adj, carry_adj + s.carry_cells, 0.0);
  // A generated adjoint consumes and clears every instruction output.  Once
  // a row has been harvested, only its live-in and explicitly seeded cells
  // can remain.  Clear those narrow ranges below instead of streaming across
  // the template's entire (often much larger) compact adjoint file for every
  // row.  The one full clear here establishes the invariant for row one.
  std::fill(adj, adj + l.adjoints, 0.0);
  int64_t carry_at = 0;
  for (size_t carry_index = 0; carry_index < s.templates.front().carry.size();
       ++carry_index) {
    const auto& c = s.templates.front().carry[carry_index];
    const bool any_active = std::any_of(s.templates.begin(), s.templates.end(),
                                        [&](const ScanSpec::Template& tm) {
                                          return tm.carry[carry_index].active;
                                        });
    if (any_active)
      for (int64_t k = 0; k < c.len; ++k)
        carry_adj[carry_at + k] = ctx.out_adj_vec.data[c.output_offset + k];
    carry_at += c.len;
  }
  const double target_adj = ctx.out_adj_vec.data[s.output_cells - 1];

  // Optional phase accounting for large production scans. Keep the clock
  // entirely out of the ordinary path; this is deliberately a kernel-local
  // diagnostic rather than part of the global opcode profiler.
  const char* const profile_env = std::getenv("STANLI_PROFILE_SCAN");
  const bool profile = profile_env != nullptr && profile_env[0] != '0';
  const bool sparse_adj_reset =
      std::getenv("STANLI_NO_SCAN_SPARSE_ADJ_RESET") == nullptr;
  using Clock = std::chrono::steady_clock;
  using Nanoseconds = std::chrono::duration<double, std::nano>;
  double block_replay_ns = 0.0;
  double row_replay_ns = 0.0;
  double zero_ns = 0.0;
  double seed_ns = 0.0;
  double adjoint_ns = 0.0;
  double harvest_ns = 0.0;
  StepProfile row_step_profile;

  for (int64_t block = l.blocks; block-- > 0;) {
    const int64_t begin = block * l.block;
    const int64_t end = std::min(s.count, begin + l.block);
    const int64_t rows = end - begin;

    if (l.block > 1) {
      const auto phase_start = profile ? Clock::now() : Clock::time_point{};
      if (s.carry_cells > 0)
        std::memcpy(temporary, boundaries + block * s.carry_cells,
                    (size_t)s.carry_cells * sizeof(double));
      uint64_t replay_hash = kHashSeed;
      for (int64_t r = 0; r < rows; ++r) {
        const int64_t t = begin + r;
        const auto& tm = template_at(s, t);
        const size_t template_index =
            static_cast<size_t>(&tm - s.templates.data());
        double* const tm_cache =
            tm.invariant_calls.empty()
                ? nullptr
                : invariant_cache + template_cache_offset(s, template_index);
        double* const tm_valid = tm.invariant_calls.empty()
                                     ? nullptr
                                     : invariant_valid + template_index;
        const double* const retained_record =
            tm.prepared_solve_retention.empty()
                ? nullptr
                : prepared_retention + s.prepared_retention_iteration_offsets
                                           [static_cast<size_t>(t)];
        PreparedRetentionReplay retained{&tm, retained_record};
        ProgramCallHook retention_hook{&retained, restore_prepared_retention};
        const ProgramCallHook* const hook =
            tm.prepared_solve_retention.empty() ? nullptr : &retention_hook;
        const uint64_t row_hash =
            run_step(tm, ctx, t, temporary + r * s.carry_cells,
                     temporary + (r + 1) * s.carry_cells, values, tm_cache,
                     tm_valid, nullptr, hook);
        store_hash(temporary_hashes + r, row_hash);
        replay_hash = mix(replay_hash, row_hash);
      }
      require_same(replay_hash, load_hash(block_hashes + block),
                   "scan observable values diverged during block replay");
      require_same_carry(temporary + rows * s.carry_cells,
                         boundaries + (block + 1) * s.carry_cells,
                         s.carry_cells);
      if (profile)
        block_replay_ns += Nanoseconds(Clock::now() - phase_start).count();
    }

    for (int64_t r = rows; r-- > 0;) {
      const int64_t t = begin + r;
      const auto& tm = template_at(s, t);
      const size_t template_index =
          static_cast<size_t>(&tm - s.templates.data());
      double* const tm_cache =
          tm.invariant_calls.empty()
              ? nullptr
              : invariant_cache + template_cache_offset(s, template_index);
      double* const tm_valid = tm.invariant_calls.empty()
                                   ? nullptr
                                   : invariant_valid + template_index;
      const double* const entry = l.block == 1 ? boundaries + t * s.carry_cells
                                               : temporary + r * s.carry_cells;
      const double* const expected_exit =
          l.block == 1 ? boundaries + (t + 1) * s.carry_cells
                       : temporary + (r + 1) * s.carry_cells;
      const auto replay_start = profile ? Clock::now() : Clock::time_point{};
      const double* const retained_record =
          tm.prepared_solve_retention.empty()
              ? nullptr
              : prepared_retention +
                    s.prepared_retention_iteration_offsets[static_cast<size_t>(
                        t)];
      PreparedRetentionReplay retained{&tm, retained_record};
      ProgramCallHook retention_hook{&retained, restore_prepared_retention};
      const ProgramCallHook* const hook =
          tm.prepared_solve_retention.empty() ? nullptr : &retention_hook;
      const uint64_t row_hash =
          run_step(tm, ctx, t, entry, carry, values, tm_cache, tm_valid,
                   profile ? &row_step_profile : nullptr, hook);
      require_same_carry(carry, expected_exit, s.carry_cells);
      if (l.block == 1)
        require_same(mix(kHashSeed, row_hash), load_hash(block_hashes + block),
                     "scan observable values diverged during row replay");
      else
        require_same(row_hash, load_hash(temporary_hashes + r),
                     "scan observable values diverged during row replay");
      if (profile)
        row_replay_ns += Nanoseconds(Clock::now() - replay_start).count();

      const auto full_zero_start = profile ? Clock::now() : Clock::time_point{};
      if (!sparse_adj_reset) {
        std::fill(adj, adj + tm.step.adj.n_regs, 0.0);
        if (profile)
          zero_ns += Nanoseconds(Clock::now() - full_zero_start).count();
      }
      const auto seed_start = profile ? Clock::now() : Clock::time_point{};
      const auto& map = tm.step.adj.adj_reg;
      carry_at = 0;
      for (const auto& c : tm.carry) {
        if (c.active && c.exit_reg >= 0)
          for (int64_t k = 0; k < c.len; ++k)
            adj[(size_t)map[(size_t)(c.exit_reg + k)]] +=
                carry_adj[carry_at + k];
        carry_at += c.len;
      }
      for (const auto& b : tm.sinks) {
        const int64_t out = b.output_offset + t * b.iteration_stride;
        for (int64_t k = 0; k < b.len; ++k)
          adj[(size_t)map[(size_t)(b.step_reg + k)]] +=
              ctx.out_adj_vec.data[out + k];
      }
      if (tm.target_reg >= 0)
        adj[(size_t)map[(size_t)tm.target_reg]] += target_adj;
      if (profile) seed_ns += Nanoseconds(Clock::now() - seed_start).count();

      const auto adjoint_start = profile ? Clock::now() : Clock::time_point{};
      run_adjoint(tm.step, tm.step.adj, values, adj);
      if (profile)
        adjoint_ns += Nanoseconds(Clock::now() - adjoint_start).count();

      const auto harvest_start = profile ? Clock::now() : Clock::time_point{};
      for (const auto& b : tm.inputs) {
        if (!b.active || ctx.in_adj[b.op_input].data == nullptr) continue;
        const int64_t off = b.input_offset + t * b.iteration_stride;
        for (int64_t k = 0; k < b.len; ++k)
          ctx.in_adj[b.op_input].data[off + k] +=
              adj[(size_t)map[(size_t)(b.step_reg + k)]];
      }
      carry_at = 0;
      for (const auto& c : tm.carry) {
        if (c.exit_reg >= 0) {
          for (int64_t k = 0; k < c.len; ++k)
            carry_adj[carry_at + k] =
                c.entry_reg < 0 ? 0.0
                                : adj[(size_t)map[(size_t)(c.entry_reg + k)]];
        } else if (c.entry_reg >= 0) {
          for (int64_t k = 0; k < c.len; ++k)
            carry_adj[carry_at + k] +=
                adj[(size_t)map[(size_t)(c.entry_reg + k)]];
        }
        carry_at += c.len;
      }
      if (profile)
        harvest_ns += Nanoseconds(Clock::now() - harvest_start).count();

      // Derivatives stop at the step's entry bindings. Seeded live-outs are
      // also named in case a guarded adjoint did not execute the instruction
      // which ordinarily consumes them. Mapping through adj_reg preserves
      // copy aliases; repeated clears are harmless and much cheaper than the
      // old full-file clear.
      const auto zero_start = profile ? Clock::now() : Clock::time_point{};
      const auto clear_mapped = [&](int32_t reg, int64_t len) {
        if (reg < 0) return;
        for (int64_t k = 0; k < len; ++k)
          adj[(size_t)map[(size_t)(reg + k)]] = 0.0;
      };
      if (sparse_adj_reset) {
        for (const auto& input : tm.step.ins)
          clear_mapped(input.reg, input.len);
        for (const auto& c : tm.carry) clear_mapped(c.exit_reg, c.len);
        for (const auto& b : tm.sinks) clear_mapped(b.step_reg, b.len);
        clear_mapped(tm.target_reg, tm.target_reg < 0 ? 0 : 1);
      }
      if (profile) zero_ns += Nanoseconds(Clock::now() - zero_start).count();
    }
  }

  carry_at = 0;
  for (size_t carry_index = 0; carry_index < s.templates.front().carry.size();
       ++carry_index) {
    const auto& c = s.templates.front().carry[carry_index];
    const bool any_active = std::any_of(s.templates.begin(), s.templates.end(),
                                        [&](const ScanSpec::Template& tm) {
                                          return tm.carry[carry_index].active;
                                        });
    if (any_active && ctx.in_adj[c.op_input].data != nullptr)
      for (int64_t k = 0; k < c.len; ++k)
        ctx.in_adj[c.op_input].data[c.input_offset + k] +=
            carry_adj[carry_at + k];
    carry_at += c.len;
  }
  std::fill(ctx.out_adj_vec.data, ctx.out_adj_vec.data + ctx.out_adj_vec.len,
            0.0);
  if (profile) {
    std::fprintf(
        stderr,
        "stanli_scan_bwd rows=%lld block_replay_ns=%.0f row_replay_ns=%.0f "
        "zero_ns=%.0f seed_ns=%.0f adjoint_ns=%.0f harvest_ns=%.0f "
        "step_bind_ns=%.0f step_program_ns=%.0f step_exit_ns=%.0f "
        "sparse_reset=%d retained_cells=%lld\n",
        static_cast<long long>(s.count), block_replay_ns, row_replay_ns,
        zero_ns, seed_ns, adjoint_ns, harvest_ns, row_step_profile.bind_ns,
        row_step_profile.program_ns, row_step_profile.exit_ns,
        sparse_adj_reset ? 1 : 0,
        static_cast<long long>(s.prepared_retention_cells));
    for (size_t i = 0; i < s.templates.size(); ++i)
      std::fprintf(stderr,
                   "stanli_scan_template index=%zu rows=%lld values=%d "
                   "adjoints=%d adj_code=%zu replay_code=%zu "
                   "invariant_cells=%lld retained_calls=%zu "
                   "retained_record_cells=%lld\n",
                   i,
                   static_cast<long long>(
                       s.template_for_iteration.empty()
                           ? (i == 0 ? s.count : 0)
                           : std::count(s.template_for_iteration.begin(),
                                        s.template_for_iteration.end(),
                                        static_cast<uint32_t>(i))),
                   s.templates[i].step.n_regs, s.templates[i].step.adj.n_regs,
                   s.templates[i].step.adj.code.size(),
                   s.templates[i].repeated_code.empty()
                       ? s.templates[i].step.code.size()
                       : s.templates[i].repeated_code.size(),
                   static_cast<long long>(s.templates[i].invariant_cache_cells),
                   s.templates[i].prepared_solve_retention.size(),
                   static_cast<long long>(
                       s.templates[i].prepared_retention_record_cells));
  }
}

int64_t scan_scratch(const Op& op, const Slot* slots) {
  return validate_and_layout(op, slots).total;
}

}  // namespace

size_t prepare_scan_invariants(ScanSpec::Template* tm) {
  if (tm == nullptr) return 0;
  tm->invariant_calls.clear();
  tm->repeated_code.clear();
  tm->invariant_cache_cells = 0;
  InvariantAnalysis analysis = analyze_invariants(*tm);
  tm->invariant_calls = std::move(analysis.calls);
  tm->repeated_code = std::move(analysis.repeated_code);
  tm->invariant_cache_cells = analysis.cache_cells;
  return tm->invariant_calls.size();
}

size_t prepare_scan_prepared_retention(ScanSpec* spec) {
  if (spec == nullptr) return 0;
  for (ScanSpec::Template& tm : spec->templates) {
    tm.prepared_solve_retention.clear();
    tm.prepared_retention_record_cells = 0;
  }
  spec->prepared_retention_iteration_offsets.clear();
  spec->prepared_retention_cells = 0;

  const char* const escape = std::getenv("STANLI_NO_SCAN_PREPARED_RETENTION");
  if (escape != nullptr && escape[0] != '0') return 0;
  const char* const force = std::getenv("STANLI_SCAN_PREPARED_RETENTION");
  if (force == nullptr || force[0] == '0') return 0;

  PreparedRetentionAnalysis analysis;
  try {
    if (!analyze_prepared_retention(*spec, &analysis) || analysis.calls == 0 ||
        analysis.cells <= 0 ||
        analysis.cells > kScanPreparedRetentionBudgetBytes /
                             static_cast<int64_t>(sizeof(double)))
      return 0;
  } catch (const std::exception&) {
    return 0;
  }

  // Everything above was computed in temporaries. These vector moves are the
  // publication point; no malformed candidate can leave a partial plan.
  for (size_t i = 0; i < spec->templates.size(); ++i) {
    auto& tm = spec->templates[i];
    tm.prepared_solve_retention = std::move(analysis.plans[i]);
    int64_t width = 0;
    for (const auto& plan : tm.prepared_solve_retention)
      width += 1 + plan.output_len + plan.scratch_len;
    tm.prepared_retention_record_cells = width;
  }
  spec->prepared_retention_iteration_offsets =
      std::move(analysis.iteration_offsets);
  spec->prepared_retention_cells = analysis.cells;
  return analysis.calls;
}

void register_scan_kernel() {
  register_kernel(OP_SCAN, Kernel{scan_fwd, scan_bwd, scan_scratch});
}

}  // namespace stanli
