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
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace stanli {

bool supports_selector_adjoint(const IslandProg& p) {
  if (p.n_regs < 0 || !p.calls.empty() || p.code.empty()) return false;
  const auto range = [&](int reg, int len) {
    return reg >= 0 && len >= 0 && reg <= p.n_regs && len <= p.n_regs - reg;
  };
  const auto pool_range = [&](int offset, int len) {
    return offset >= 0 && len >= 0 &&
           static_cast<size_t>(offset) <= p.pool.size() &&
           static_cast<size_t>(len) <=
               p.pool.size() - static_cast<size_t>(offset);
  };
  for (size_t k = 0; k < p.ins.size(); ++k) {
    const IslandProg::LiveIn& input = p.ins[k];
    const int op_input = input.input >= 0 ? input.input : static_cast<int>(k);
    if (!range(input.reg, input.len) || input.offset < 0 || input.input < -1 ||
        op_input < 0 || op_input >= 6)
      return false;
  }
  for (int output : p.out_regs)
    if (!range(output, 1)) return false;

  bool has_branch = false;
  for (size_t pc = 0; pc < p.code.size(); ++pc) {
    const Program::Instr& instruction = p.code[pc];
    switch (instruction.code) {
      case Program::CONST:
        if (!range(instruction.dst, 1) ||
            !pool_range(instruction.a, 1))
          return false;
        break;
      case Program::CONSTR:
        if (!range(instruction.dst, instruction.len) ||
            !pool_range(instruction.a, instruction.len))
          return false;
        break;
      case Program::MOV:
        if (!range(instruction.dst, 1) || !range(instruction.a, 1))
          return false;
        break;
      case Program::MOVR:
        if (!range(instruction.dst, instruction.len) ||
            !range(instruction.a, instruction.len))
          return false;
        break;
      case Program::GT:
      case Program::GE:
      case Program::LT:
      case Program::LE:
      case Program::EQ:
      case Program::NE:
        if (!range(instruction.dst, 1) || !range(instruction.a, 1) ||
            !range(instruction.b, 1))
          return false;
        break;
      case Program::JZ:
        if (!range(instruction.a, 1)) return false;
        [[fallthrough]];
      case Program::JMP:
        has_branch = true;
        // Strictly forward control is finite and needs no trace stack.
        if (instruction.dst <= static_cast<int>(pc) ||
            instruction.dst > static_cast<int>(p.code.size()))
          return false;
        break;
      default:
        return false;
    }
  }
  return has_branch;
}

namespace {

// Describe the exact graph-kernel backward for a structured Program
// instruction without changing its forward.  Keeping the direct-double
// instruction matters: several Stan Math matrix functions select a different
// (and substantially cheaper) primitive forward than their active overload.
// Return 0 for an unrelated instruction, 1 on success, -1 when malformed.
bool force_dimension_enabled(const char* flag, const char* minimum_flag,
                             int n) {
  const char* const value = std::getenv(flag);
  if (value == nullptr || value[0] == '0') return false;

  // Diagnostic force switches apply to every eligible dimension when no
  // threshold is present. A threshold is a deliberate second switch, so
  // setting it alone cannot change production lowering. Malformed values
  // fail closed instead of accidentally enabling a path in a benchmark.
  const char* const minimum = std::getenv(minimum_flag);
  if (minimum == nullptr) return true;
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(minimum, &end, 10);
  return errno == 0 && end != minimum && *end == '\0' && parsed > 0 &&
         parsed <= std::numeric_limits<int>::max() && n >= parsed;
}

bool environment_switch_enabled(const char* flag) {
  const char* const value = std::getenv(flag);
  return value != nullptr && value[0] != '0';
}

bool default_or_forced_dimension_enabled(const char* escape_flag,
                                         const char* force_flag,
                                         const char* minimum_flag, int n,
                                         int production_minimum) {
  // The escape hatch is authoritative, including over diagnostic force
  // settings. A present force switch owns its threshold decision: malformed
  // values fail closed instead of silently falling back to the production
  // default. A threshold without its force switch does not alter production.
  if (environment_switch_enabled(escape_flag)) return false;
  if (environment_switch_enabled(force_flag))
    return force_dimension_enabled(force_flag, minimum_flag, n);
  return n >= production_minimum;
}

bool prepared_cfg_mdivide_left_enabled(int n) {
  return force_dimension_enabled("STANLI_CFG_PREPARED_MDIVIDE_LEFT",
                                 "STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N", n);
}

bool prepared_cfg_mdivide_left_prim_lu_enabled(int n) {
  return default_or_forced_dimension_enabled(
      "STANLI_NO_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU",
      "STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU",
      "STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N", n, 32);
}

bool cfg_matrix_exp_block_frechet_enabled(int n) {
  return default_or_forced_dimension_enabled(
      "STANLI_NO_CFG_MATRIX_EXP_BLOCK_FRECHET",
      "STANLI_CFG_MATRIX_EXP_BLOCK_FRECHET",
      "STANLI_CFG_MATRIX_EXP_BLOCK_FRECHET_MIN_N", n, 6);
}

int make_structured_call(const Program::Instr& instruction,
                         Program::Call* call) {
  const auto set = [&](uint16_t opcode, uint8_t variant,
                       std::initializer_list<std::pair<int, int>> inputs,
                       int out, int out_len,
                       std::initializer_list<int> idata) {
    *call = Program::Call{};
    call->opcode = opcode;
    call->variant = variant;
    call->n_in = static_cast<int8_t>(inputs.size());
    int k = 0;
    for (const auto& input : inputs) {
      call->in[k] = input.first;
      call->in_len[k] = input.second;
      ++k;
    }
    call->out = out;
    call->out_len = out_len;
    call->idata.assign(idata.begin(), idata.end());
    const Kernel* kernel = find_kernel(opcode);
    if (kernel != nullptr) {
      call->forward = kernel->forward;
      call->backward = kernel->backward;
    }
  };
  switch (instruction.code) {
    case Program::MATRIX_EXP: {
      const int n = instruction.b;
      const int64_t width = static_cast<int64_t>(n) * n;
      if (n < 0 || instruction.c != n || width > INT32_MAX ||
          instruction.len != width)
        return -1;
      set(cfg_matrix_exp_block_frechet_enabled(n)
              ? OP_MATRIX_EXP_BLOCK_FRECHET
              : OP_MATRIX_EXP,
          0, {{instruction.a, static_cast<int>(width)}}, instruction.dst,
          static_cast<int>(width), {n});
      return 1;
    }
    case Program::MDIVIDE_LEFT:
    case Program::MDIVIDE_RIGHT_SPD: {
      const bool left = instruction.code == Program::MDIVIDE_LEFT;
      const int n = std::abs(instruction.c);
      const bool vector = instruction.c < 0;
      if (n == 0 || instruction.len <= 0 || instruction.len % n != 0 ||
          static_cast<int64_t>(n) * n > INT32_MAX)
        return -1;
      const int k = vector ? 1 : instruction.len / n;
      if (left) {
        const uint16_t opcode = prepared_cfg_mdivide_left_prim_lu_enabled(n)
                                    ? OP_MDIVIDE_LEFT_PREPARED_PRIM_LU
                                : prepared_cfg_mdivide_left_enabled(n)
                                    ? OP_MDIVIDE_LEFT_PREPARED
                                    : OP_MDIVIDE_LEFT;
        set(opcode,
            vector ? uint8_t{2} : uint8_t{0},
            {{instruction.a, n * n}, {instruction.b, instruction.len}},
            instruction.dst, instruction.len, {n, k});
      } else {
        set(OP_MDIVIDE_RIGHT_SPD, vector ? uint8_t{2} : uint8_t{0},
            {{instruction.b, instruction.len}, {instruction.a, n * n}},
            instruction.dst, instruction.len, {n, k});
      }
      return 1;
    }
    case Program::QUAD_FORM_SYM: {
      const int n = std::abs(instruction.c);
      const bool vector = instruction.c < 0;
      const int m = vector ? 1 : static_cast<int>(std::sqrt(instruction.len));
      if (n <= 0 || m <= 0 || m * m != instruction.len ||
          static_cast<int64_t>(n) * n > INT32_MAX ||
          static_cast<int64_t>(n) * m > INT32_MAX)
        return -1;
      set(OP_QUAD_FORM_SYM, vector ? uint8_t{1} : uint8_t{0},
          {{instruction.a, n * n}, {instruction.b, n * m}}, instruction.dst,
          m * m, {n, m});
      return 1;
    }
    default:
      return 0;
  }
}

void prepare_sparse_adj_clear(IslandProg& p) {
  std::vector<int32_t> cells;
  for (const auto& input : p.ins)
    for (int k = 0; k < input.len; ++k)
      cells.push_back(
          p.adj.adj_reg[static_cast<size_t>(input.reg + k)]);
  for (int output : p.out_regs)
    cells.push_back(p.adj.adj_reg[static_cast<size_t>(output)]);
  std::sort(cells.begin(), cells.end());
  cells.erase(std::unique(cells.begin(), cells.end()), cells.end());

  // An indexed store is intentionally priced at four contiguous stores.
  // This force-only experiment therefore refuses small files and any plan
  // which cannot remove at least three quarters of the clearing traffic.
  constexpr int32_t kMinAdjointCells = 256;
  p.sparse_adj_clear_eligible =
      p.adj.n_regs >= kMinAdjointCells &&
      cells.size() <= static_cast<size_t>(p.adj.n_regs / 4);
  p.sparse_adj_clear_cells = std::move(cells);
}

bool gen_adjoint_impl(IslandProg& p, bool cfg_mode) {
  Program& fwd = p;
  const std::vector<Program::Instr> orig = fwd.code;
  for (const auto& I : orig) {
    if (I.code != Program::CALL) continue;
    if (I.a < 0 || static_cast<size_t>(I.a) >= fwd.calls.size()) return false;
    Program::Call& call = fwd.calls[static_cast<size_t>(I.a)];
    if (call.n_in < 0 || call.n_in > 6 || call.forward == nullptr ||
        call.backward == nullptr)
      return false;
  }
  std::vector<int32_t> structured_call(orig.size(), -1);
  if (cfg_mode) {
    for (size_t i = 0; i < orig.size(); ++i) {
      Program::Call call;
      const int made = make_structured_call(orig[i], &call);
      if (made < 0) return false;
      if (made == 0) continue;
      if (call.forward == nullptr || call.backward == nullptr) return false;
      structured_call[i] = static_cast<int32_t>(fwd.calls.size());
      fwd.calls.push_back(std::move(call));
    }
    // Synthetic structured calls are normally backward-only and own no
    // forward scratch. Scratch-producing prepared solves are rewritten to CALL
    // below, so reserve their private value-only factor ranges. The QR path
    // retains matrixQR+hCoeffs; the exact-prim path retains PartialPivLU,
    // its row permutation, and one usability/rcond cell. gen_cfg_adjoint's
    // candidate copy makes growth transactional on any later refusal.
    for (int32_t call_index : structured_call) {
      if (call_index < 0) continue;
      Program::Call& call = fwd.calls[(size_t)call_index];
      if (call.opcode != OP_MDIVIDE_LEFT_PREPARED &&
          call.opcode != OP_MDIVIDE_LEFT_PREPARED_PRIM_LU)
        continue;
      if (call.idata.size() != 2 || call.idata[0] < 0) return false;
      const int64_t n = call.idata[0];
      const int64_t scratch_len =
          n * n + n +
          (call.opcode == OP_MDIVIDE_LEFT_PREPARED_PRIM_LU ? 1 : 0);
      if (scratch_len > INT32_MAX ||
          static_cast<int64_t>(fwd.n_regs) + scratch_len > INT32_MAX)
        return false;
      call.scratch = fwd.n_regs;
      call.scratch_len = static_cast<int32_t>(scratch_len);
      fwd.n_regs += call.scratch_len;
    }
  }
  const auto call_index_at = [&](size_t pc) -> int32_t {
    return orig[pc].code == Program::CALL ? orig[pc].a
                                          : structured_call[pc];
  };
  const auto has_call_at = [&](size_t pc) {
    return call_index_at(pc) >= 0;
  };
  const int n0 = fwd.n_regs;
  std::vector<Program::Call> bound_calls;
  bound_calls.reserve(fwd.calls.size());
  const bool elide_inactive_calls =
      std::getenv("STANLI_NO_INACTIVE_CALL_ELIDE") == nullptr;
  // Preserve one particularly common piece of structure that is still
  // unambiguous in the flat stream: a single forward JZ around the terminal
  // body (`if (c) body;`). Its reverse is the same guard around the reversed
  // body. Anything with an else arm, a suffix, nested control, a backedge, or
  // another unmodelled opcode keeps the var replay.
  int terminal_jz = -1;
  bool has_cfg_branch = false;
  for (size_t pc = 0; pc < orig.size(); ++pc) {
    const Program::Instr& I = orig[pc];
    if (!program_code_spec(I.code).has(kProgramNoAdjoint)) continue;
    if (cfg_mode && (I.code == Program::JZ || I.code == Program::JMP)) {
      // Strictly forward edges make the dynamic execution order a
      // subsequence of increasing original PCs.  Reversing the global stream
      // and filtering it by the trace therefore exactly reverses execution.
      if (I.dst <= static_cast<int>(pc) ||
          I.dst > static_cast<int>(orig.size()))
        return false;
      has_cfg_branch = true;
      continue;
    }
    if (cfg_mode && (I.code == Program::DIAG_PRE_MULTIPLY ||
                     I.code == Program::DIAG_POST_MULTIPLY))
      continue;
    if (cfg_mode && structured_call[pc] >= 0) continue;
    if (I.code == Program::JZ && terminal_jz < 0 &&
        I.dst == static_cast<int>(orig.size()) &&
        I.dst > static_cast<int>(pc)) {
      terminal_jz = static_cast<int>(pc);
      continue;
    }
    return false;
  }
  if (cfg_mode && !has_cfg_branch) return false;

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
    if (has_call_at((size_t)i)) {
      // A CALL's writes come from its payload: the output range and the
      // scratch its kernel stashes partials in.
      const int32_t call_index = call_index_at((size_t)i);
      if ((size_t)call_index >= fwd.calls.size()) return false;
      const Program::Call& call = fwd.calls[(size_t)call_index];
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
  for (size_t i = 0; i < orig.size(); ++i) {
    const Program::Instr& I = orig[i];
    if (I.code == Program::DENSITY && program_density_arity(I.len) > 3)
      for (int k = 0; k < program_density_arity(I.len); ++k)
        if (I.a + k >= 0 && I.a + k < n0) no_alias[(size_t)(I.a + k)] = 1;
    if (has_call_at(i)) {
      // The kernel's backward accumulates adjoints over whole ranges, so
      // every CALL range keeps identity adjoint cells.
      const int32_t call_index = call_index_at(i);
      if ((size_t)call_index >= fwd.calls.size()) return false;
      const Program::Call& call = fwd.calls[(size_t)call_index];
      for (int j = 0; j < call.n_in; ++j)
        for (int k = 0; k < call.in_len[j]; ++k) {
          if (call.in[j] + k < 0 || call.in[j] + k >= n0) return false;
          no_alias[(size_t)(call.in[j] + k)] = 1;
        }
      for (int k = 0; k < call.out_len; ++k) {
        if (call.out + k < 0 || call.out + k >= n0) return false;
        no_alias[(size_t)(call.out + k)] = 1;
      }
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
  for (size_t i = 0; i < orig.size(); ++i) {
    const Program::Instr& I = orig[i];
    const ProgramOpSpec& spec = program_code_spec(I.code);
    if (has_call_at(i)) {
      const int32_t call_index = call_index_at(i);
      if ((size_t)call_index >= fwd.calls.size()) return false;
      const Program::Call& call = fwd.calls[(size_t)call_index];
      if (call.n_in < 0 || call.n_in > 6) return false;
      for (int j = 0; j < call.n_in; ++j) {
        if (!in_range(call.in[j], call.in_len[j])) return false;
        if (overlaps(call.out, call.out_len, call.in[j], call.in_len[j]))
          return false;
      }
      if (!in_range(call.out, call.out_len)) return false;
      if (call.scratch_len && !in_range(call.scratch, call.scratch_len))
        return false;
      continue;
    }
    if (I.code == Program::DIAG_PRE_MULTIPLY ||
        I.code == Program::DIAG_POST_MULTIPLY) {
      const int rows = I.c, cols = I.len;
      const int64_t matrix_len = static_cast<int64_t>(rows) * cols;
      const int vector_len =
          I.code == Program::DIAG_PRE_MULTIPLY ? rows : cols;
      if (rows < 0 || cols < 0 || matrix_len > INT32_MAX ||
          !in_range(I.a, vector_len) ||
          !in_range(I.b, static_cast<int>(matrix_len)) ||
          !in_range(I.dst, static_cast<int>(matrix_len)) ||
          overlaps(I.a, vector_len, I.b, static_cast<int>(matrix_len)) ||
          overlaps(I.dst, static_cast<int>(matrix_len), I.a, vector_len) ||
          overlaps(I.dst, static_cast<int>(matrix_len), I.b,
                   static_cast<int>(matrix_len)))
        return false;
      continue;
    }
    const int reads = spec.has(kProgramNoInputs) ? 0 : 3;
    if (I.code == Program::DENSITY && program_density_arity(I.len) > 3 &&
        !in_range(I.a, program_density_arity(I.len)))
      return false;
    const bool ranged_density =
        I.code == Program::DENSITY && program_density_arity(I.len) > 3;
    if (!ranged_density) {
      if (reads > 0 && !in_range(I.a, 1)) return false;
      if (reads > 1 && !in_range(I.b, 1)) return false;
      if (reads > 2 && !in_range(I.c, 1)) return false;
    }
    const int wl = program_output_len(I);
    const bool coincident_range = spec.has(kProgramRangeOutput);
    if (spec.has(kProgramRangeA) &&
        (!in_range(I.a, I.len) || (overlaps(I.dst, wl, I.a, I.len) &&
                                   !(coincident_range && I.dst == I.a))))
      return false;
    if (spec.has(kProgramRangeB) &&
        (!in_range(I.b, I.len) || (overlaps(I.dst, wl, I.b, I.len) &&
                                   !(coincident_range && I.dst == I.b))))
      return false;
  }

  // Which registers carry a parameter: seeded from the live-ins the carver
  // marked active, grown forwards. A density argument outside that set gets
  // no partial (program_density.hpp) -- its adjoint cell reaches nothing the
  // executor reads. Read at the density rather than after the sweep, because
  // registers are cells: one holding data here may hold a parameter later.
  std::vector<uint8_t> dmask(orig.size(), 0xf);
  // A deterministic CALL with no parameter-active input can influence later
  // values, but it cannot carry an adjoint to anything the island publishes.
  // Remember that proof while activity is already being propagated; its
  // backward may be much more expensive than the surrounding bytecode (a
  // necessity island replays stan::math::var, for example).
  std::vector<uint8_t> call_active(orig.size(), 1);
  std::vector<uint8_t> call_input_mask(orig.size(), 0);
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
      if (has_call_at(i)) {
        const Program::Call& call =
            fwd.calls[(size_t)call_index_at(i)];
        for (int j = 0; j < call.n_in; ++j) {
          bool input_active = false;
          for (int k = 0; k < call.in_len[j]; ++k)
            input_active =
                input_active || active[(size_t)(call.in[j] + k)] != 0;
          if (input_active) call_input_mask[i] |= uint8_t{1} << j;
          any = any || input_active;
        }
        call_active[i] = !elide_inactive_calls || any ? 1 : 0;
        if (!any) continue;
        write(call.out, call.out_len);
        write(call.scratch, call.scratch_len);
        continue;
      }
      const ProgramOpSpec& spec = program_code_spec(I.code);
      if (spec.has(kProgramNoInputs)) continue;
      if (I.code == Program::DIAG_PRE_MULTIPLY ||
          I.code == Program::DIAG_POST_MULTIPLY) {
        const int vector_len =
            I.code == Program::DIAG_PRE_MULTIPLY ? I.c : I.len;
        read(I.a, vector_len);
        read(I.b, I.c * I.len);
      } else if (I.code == Program::DENSITY) {
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
        read(I.a, spec.has(kProgramRangeA) ? I.len : 1);
        if (spec.has(kProgramReadB))
          read(I.b, spec.has(kProgramRangeB) ? I.len : 1);
        if (spec.has(kProgramReadC)) read(I.c, 1);
      }
      if (any) write(I.dst, program_output_len(I));
    }
  }
  // The graph matrix kernels preserve the scalar types that select Stan
  // Math's numerically distinct overloads.  The Program compiler has shapes;
  // parameter activity becomes available only in the generator, after
  // LiveIn::active is bound, so complete those variants here.
  for (size_t i = 0; i < orig.size(); ++i) {
    if (!has_call_at(i)) continue;
    Program::Call& call = fwd.calls[(size_t)call_index_at(i)];
    const uint8_t mask = call_input_mask[i];
    if (call.opcode == OP_MDIVIDE_LEFT ||
        call.opcode == OP_MDIVIDE_LEFT_PREPARED ||
        call.opcode == OP_MDIVIDE_LEFT_PREPARED_PRIM_LU) {
      call.variant = static_cast<uint8_t>(
          (call.variant & 2u) | (mask != 0 ? 1u : 0u) |
          ((mask & 1u) ? 4u : 0u) | ((mask & 2u) ? 8u : 0u));
    } else if (call.opcode == OP_MDIVIDE_RIGHT_SPD) {
      // Right-solve CALL inputs are {dividend, divisor}, while the type bits
      // retain the language-level {divisor, dividend} meaning.
      call.variant = static_cast<uint8_t>(
          (call.variant & 2u) | (mask != 0 ? 1u : 0u) |
          ((mask & 2u) ? 4u : 0u) | ((mask & 1u) ? 8u : 0u));
    } else if (call.opcode == OP_QUAD_FORM_SYM) {
      call.variant =
          static_cast<uint8_t>((call.variant & 1u) | (mask != 0 ? 2u : 0u));
    }
  }
  // STANLI_NO_DENSITY_MASK=1 binds every density argument as a recorder
  // scalar again, which is the comparison the masks have to survive.
  if (std::getenv("STANLI_NO_DENSITY_MASK"))
    std::fill(dmask.begin(), dmask.end(), (uint8_t)0xf);

  std::vector<Program::Instr> ncode;
  ncode.reserve(orig.size());
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
    // A static copy equivalence is not path-sensitive.  Across a CFG it can
    // merge values produced by mutually exclusive writes and can leave the
    // destination sharing a source on a path where the copy never ran.
    if (cfg_mode) return false;
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

  // CALL scratch holds forward values/partials for the kernel's backward; it
  // is never itself differentiated. Do not give a scratch-only register an
  // adjoint cell. Keep the conservative identity mapping if a hand-authored
  // Program also exposes, reads, or writes that register -- compiled CALL
  // scratch is a private disjoint range, but the public Program contract is
  // cheap to guard.
  std::vector<char> forward_read((size_t)n0, 0), adjoint_output((size_t)n0, 0),
      externally_named((size_t)n0, 0);
  const auto mark_range = [&](std::vector<char>& marks, int reg, int len) {
    for (int k = 0; k < len; ++k) marks[(size_t)(reg + k)] = 1;
  };
  for (const auto& li : p.ins) mark_range(externally_named, li.reg, li.len);
  for (int reg : p.out_regs) externally_named[(size_t)reg] = 1;
  for (size_t i = 0; i < orig.size(); ++i) {
    const Program::Instr& I = orig[i];
    if (has_call_at(i)) {
      const Program::Call& call =
          fwd.calls[(size_t)call_index_at(i)];
      for (int j = 0; j < call.n_in; ++j)
        mark_range(forward_read, call.in[j], call.in_len[j]);
      mark_range(adjoint_output, call.out, call.out_len);
      continue;
    }
    mark_range(adjoint_output, I.dst, program_output_len(I));
    if (I.code == Program::DIAG_PRE_MULTIPLY ||
        I.code == Program::DIAG_POST_MULTIPLY) {
      mark_range(forward_read, I.a,
                 I.code == Program::DIAG_PRE_MULTIPLY ? I.c : I.len);
      mark_range(forward_read, I.b, I.c * I.len);
      continue;
    }
    const ProgramOpSpec& spec = program_code_spec(I.code);
    if (spec.has(kProgramNoInputs)) continue;
    if (I.code == Program::DENSITY) {
      const int arity = program_density_arity(I.len);
      if (arity > 3) {
        mark_range(forward_read, I.a, arity);
      } else {
        mark_range(forward_read, I.a, 1);
        if (arity > 1) mark_range(forward_read, I.b, 1);
        if (arity > 2) mark_range(forward_read, I.c, 1);
      }
      continue;
    }
    mark_range(forward_read, I.a,
               I.code == Program::DYN_INDEX
                   ? I.len
                   : (spec.has(kProgramRangeA) ? I.len : 1));
    if (spec.has(kProgramReadB))
      mark_range(forward_read, I.b, spec.has(kProgramRangeB) ? I.len : 1);
    if (spec.has(kProgramReadC)) mark_range(forward_read, I.c, 1);
  }
  if (n0 > 0) {
    for (size_t i = 0; i < orig.size(); ++i) {
      if (!has_call_at(i)) continue;
      const Program::Call& call =
          fwd.calls[(size_t)call_index_at(i)];
      if (!elide_inactive_calls &&
          call.opcode != OP_MDIVIDE_LEFT_PREPARED &&
          call.opcode != OP_MDIVIDE_LEFT_PREPARED_PRIM_LU)
        continue;
      for (int k = 0; k < call.scratch_len; ++k) {
        const int reg = call.scratch + k;
        if (!forward_read[(size_t)reg] && !adjoint_output[(size_t)reg] &&
            !externally_named[(size_t)reg])
          ap.adj_reg[(size_t)reg] = 0;
      }
    }
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
  std::vector<int32_t> trace_pc;
  if (cfg_mode) trace_pc.reserve(orig.size());
  auto emit_forward = [&](const Program::Instr& instruction,
                          int32_t original_pc) {
    ncode.push_back(instruction);
    if (cfg_mode) trace_pc.push_back(original_pc);
  };
  auto checkpoint = [&](int r, int len, bool needed) {
    if (!needed) return r;
    const int ck = n_regs;
    n_regs += len;
    Program::Instr save;
    save.code = len == 1 ? Program::MOV : Program::MOVR;
    save.dst = ck;
    save.a = r;
    save.len = len;
    emit_forward(save, -1);
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

  int branch_ncode = -1;
  int branch_prefix_adj = -1;
  int branch_condition_value = -1;
  std::vector<int32_t> original_boundary;
  std::vector<std::pair<int32_t, int32_t>> cfg_branches;
  if (cfg_mode) original_boundary.resize(orig.size() + 1, -1);

  for (int i = 0; i < (int)orig.size(); ++i) {
    const Program::Instr& I = orig[i];
    if (cfg_mode) original_boundary[(size_t)i] = (int32_t)ncode.size();
    if (I.code == Program::JZ) {
      if (cfg_mode) {
        // The trace, rather than a synthesized reverse guard, chooses the
        // reverse path.  A JZ has no value pullback of its own.
        cfg_branches.emplace_back((int32_t)ncode.size(), I.dst);
        emit_forward(I, i);
        continue;
      }
      // The body may overwrite the condition register. Preserve the value
      // that selected the forward path, then retarget the jump after value
      // checkpoints have expanded the instruction stream.
      branch_condition_value = save_range(I.a, 1, i);
      branch_ncode = static_cast<int>(ncode.size());
      emit_forward(I, i);
      branch_prefix_adj = static_cast<int>(ap.code.size());
      continue;
    }
    if (cfg_mode && I.code == Program::JMP) {
      cfg_branches.emplace_back((int32_t)ncode.size(), I.dst);
      emit_forward(I, i);
      continue;
    }
    if (has_call_at((size_t)i)) {
      // The kernel's backward may read its input VALUES, not just its
      // scratch (backward_ignores_values is a whitelist, not a
      // guarantee), and some read their output values too -- so both are
      // checkpointed whenever a later instruction overwrites them.
      const int32_t call_index = call_index_at((size_t)i);
      Program::Call call = fwd.calls[(size_t)call_index];
      const int32_t bound_index = static_cast<int32_t>(bound_calls.size());
      if (!call_active[(size_t)i]) {
        // The output can feed an active downstream instruction, so its
        // adjoint may be nonzero. Consume it just like the omitted kernel
        // backward would, while deliberately leaving the inactive inputs
        // untouched. No input/output value checkpoint is needed.
        Program::Instr inactive_forward = I;
        if (inactive_forward.code == Program::CALL)
          inactive_forward.a = bound_index;
        emit_forward(inactive_forward, i);
        if (call.out_len > 0) {
          AdjInstr clear;
          clear.code = call.out_len == 1 ? Program::CONST : Program::CONSTR;
          clear.dst =
              call.out_len == 1 ? map1(call.out) : mapn(call.out, call.out_len);
          clear.len = call.out_len;
          if (cfg_mode) clear.fwd_pc = i;
          ap.code.push_back(clear);
        }
        if (!mapped_ranges_ok) return false;
        bound_calls.push_back(std::move(call));
        continue;
      }
      for (int j = 0; j < call.n_in; ++j) {
        call.bwd_adj_in[j] = mapn(call.in[j], call.in_len[j]);
        call.bwd_value_in[j] = save_range(call.in[j], call.in_len[j], i);
      }
      call.bwd_adj_out = mapn(call.out, call.out_len);
      if (!mapped_ranges_ok) return false;
      Program::Instr forward = I;
      if (cfg_mode && structured_call[(size_t)i] >= 0 &&
          (call.opcode == OP_MDIVIDE_LEFT_PREPARED ||
           call.opcode == OP_MDIVIDE_LEFT_PREPARED_PRIM_LU)) {
        // A prepared backward consumes factor scratch, so unlike an ordinary
        // synthetic structured call its active forward must be the producer.
        forward = Program::Instr{};
        forward.code = Program::CALL;
      }
      if (forward.code == Program::CALL) forward.a = bound_index;
      emit_forward(forward, i);
      call.bwd_value_out = save_range(call.out, call.out_len, i);
      AdjInstr A;
      A.code = Program::CALL;
      A.a = bound_index;
      if (cfg_mode) A.fwd_pc = i;
      bound_calls.push_back(std::move(call));
      ap.code.push_back(A);
      continue;
    }
    if (aliasable(I, i)) {
      emit_forward(I, i);
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
    if (cfg_mode) A.fwd_pc = i;
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
    const ProgramOpSpec& spec = program_code_spec(I.code);
    if (I.code == Program::DIAG_PRE_MULTIPLY ||
        I.code == Program::DIAG_POST_MULTIPLY) {
      const int vector_len =
          I.code == Program::DIAG_PRE_MULTIPLY ? I.c : I.len;
      A.va = save_before(I.a, vector_len);
      A.vb = save_before(I.b, I.c * I.len);
    } else if (I.code == Program::DENSITY) {
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
        A.va = save_before(I.a, spec.has(kProgramRangeA) ? I.len : 1);
      if (spec.has(kProgramSaveB))
        A.vb = save_before(I.b, spec.has(kProgramRangeB) ? I.len : 1);
      if (spec.has(kProgramSaveC)) A.vc = save_before(I.c, 1);
    }

    emit_forward(I, i);

    // An output value is needed as this instruction LEFT it, so only a
    // later overwrite can lose it.
    if (spec.has(kProgramSaveOut)) A.vd = save_range(I.dst, wl, i);

    // Adjoint operands go through the sharing map; value operands do not.
    // A range has to map to a range: aliasing builds contiguous maps from
    // contiguous copies, so this holds, and refusing is cheaper than
    // scattering the interpreter's loops.
    A.dst = wl > 1 ? mapn(I.dst, wl) : map1(I.dst);
    if (I.code == Program::DIAG_PRE_MULTIPLY ||
        I.code == Program::DIAG_POST_MULTIPLY) {
      A.dst = mapn(I.dst, I.c * I.len);
      A.a = mapn(I.a,
                 I.code == Program::DIAG_PRE_MULTIPLY ? I.c : I.len);
      A.b = mapn(I.b, I.c * I.len);
    } else if (I.code == Program::DENSITY) {
      const int ar = program_density_arity(I.len);
      if (ar > 3) {
        A.a = mapn(I.a, ar);
      } else {
        A.a = map1(I.a);
        A.b = map1(I.b);
        A.c = map1(I.c);
      }
    } else if (!spec.has(kProgramNoInputs)) {
      A.a = spec.has(kProgramRangeA) ? mapn(I.a, I.len) : map1(I.a);
      A.b = spec.has(kProgramRangeB) ? mapn(I.b, I.len) : map1(I.b);
      A.c = map1(I.c);
    }
    if (!mapped_ranges_ok) return false;
    ap.code.push_back(A);
  }

  if (cfg_mode) {
    original_boundary[orig.size()] = (int32_t)ncode.size();
    for (const auto& branch : cfg_branches) {
      if (branch.first < 0 || (size_t)branch.first >= ncode.size() ||
          branch.second < 0 || (size_t)branch.second >= original_boundary.size())
        return false;
      const int32_t target = original_boundary[(size_t)branch.second];
      if (target < 0) return false;
      ncode[(size_t)branch.first].dst = target;
    }
  }

  if (terminal_jz >= 0) {
    if (branch_ncode < 0 || branch_prefix_adj < 0 || branch_condition_value < 0)
      return false;
    ncode[(size_t)branch_ncode].dst = static_cast<int32_t>(ncode.size());
    AdjInstr guard;
    guard.code = Program::JZ;
    guard.va = branch_condition_value;
    // ap.code is still in forward order. Appending the guard puts it before
    // the reversed body after the final reverse; false skips exactly that
    // body and resumes at the always-executed prefix's reverse.
    guard.dst = 1 + static_cast<int32_t>(ap.code.size()) - branch_prefix_adj;
    ap.code.push_back(guard);
  }

  std::reverse(ap.code.begin(), ap.code.end());
  fwd.code = std::move(ncode);
  fwd.calls = std::move(bound_calls);
  fwd.n_regs = n_regs;
  ap.trace_bits = cfg_mode ? static_cast<int>(orig.size()) : 0;
  p.adj = std::move(ap);
  p.trace_pc = std::move(trace_pc);
  prepare_sparse_adj_clear(p);
  return true;
}

}  // namespace

bool gen_adjoint(IslandProg& p) { return gen_adjoint_impl(p, false); }

bool prepare_cfg_trace_blocks(IslandProg& p) {
  if (p.adj.trace_bits <= 0 || !p.var_replay || p.code.empty() ||
      p.trace_pc.size() != p.code.size() || p.adj.code.empty() ||
      p.code.size() > static_cast<size_t>(INT32_MAX) ||
      p.adj.code.size() > static_cast<size_t>(INT32_MAX))
    return false;

  const Program& canonical = *p.var_replay;
  const size_t canonical_size = canonical.code.size();
  const size_t final_size = p.code.size();
  if (canonical_size == 0 ||
      canonical_size != static_cast<size_t>(p.adj.trace_bits))
    return false;

  bool has_branch = false;
  for (size_t pc = 0; pc < canonical_size; ++pc) {
    const Program::Instr& instruction = canonical.code[pc];
    if (instruction.code != Program::JZ && instruction.code != Program::JMP)
      continue;
    has_branch = true;
    if (instruction.dst <= static_cast<int>(pc) ||
        instruction.dst > static_cast<int>(canonical_size))
      return false;
  }
  if (!has_branch) return false;

  // Generation maps each canonical instruction once and in order. Inserted
  // value checkpoints alone carry -1 and are MOV/MOVR instructions.
  std::vector<int32_t> canonical_to_final(canonical_size, -1);
  size_t next_canonical = 0;
  for (size_t pc = 0; pc < final_size; ++pc) {
    const int32_t original = p.trace_pc[pc];
    if (original == -1) {
      if (p.code[pc].code != Program::MOV &&
          p.code[pc].code != Program::MOVR)
        return false;
      continue;
    }
    if (original < 0 || static_cast<size_t>(original) != next_canonical ||
        next_canonical >= canonical_size)
      return false;
    canonical_to_final[next_canonical] = static_cast<int32_t>(pc);
    ++next_canonical;
  }
  if (next_canonical != canonical_size) return false;

  // Which original PC is reached at or after each final instruction. This
  // validates branch retargeting while permitting checkpoints at a target's
  // generated boundary.
  std::vector<int32_t> next_mapped(final_size + 1,
                                   static_cast<int32_t>(canonical_size));
  for (size_t pc = final_size; pc-- > 0;) {
    next_mapped[pc] = p.trace_pc[pc] >= 0 ? p.trace_pc[pc]
                                           : next_mapped[pc + 1];
  }

  std::vector<uint8_t> leader(final_size, 0);
  leader[0] = 1;
  for (size_t pc = 0; pc < final_size; ++pc) {
    const Program::Instr& instruction = p.code[pc];
    if (instruction.code != Program::JZ && instruction.code != Program::JMP)
      continue;
    const int32_t original = p.trace_pc[pc];
    if (original < 0 || static_cast<size_t>(original) >= canonical_size)
      return false;
    const Program::Instr& source = canonical.code[static_cast<size_t>(original)];
    if (source.code != instruction.code ||
        instruction.dst <= static_cast<int>(pc) ||
        instruction.dst > static_cast<int>(final_size))
      return false;
    const int32_t reached = next_mapped[static_cast<size_t>(instruction.dst)];
    if (reached != source.dst) return false;
    if (pc + 1 < final_size && next_mapped[pc + 1] != original + 1)
      return false;
    if (pc + 1 < final_size) leader[pc + 1] = 1;
    if (instruction.dst < static_cast<int>(final_size))
      leader[static_cast<size_t>(instruction.dst)] = 1;
  }

  // CALL is not a semantic obstacle, but keeping it in a singleton range
  // makes the prototype incapable of accidentally moving a kernel backward
  // across a future block-level optimization.
  for (size_t pc = 0; pc < final_size; ++pc) {
    if (p.code[pc].code != Program::CALL) continue;
    leader[pc] = 1;
    if (pc + 1 < final_size) leader[pc + 1] = 1;
  }

  std::vector<int32_t> block_of_final(final_size, -1);
  int32_t block = -1;
  for (size_t pc = 0; pc < final_size; ++pc) {
    if (leader[pc]) ++block;
    if (block < 0) return false;
    block_of_final[pc] = block;
  }

  std::vector<AdjTraceBlock> plan;
  plan.reserve(static_cast<size_t>(block + 1));
  size_t begin = 0;
  int32_t current_block = -1;
  int32_t previous_block = std::numeric_limits<int32_t>::max();
  int32_t previous_fwd_pc = std::numeric_limits<int32_t>::max();
  for (size_t pc = 0; pc < p.adj.code.size(); ++pc) {
    const AdjInstr& instruction = p.adj.code[pc];
    if (instruction.fwd_pc < 0 ||
        instruction.fwd_pc >= p.adj.trace_bits ||
        instruction.fwd_pc >= previous_fwd_pc ||
        instruction.code == Program::JZ || instruction.code == Program::JMP)
      return false;
    previous_fwd_pc = instruction.fwd_pc;
    const int32_t final_pc =
        canonical_to_final[static_cast<size_t>(instruction.fwd_pc)];
    if (final_pc < 0 || static_cast<size_t>(final_pc) >= final_size)
      return false;
    const int32_t instruction_block =
        block_of_final[static_cast<size_t>(final_pc)];
    if (instruction_block > previous_block) return false;
    previous_block = instruction_block;

    const bool split =
        pc > begin &&
        (instruction_block != current_block ||
         instruction.code == Program::CALL ||
         p.adj.code[pc - 1].code == Program::CALL);
    if (split) {
      plan.push_back(AdjTraceBlock{static_cast<int32_t>(pc),
                                   p.adj.code[begin].fwd_pc});
      begin = pc;
    }
    current_block = instruction_block;
  }
  plan.push_back(AdjTraceBlock{static_cast<int32_t>(p.adj.code.size()),
                               p.adj.code[begin].fwd_pc});

  int32_t begin_check = 0;
  for (const AdjTraceBlock& range : plan) {
    if (range.end <= begin_check ||
        range.end > static_cast<int32_t>(p.adj.code.size()) ||
        range.trace_pc < 0 || range.trace_pc >= p.adj.trace_bits)
      return false;
    begin_check = range.end;
  }
  if (begin_check != static_cast<int32_t>(p.adj.code.size())) return false;
  for (AdjInstr& instruction : p.adj.code)
    instruction.pair = AdjPair::None;
  p.adj.has_pairs = false;
  p.adj.trace_blocks = std::move(plan);
  return true;
}

bool prepare_cfg_adjoint_superinstructions(IslandProg& p) {
  if (p.adj.trace_blocks.empty() || p.adj.code.empty()) return false;

  // Do not trust a caller-provided partition. Rebuild it transactionally
  // from the canonical CFG and require an exact match before any tags move
  // into p. This also rechecks forward-only edges, maps, and CALL singletons.
  IslandProg verified = p;
  if (!prepare_cfg_trace_blocks(verified) ||
      verified.adj.trace_blocks.size() != p.adj.trace_blocks.size())
    return false;
  for (size_t i = 0; i < p.adj.trace_blocks.size(); ++i) {
    if (verified.adj.trace_blocks[i].end != p.adj.trace_blocks[i].end ||
        verified.adj.trace_blocks[i].trace_pc !=
            p.adj.trace_blocks[i].trace_pc)
      return false;
  }

  const auto pair_for = [](Program::Code first,
                           Program::Code second) -> AdjPair {
    if (first == Program::MOV && second == Program::MOV)
      return AdjPair::MovMov;
    if (first == Program::MOV && second == Program::CONST)
      return AdjPair::MovConst;
    if (first == Program::NEG && second == Program::NEG)
      return AdjPair::NegNeg;
    if (first == Program::CONST && second == Program::MOV)
      return AdjPair::ConstMov;
    if (first == Program::MUL && second == Program::ADD)
      return AdjPair::MulAdd;
    if (first == Program::ADD && second == Program::MUL)
      return AdjPair::AddMul;
    if (first == Program::MUL && second == Program::MUL)
      return AdjPair::MulMul;
    if (first == Program::ADD && second == Program::ADD)
      return AdjPair::AddAdd;
    if (first == Program::SUB && second == Program::SUB)
      return AdjPair::SubSub;
    return AdjPair::None;
  };

  std::vector<AdjPair> tags(p.adj.code.size(), AdjPair::None);
  int32_t begin = 0;
  size_t paired = 0;
  for (const AdjTraceBlock& block : p.adj.trace_blocks) {
    if (block.end <= begin ||
        block.end > static_cast<int32_t>(p.adj.code.size()))
      return false;
    int32_t pc = begin;
    while (pc + 1 < block.end) {
      const AdjInstr& first = p.adj.code[static_cast<size_t>(pc)];
      const AdjInstr& second = p.adj.code[static_cast<size_t>(pc + 1)];
      // The whitelist contains scalar rules only. Keep the explicit shape
      // check so a malformed or future ranged encoding fails closed rather
      // than silently acquiring pair semantics.
      const AdjPair pair =
          first.len >= 0 && first.len <= 1 && second.len >= 0 &&
                  second.len <= 1
              ? pair_for(first.code, second.code)
              : AdjPair::None;
      if (pair == AdjPair::None) {
        ++pc;
        continue;
      }
      tags[static_cast<size_t>(pc)] = pair;
      ++paired;
      pc += 2;
    }
    begin = block.end;
  }
  if (begin != static_cast<int32_t>(p.adj.code.size())) return false;

  for (size_t i = 0; i < tags.size(); ++i) p.adj.code[i].pair = tags[i];
  p.adj.has_pairs = paired != 0;
  return true;
}

bool gen_cfg_adjoint(IslandProg& p) {
  IslandProg candidate = p;
  candidate.var_replay =
      std::make_shared<Program>(static_cast<const Program&>(p));
  if (!gen_adjoint_impl(candidate, true)) return false;
  const bool pair_force =
      environment_switch_enabled("STANLI_CFG_ADJ_SUPERINSTRUCTIONS") &&
      !environment_switch_enabled("STANLI_NO_CFG_ADJ_SUPERINSTRUCTIONS");
  const bool trace_force =
      (environment_switch_enabled("STANLI_CFG_ADJ_TRACE_BLOCKS") ||
       pair_force) &&
      !environment_switch_enabled("STANLI_NO_CFG_ADJ_TRACE_BLOCKS");
  if (trace_force && prepare_cfg_trace_blocks(candidate) && pair_force)
    (void)prepare_cfg_adjoint_superinstructions(candidate);
  p = std::move(candidate);
  return true;
}

bool cfg_native_profitable(const IslandProg& p) {
  if (p.adj.trace_bits <= 0) return true;
  if (std::getenv("STANLI_CFG_STRUCTURED_NATIVE")) return true;

  // Scalar CFGs are already a measured win at tiny sizes. Structured rules
  // have a different crossover: a 4x4 DIAG loses, while measured 19,754- and
  // 24,470-instruction structured paths win. Derive that path size from the
  // canonical forward PCs rather than total generated code: a large cold arm
  // must not make a cheap path look profitable.
  constexpr size_t kMinStructuredReverseWork = 16384;
  constexpr size_t kMaxZeroStructuredScan = 32768;
  if (!p.var_replay) return false;
  const Program& canonical = *p.var_replay;
  const size_t n = canonical.code.size();
  if (!canonical.calls.empty() || n == 0 ||
      n > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      p.adj.trace_bits != static_cast<int>(n) ||
      p.trace_pc.size() != p.code.size())
    return false;

  // Generation emits each canonical instruction once, in order; only value
  // checkpoints carry -1. Validate the forward trace map as well as the
  // reverse map before using either as a cost proof.
  size_t next_canonical_pc = 0;
  for (int32_t pc : p.trace_pc) {
    if (pc == -1) continue;
    if (pc < 0 || static_cast<size_t>(pc) != next_canonical_pc) return false;
    ++next_canonical_pc;
  }
  if (next_canonical_pc != n) return false;

  std::vector<size_t> reverse_work(n, 0);
  for (const AdjInstr& instruction : p.adj.code) {
    if (instruction.fwd_pc < 0 ||
        static_cast<size_t>(instruction.fwd_pc) >= n)
      return false;
    ++reverse_work[static_cast<size_t>(instruction.fwd_pc)];
  }

  std::vector<uint8_t> structured(n, 0);
  bool has_structured = false;
  bool has_branch = false;
  for (size_t pc = 0; pc < n; ++pc) {
    const Program::Instr& instruction = canonical.code[pc];
    switch (instruction.code) {
      case Program::CALL:
        // A canonical CALL can carry arbitrary user/kernel behavior. The
        // structured calls synthesized by gen_cfg_adjoint live only in p,
        // not in this immutable replay oracle.
        return false;
      case Program::DIAG_PRE_MULTIPLY:
      case Program::DIAG_POST_MULTIPLY:
      case Program::MATRIX_EXP:
      case Program::MDIVIDE_LEFT:
      case Program::MDIVIDE_RIGHT_SPD:
      case Program::QUAD_FORM_SYM:
        structured[pc] = 1;
        has_structured = true;
        break;
      case Program::JZ:
      case Program::JMP:
        has_branch = true;
        if (instruction.dst <= static_cast<int>(pc) ||
            instruction.dst > static_cast<int>(n))
          return false;
        break;
      default:
        // No-adjoint integer/index/reduction instructions are not part of
        // the supported scalar tier and cannot silently become a future
        // structured rule.
        if (program_code_spec(instruction.code).has(kProgramNoAdjoint))
          return false;
        break;
    }
  }
  if (!has_branch) return false;
  if (!has_structured) return true;

  const size_t unreachable = std::numeric_limits<size_t>::max();
  std::vector<size_t> min_any(n + 1, 0);
  std::vector<size_t> min_with_structured(n + 1, unreachable);
  std::vector<uint8_t> zero_structured_path(n + 1, 0);
  zero_structured_path[n] = 1;

  // Every edge is strictly forward, so this reverse-PC dynamic program is
  // exact. min_with_structured excludes zero-structured paths; the separate
  // boolean records whether such a path exists so its full trace-filter scan
  // can be bounded below.
  for (size_t pc = n; pc-- > 0;) {
    const Program::Instr& instruction = canonical.code[pc];
    size_t any_tail = min_any[pc + 1];
    size_t structured_tail = min_with_structured[pc + 1];
    bool zero_tail = zero_structured_path[pc + 1] != 0;
    if (instruction.code == Program::JMP) {
      const size_t dst = static_cast<size_t>(instruction.dst);
      any_tail = min_any[dst];
      structured_tail = min_with_structured[dst];
      zero_tail = zero_structured_path[dst] != 0;
    } else if (instruction.code == Program::JZ) {
      const size_t dst = static_cast<size_t>(instruction.dst);
      any_tail = std::min(any_tail, min_any[dst]);
      structured_tail =
          std::min(structured_tail, min_with_structured[dst]);
      zero_tail = zero_tail || zero_structured_path[dst] != 0;
    }

    min_any[pc] = reverse_work[pc] + any_tail;
    if (structured[pc]) {
      min_with_structured[pc] = reverse_work[pc] + any_tail;
      zero_structured_path[pc] = 0;
    } else {
      if (structured_tail != unreachable)
        min_with_structured[pc] = reverse_work[pc] + structured_tail;
      zero_structured_path[pc] = zero_tail ? 1 : 0;
    }
  }

  if (min_with_structured[0] == unreachable ||
      min_with_structured[0] < kMinStructuredReverseWork)
    return false;
  // A path that never invokes a structured rule still scans every reverse
  // entry to reject untaken PCs. The measured 24,470-entry canary wins on
  // that path, but cap the exemption so an arbitrarily large cold arm fails
  // closed.
  if (zero_structured_path[0] &&
      p.adj.code.size() > kMaxZeroStructuredScan)
    return false;
  return true;
}

// Ranged arms below map their operand and output ranges. The validator in
// gen_adjoint admits only disjoint ranges or exactly coincident ones, and the
// coincident case keeps the scalar loop: reading before clearing is what an
// in-place `x = exp(x)` needs.
using AdjA = Eigen::Map<Eigen::ArrayXd>;
using CAdjA = Eigen::Map<const Eigen::ArrayXd>;

void run_adjoint(const Program& fwd, const AdjProgram& ap, const double* val,
                 double* adj, const uint8_t* executed) {
  std::optional<KernelCtx> call_ctx_storage;
  if (!fwd.calls.empty()) call_ctx_storage.emplace();
  KernelCtx* call_ctx = call_ctx_storage ? &*call_ctx_storage : nullptr;
  const bool block_filter = !ap.trace_blocks.empty();
  size_t block_index = 0;
  int64_t block_end = 0;
  for (int64_t pc = 0; pc < static_cast<int64_t>(ap.code.size()); ++pc) {
    if (block_filter && pc == block_end) {
      const AdjTraceBlock& block = ap.trace_blocks[block_index++];
      block_end = block.end;
      if (executed == nullptr || block.trace_pc >= ap.trace_bits ||
          (executed[static_cast<size_t>(block.trace_pc) >> 3] &
           static_cast<uint8_t>(1u << (block.trace_pc & 7))) == 0) {
        pc = block_end - 1;
        continue;
      }
    }
    const AdjInstr& I = ap.code[(size_t)pc];
    if (!block_filter && I.fwd_pc >= 0) {
      // A traced program is admitted only when every edge is forward.  Its
      // executed PCs are therefore increasing, and this globally reversed,
      // filtered stream is exactly the dynamic reverse execution order.
      if (executed == nullptr || I.fwd_pc >= ap.trace_bits ||
          (executed[(size_t)I.fwd_pc >> 3] &
           static_cast<uint8_t>(1u << (I.fwd_pc & 7))) == 0)
        continue;
    }
    if (ap.has_pairs && I.pair != AdjPair::None && block_filter &&
        pc + 1 < block_end) {
      const AdjInstr& J = ap.code[static_cast<size_t>(pc + 1)];
      const auto mov = [&](const AdjInstr& instruction) {
        const double u = adj[instruction.dst];
        adj[instruction.dst] = 0.0;
        adj[instruction.a] += u;
      };
      const auto constant = [&](const AdjInstr& instruction) {
        adj[instruction.dst] = 0.0;
      };
      const auto neg = [&](const AdjInstr& instruction) {
        const double u = adj[instruction.dst];
        adj[instruction.dst] = 0.0;
        adj[instruction.a] -= u;
      };
      const auto mul = [&](const AdjInstr& instruction) {
        const double u = adj[instruction.dst];
        adj[instruction.dst] = 0.0;
        adj[instruction.a] += val[instruction.vb] * u;
        adj[instruction.b] += val[instruction.va] * u;
      };
      const auto add = [&](const AdjInstr& instruction) {
        const double u = adj[instruction.dst];
        adj[instruction.dst] = 0.0;
        adj[instruction.a] += u;
        adj[instruction.b] += u;
      };
      const auto sub = [&](const AdjInstr& instruction) {
        const double u = adj[instruction.dst];
        adj[instruction.dst] = 0.0;
        adj[instruction.a] += u;
        adj[instruction.b] -= u;
      };
      bool matched = true;
      switch (I.pair) {
        case AdjPair::MovMov:
          matched = I.code == Program::MOV && J.code == Program::MOV;
          if (matched) {
            mov(I);
            mov(J);
          }
          break;
        case AdjPair::MovConst:
          matched = I.code == Program::MOV && J.code == Program::CONST;
          if (matched) {
            mov(I);
            constant(J);
          }
          break;
        case AdjPair::NegNeg:
          matched = I.code == Program::NEG && J.code == Program::NEG;
          if (matched) {
            neg(I);
            neg(J);
          }
          break;
        case AdjPair::ConstMov:
          matched = I.code == Program::CONST && J.code == Program::MOV;
          if (matched) {
            constant(I);
            mov(J);
          }
          break;
        case AdjPair::MulAdd:
          matched = I.code == Program::MUL && J.code == Program::ADD;
          if (matched) {
            mul(I);
            add(J);
          }
          break;
        case AdjPair::AddMul:
          matched = I.code == Program::ADD && J.code == Program::MUL;
          if (matched) {
            add(I);
            mul(J);
          }
          break;
        case AdjPair::MulMul:
          matched = I.code == Program::MUL && J.code == Program::MUL;
          if (matched) {
            mul(I);
            mul(J);
          }
          break;
        case AdjPair::AddAdd:
          matched = I.code == Program::ADD && J.code == Program::ADD;
          if (matched) {
            add(I);
            add(J);
          }
          break;
        case AdjPair::SubSub:
          matched = I.code == Program::SUB && J.code == Program::SUB;
          if (matched) {
            sub(I);
            sub(J);
          }
          break;
        case AdjPair::None:
          matched = false;
          break;
      }
      if (matched) {
        ++pc;
        continue;
      }
    }
    if (I.code == Program::JZ) {
      if (val[I.va] == 0.0) pc = I.dst - 1;
      continue;
    }
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
        ctx.in_adj[k] = Desc{adj + call.bwd_adj_in[k], call.in_len[k]};
      }
      ctx.out = Desc{const_cast<double*>(val) + call.bwd_value_out,
                     call.out_len};
      ctx.out_adj_vec = Desc{adj + call.bwd_adj_out, call.out_len};
      ctx.out_adj = call.out_len == 1 ? adj[call.bwd_adj_out] : 0.0;
      ctx.variant = call.variant;
      ctx.scratch = const_cast<double*>(val) + call.scratch;
      ctx.idata = call.idata.data();
      ctx.n_idata = (int64_t)call.idata.size();
      ctx.udata = call.udata;
      ctx.program_call_hook = nullptr;
      call.backward(ctx);
      for (int j = 0; j < call.out_len; ++j)
        adj[call.bwd_adj_out + j] = 0.0;
      continue;
    }
    if (I.code == Program::DIAG_PRE_MULTIPLY ||
        I.code == Program::DIAG_POST_MULTIPLY) {
      using stan::math::var;
      using VarVec = Eigen::Matrix<var, Eigen::Dynamic, 1>;
      using VarMat = Eigen::Matrix<var, Eigen::Dynamic, Eigen::Dynamic>;
      const int rows = I.c, cols = I.len;
      const int vector_len =
          I.code == Program::DIAG_PRE_MULTIPLY ? rows : cols;
      stan::math::nested_rev_autodiff nested;
      VarVec vector(vector_len);
      VarMat matrix(rows, cols);
      for (int k = 0; k < vector_len; ++k) vector(k) = val[I.va + k];
      for (int k = 0; k < rows * cols; ++k)
        matrix.data()[k] = val[I.vb + k];
      VarMat output(rows, cols);
      if (I.code == Program::DIAG_PRE_MULTIPLY)
        output = stan::math::diag_pre_multiply(vector, matrix);
      else
        output = stan::math::diag_post_multiply(matrix, vector);
      var objective = 0.0;
      for (int k = 0; k < rows * cols; ++k)
        objective += output.data()[k] * adj[I.dst + k];
      stan::math::grad(objective.vi_);
      for (int k = 0; k < rows * cols; ++k) {
        adj[I.dst + k] = 0.0;
        adj[I.b + k] += matrix.data()[k].adj();
      }
      for (int k = 0; k < vector_len; ++k)
        adj[I.a + k] += vector(k).adj();
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
        adj[I.a] += t * stan::math::inv_logit(val[I.va] - val[I.vb]);
        adj[I.b] += t * stan::math::inv_logit(val[I.vb] - val[I.va]);
        break;
      case Program::LOG_DIFF_EXP:
        adj[I.dst] = 0.0;
        // Match rev/fun/log_diff_exp.hpp exactly. Besides being stable when
        // the arguments are close, expm1 has observably different rounding
        // from spelling either denominator with exp.
        adj[I.a] -= t / stan::math::expm1(val[I.vb] - val[I.va]);
        adj[I.b] -= t / stan::math::expm1(val[I.va] - val[I.vb]);
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
      case Program::DYN_INDEX:
      case Program::IDIV:
      case Program::MAX_RANGE:
      case Program::JMP:
      case Program::DIAG_PRE_MULTIPLY:
      case Program::DIAG_POST_MULTIPLY:
      case Program::MATRIX_EXP:
      case Program::MDIVIDE_LEFT:
      case Program::MDIVIDE_RIGHT_SPD:
      case Program::QUAD_FORM_SYM:
        break;  // gen_adjoint refuses these; unreachable
      case Program::JZ:
        break;  // handled as a reverse guard before reading an adjoint cell
      case Program::CALL:
        break;  // handled before this switch
    }
  }
}

}  // namespace stanli
