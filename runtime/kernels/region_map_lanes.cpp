#include <stanli/adjoint.hpp>
#include <stanli/adjoint_rules.hpp>
#include <stanli/program_density.hpp>
#include <stanli/region_map.hpp>

#include <algorithm>
#include <limits>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <set>
#include <type_traits>
#include <string>
#include <string_view>

namespace stanli {
namespace {

bool lane_opcode(Program::Code c) {
  switch (c) {
    case Program::CONST:
    case Program::FILL:
    case Program::CONSTR:
    case Program::MOV:
    case Program::ADD:
    case Program::SUB:
    case Program::MUL:
    case Program::DIV:
    case Program::FMA:
    case Program::IADD:
    case Program::IMOD:
    case Program::INEG:
    case Program::POW:
    case Program::FMAX:
    case Program::FMIN:
    case Program::NEG:
    case Program::EXP:
    case Program::LOG:
    case Program::LOG1P_EXP:
    case Program::SQRT:
    case Program::SQUARE:
    case Program::INV:
    case Program::FABS:
    case Program::GT:
    case Program::GE:
    case Program::LT:
    case Program::LE:
    case Program::EQ:
    case Program::DYN_INDEX:
    case Program::JZ:
    case Program::JMP:
    case Program::LSE2:
    case Program::LOG_DIFF_EXP:
    case Program::LOG_MIX:
    case Program::CALL:
      return true;
    default:
      return false;
  }
}

std::string opcode_name(Program::Code c) {
  static const char* const names[] = {
      "CONST",
      "FILL",
      "CONSTR",
      "MOV",
      "MOVR",
      "ADD",
      "SUB",
      "MUL",
      "DIV",
      "IMOD",
      "IDIV",
      "IADD",
      "ISUB",
      "IMUL",
      "INEG",
      "IABS",
      "POW",
      "FMAX",
      "FMIN",
      "NEG",
      "EXP",
      "LOG",
      "SQRT",
      "SQUARE",
      "INV",
      "FABS",
      "INV_LOGIT",
      "LOG1M",
      "LOG1P_EXP",
      "TANH",
      "GT",
      "GE",
      "LT",
      "LE",
      "EQ",
      "NE",
      "DYN_SET",
      "DYN_INDEX",
      "EXTREMA_RANGE",
      "JZ",
      "JMP",
      "LOG_RANGE",
      "EXP_RANGE",
      "DOT",
      "DYN_LSE_RANGE",
      "LSE_RANGE",
      "SOFTMAX",
      "LSE2",
      "LOG_DIFF_EXP",
      "LOG_MIX",
      "FMA",
      "DIAG_PRE_MULTIPLY",
      "DIAG_POST_MULTIPLY",
      "MDIVIDE_LEFT",
      "MDIVIDE_RIGHT_SPD",
      "DENSITY",
      "CALL",
      "TRANSFORM",
      "PRINT",
      "REJECT",
      "DENSITY_VEC",
      "RANGE",
  };
  const size_t i = static_cast<size_t>(c);
  return i < sizeof(names) / sizeof(names[0]) ? names[i]
                                              : std::to_string((int)c);
}

struct Span {
  int base = 0, len = 0;
};

bool overlaps(const Span& a, const Span& b) {
  return a.len > 0 && b.len > 0 && a.base < b.base + b.len &&
         b.base < a.base + a.len;
}

struct Refusal {
  std::string why;
};

[[noreturn]] void refuse(const std::string& why) { throw Refusal{why}; }

class Analysis {
 public:
  Analysis(RegionMapProg& p, int64_t storage_limit, int64_t recompute_cells,
           bool tile_calls)
      : p_(p),
        plan_(p.lanes),
        limit_(storage_limit),
        recompute_cells_(recompute_cells),
        tile_calls_(tile_calls) {}

  void run() {
    check_opcodes();
    build_blocks();
    find_flags();
    find_undefined_reads();
    analyze_invariance();
    classify_registers();
    classify_cells();
    decode_operands();
    find_adjoint_sinks();
    check_ranges();
    collect_seeds();
    build_windows();
    count_sites();
    size_tiles();
  }

 private:
  RegionMapProg& p_;
  RegionMapLanePlan& plan_;
  int64_t limit_;
  int64_t recompute_cells_;
  bool tile_calls_;
  int n_cells_ = 0;
  std::vector<int> writes_;
  std::vector<char> undefined_read_;
  std::vector<char> invariant_reg_;

  bool shared_reg(int r) const {
    return r >= 0 && r < (int)plan_.reg_slot.size() && plan_.reg_slot[r] < 0;
  }
  bool shared_cell(int c) const { return plan_.cell_slot[(size_t)c] < 0; }

  void check_opcodes() {
    std::set<std::string> unsupported;
    for (const auto& I : p_.code)
      if (!lane_opcode(I.code)) unsupported.insert(opcode_name(I.code));
    for (const auto& A : p_.adj.code)
      if (!lane_opcode(A.code) || A.code == Program::JZ ||
          A.code == Program::JMP)
        unsupported.insert(opcode_name(A.code));
    if (unsupported.empty()) return;
    std::string why = "opcodes not lane-batched:";
    for (const auto& name : unsupported) why += " " + name;
    refuse(why);
  }

  template <typename F>
  void each_instr_write(const Program::Instr& I, F f) const {
    switch (I.code) {
      case Program::JZ:
      case Program::JMP:
        break;
      case Program::CALL: {
        const Program::Call& call = p_.calls[(size_t)I.a];
        f(call.out, call.out_len);
        f(call.scratch, call.scratch_len);
        break;
      }
      case Program::CONSTR:
      case Program::FILL:
        f(I.dst, I.len);
        break;
      default:
        f(I.dst, 1);
    }
  }

  template <typename F>
  void each_body_write(F f) const {
    for (const auto& I : p_.code) each_instr_write(I, f);
  }

  template <typename F>
  void each_adjoint_value_read(const AdjInstr& A, F f) const {
    switch (A.code) {
      case Program::CALL: {
        const Program::Call& call = p_.calls[(size_t)A.a];
        for (int k = 0; k < call.n_in; ++k)
          f(call.bwd_value_in[k], call.in_len[k]);
        f(call.bwd_value_out, call.out_len);
        f(call.scratch, call.scratch_len);
        return;
      }
      case Program::MUL:
      case Program::FMA:
      case Program::FMAX:
      case Program::FMIN:
      case Program::LSE2:
      case Program::LOG_DIFF_EXP:
        f(A.va, 1);
        f(A.vb, 1);
        return;
      case Program::DIV:
      case Program::POW:
        f(A.va, 1);
        f(A.vb, 1);
        f(A.vd, 1);
        return;
      case Program::LOG_MIX:
        f(A.va, 1);
        f(A.vb, 1);
        f(A.vc, 1);
        return;
      case Program::LOG:
      case Program::LOG1P_EXP:
      case Program::SQUARE:
      case Program::INV:
      case Program::FABS:
        f(A.va, 1);
        return;
      case Program::EXP:
      case Program::SQRT:
        f(A.vd, 1);
        return;
      case Program::DYN_INDEX:
        f(A.vb, 1);
        return;
      default:
        return;
    }
  }

  void find_flags() {
    const int n = p_.n_regs;
    const int n_blocks = (int)plan_.blocks.size();
    plan_.guard_reg.assign((size_t)n, 0);
    for (const auto& seg : p_.adj.segments) {
      if (seg.guard < 0 || seg.guard >= n) refuse("segment guard out of range");
      plan_.guard_reg[(size_t)seg.guard] = 1;
    }
    const auto guard = [&](int reg) {
      return reg >= 0 && reg < n && plan_.guard_reg[(size_t)reg] != 0;
    };
    const auto reads_guard = [&](int reg, int len) {
      for (int k = 0; k < len; ++k)
        if (guard(reg + k)) refuse("a flag register is read as a value");
    };
    std::vector<int> block_of(p_.code.size(), 0);
    for (int b = 0; b < n_blocks; ++b)
      for (int pc = plan_.blocks[(size_t)b].begin;
           pc < plan_.blocks[(size_t)b].end; ++pc)
        block_of[(size_t)pc] = b;

    struct Writes {
      std::vector<int> set_blocks;
      int first_set = std::numeric_limits<int>::max();
      int last_clear = -1;
    };
    std::vector<Writes> writes((size_t)n);
    const auto record = [&](int reg, int pc, double value) {
      Writes& w = writes[(size_t)reg];
      if (value != 0.0) {
        w.set_blocks.push_back(block_of[(size_t)pc]);
        w.first_set = std::min(w.first_set, pc);
      } else {
        if (block_of[(size_t)pc] != 0)
          refuse("a flag is cleared outside the entry block");
        w.last_clear = std::max(w.last_clear, pc);
      }
    };
    plan_.flag_store.assign(p_.code.size(), 0);
    for (size_t pc = 0; pc < p_.code.size(); ++pc) {
      const Program::Instr& I = p_.code[pc];
      each_body_read(I, reads_guard);
      switch (I.code) {
        case Program::CONST:
          if (guard(I.dst)) {
            record(I.dst, (int)pc, p_.pool[(size_t)I.a]);
            plan_.flag_store[pc] = 1;
          }
          break;
        case Program::CONSTR:
        case Program::FILL: {
          int flags = 0;
          for (int k = 0; k < I.len; ++k) flags += guard(I.dst + k);
          if (!flags) break;
          if (flags != I.len)
            refuse("a constant fill mixes flag and value registers");
          for (int k = 0; k < I.len; ++k)
            record(
                I.dst + k, (int)pc,
                p_.pool[(size_t)(I.code == Program::CONSTR ? I.a + k : I.a)]);
          plan_.flag_store[pc] = 1;
          break;
        }
        default:
          each_instr_write(I, [&](int reg, int len) {
            for (int k = 0; k < len; ++k)
              if (guard(reg + k)) refuse("a flag register has another writer");
          });
      }
    }
    for (const auto& A : p_.adj.code) each_adjoint_value_read(A, reads_guard);

    plan_.segment_count = (int)p_.adj.segments.size();
    plan_.segment_blocks.assign(p_.adj.segments.size(), {});
    for (size_t s = 0; s < p_.adj.segments.size(); ++s) {
      const Writes& w = writes[(size_t)p_.adj.segments[s].guard];
      if (w.set_blocks.empty()) refuse("a segment guard is never set");
      if (w.last_clear < 0 || w.last_clear > w.first_set)
        refuse("a segment guard is not reset before it is set");
      auto& blocks = plan_.segment_blocks[s];
      blocks = w.set_blocks;
      std::sort(blocks.begin(), blocks.end());
      blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    }
  }

  void classify_registers() {
    const int n = p_.n_regs;
    std::vector<char> per_lane((size_t)n, 0);
    each_body_write([&](int reg, int len) {
      for (int k = 0; k < len; ++k) {
        if (reg + k < 0 || reg + k >= n) refuse("register out of range");
        if (!plan_.guard_reg[(size_t)(reg + k)])
          per_lane[(size_t)(reg + k)] = 1;
      }
    });
    per_lane[(size_t)p_.iter_reg] = 1;
    plan_.reg_slot.assign((size_t)n, -1);
    int next = 0;
    for (int r = 0; r < n; ++r)
      if (per_lane[(size_t)r]) plan_.reg_slot[(size_t)r] = next++;
    plan_.fwd_regs = next;
  }

  static bool pure_clear(Program::Code c) {
    switch (c) {
      case Program::CONST:
      case Program::GT:
      case Program::GE:
      case Program::LT:
      case Program::LE:
      case Program::EQ:
      case Program::IADD:
      case Program::IMOD:
      case Program::INEG:
      case Program::FILL:
      case Program::CONSTR:
        return true;
      default:
        return false;
    }
  }

  template <typename F>
  void each_adjoint_accumulation(const AdjInstr& A, F f) const {
    switch (A.code) {
      case Program::CALL: {
        const Program::Call& call = p_.calls[(size_t)A.a];
        for (int k = 0; k < call.n_in; ++k)
          if (call.input_adjoint_mask & (1u << k))
            f(call.bwd_adj_in[k], call.in_len[k]);
        return;
      }
      case Program::FMA:
      case Program::LOG_MIX:
        f(A.a, 1);
        f(A.b, 1);
        f(A.c, 1);
        return;
      case Program::ADD:
      case Program::SUB:
      case Program::MUL:
      case Program::DIV:
      case Program::POW:
      case Program::FMAX:
      case Program::FMIN:
      case Program::LSE2:
      case Program::LOG_DIFF_EXP:
        f(A.a, 1);
        f(A.b, 1);
        return;
      case Program::MOV:
      case Program::NEG:
      case Program::EXP:
      case Program::LOG:
      case Program::LOG1P_EXP:
      case Program::SQRT:
      case Program::SQUARE:
      case Program::INV:
      case Program::FABS:
        f(A.a, 1);
        return;
      default:
        return;
    }
  }

  int32_t reg_offset(int r) const {
    if (r < 0 || r >= (int)plan_.reg_slot.size()) return -1;
    const int s = plan_.reg_slot[(size_t)r];
    return s >= 0 ? s * kRegionMapTile : -1;
  }

  int32_t cell_offset(int c) const {
    if (c < 0 || c >= (int)plan_.cell_slot.size()) return -1;
    const int s = plan_.cell_slot[(size_t)c];
    return s >= 0 ? s * kRegionMapTile : -1;
  }

  void decode_operands() {
    plan_.ops.assign(p_.code.size(), {});
    for (size_t pc = 0; pc < p_.code.size(); ++pc) {
      const Program::Instr& I = p_.code[pc];
      auto& o = plan_.ops[pc];
      switch (I.code) {
        case Program::MOV:
        case Program::NEG:
        case Program::EXP:
        case Program::LOG:
        case Program::LOG1P_EXP:
        case Program::SQRT:
        case Program::SQUARE:
        case Program::INV:
        case Program::FABS:
        case Program::INEG:
          o.dst = reg_offset(I.dst);
          o.a = reg_offset(I.a);
          break;
        case Program::ADD:
        case Program::SUB:
        case Program::MUL:
        case Program::DIV:
        case Program::IADD:
        case Program::IMOD:
        case Program::POW:
        case Program::FMAX:
        case Program::FMIN:
        case Program::GT:
        case Program::GE:
        case Program::LT:
        case Program::LE:
        case Program::EQ:
        case Program::LSE2:
        case Program::LOG_DIFF_EXP:
          o.dst = reg_offset(I.dst);
          o.a = reg_offset(I.a);
          o.b = reg_offset(I.b);
          break;
        case Program::FMA:
        case Program::LOG_MIX:
          o.dst = reg_offset(I.dst);
          o.a = reg_offset(I.a);
          o.b = reg_offset(I.b);
          o.c = reg_offset(I.c);
          break;
        case Program::CONST:
        case Program::DYN_INDEX:
          o.dst = reg_offset(I.dst);
          if (I.code == Program::DYN_INDEX) {
            o.a = reg_offset(I.a + I.c);
            o.b = reg_offset(I.b);
          }
          break;
        case Program::JZ:
          o.a = reg_offset(I.a);
          break;
        default:
          break;
      }
    }
    plan_.adj_ops.assign(p_.adj.code.size(), {});
    for (size_t pc = 0; pc < p_.adj.code.size(); ++pc) {
      const AdjInstr& A = p_.adj.code[pc];
      auto& o = plan_.adj_ops[pc];
      if (A.code == Program::CALL) continue;
      o.dst = cell_offset(A.dst);
      o.a = cell_offset(A.a);
      o.b = cell_offset(A.b);
      o.c = cell_offset(A.c);
      o.va = reg_offset(A.va);
      o.vb = reg_offset(A.vb);
      o.vc = reg_offset(A.vc);
      o.vd = reg_offset(A.vd);
    }
  }

  void find_adjoint_sinks() {
    const size_t n = (size_t)n_cells_;
    std::vector<char> read(n, 0), dirty(n, 0);
    bool dirty_all = false;
    const auto mark = [&](std::vector<char>& v, int base, int len) {
      for (int k = 0; k < len; ++k)
        if (base + k >= 0 && (size_t)(base + k) < n) v[(size_t)(base + k)] = 1;
    };
    for (const auto& A : p_.adj.code) {
      if (A.code == Program::DYN_INDEX) dirty_all = true;
      each_adjoint_accumulation(
          A, [&](int base, int len) { mark(dirty, base, len); });
      if (pure_clear(A.code)) continue;
      if (A.code == Program::CALL) {
        const Program::Call& call = p_.calls[(size_t)A.a];
        mark(read, call.bwd_adj_out, call.out_len);
      } else {
        mark(read, A.dst, 1);
      }
    }
    mark(dirty, p_.adj.adj_reg[(size_t)p_.out_regs[0]], 1);
    const auto sink = [&](int cell) {
      return cell >= 0 && (size_t)cell < n && !shared_cell(cell) &&
             !read[(size_t)cell];
    };
    plan_.adj_skip.assign(p_.adj.code.size(), 0);
    plan_.adj_residue.assign(n, 0);
    for (size_t pc = 0; pc < p_.adj.code.size(); ++pc) {
      const AdjInstr& A = p_.adj.code[pc];
      if (!pure_clear(A.code)) continue;
      const int width =
          (A.code == Program::FILL || A.code == Program::CONSTR) ? A.len : 1;
      bool all = width > 0;
      for (int k = 0; k < width; ++k) all = all && sink(A.dst + k);
      if (!all) continue;
      plan_.adj_skip[pc] = 1;
      for (int k = 0; k < width; ++k)
        if (dirty_all || dirty[(size_t)(A.dst + k)])
          plan_.adj_residue[(size_t)(A.dst + k)] = 1;
    }
  }

  void classify_cells() {
    n_cells_ = p_.adj.n_regs;
    const auto& map = p_.adj.adj_reg;
    std::vector<char> shared((size_t)n_cells_, 0);
    for (size_t k = 0; k + 1 < p_.ins.size(); ++k) {
      const auto& li = p_.ins[k];
      for (int i = 0; i < li.len; ++i) {
        const int32_t cell = map[(size_t)(li.reg + i)];
        if (cell < 0 || cell >= n_cells_) refuse("live-in cell out of range");
        shared[(size_t)cell] = 1;
      }
    }
    plan_.cell_slot.assign((size_t)n_cells_, -1);
    int next = 0;
    for (int c = 0; c < n_cells_; ++c)
      if (!shared[(size_t)c]) plan_.cell_slot[(size_t)c] = next++;
    plan_.adj_cells = next;
    const int32_t target = map[(size_t)p_.out_regs[0]];
    if (target < 0 || target >= n_cells_ || shared_cell(target))
      refuse("target cell is shared");
  }

  void same_class_regs(int base, int len, const char* what) const {
    if (len <= 0) return;
    if (base < 0 || base + len > p_.n_regs)
      refuse(std::string(what) + " range out of bounds");
    const bool s = shared_reg(base);
    for (int k = 1; k < len; ++k)
      if (shared_reg(base + k) != s)
        refuse(std::string(what) +
               " range mixes shared and per-lane registers");
  }

  void same_class_cells(int base, int len, const char* what) const {
    if (len <= 0) return;
    if (base < 0 || base + len > n_cells_)
      refuse(std::string(what) + " cell range out of bounds");
    const bool s = shared_cell(base);
    for (int k = 1; k < len; ++k)
      if (shared_cell(base + k) != s)
        refuse(std::string(what) +
               " cell range mixes shared and per-lane cells");
  }

  void per_lane_cells(int base, int len, const char* what) const {
    same_class_cells(base, len, what);
    if (len > 0 && shared_cell(base))
      refuse(std::string("adjoint consumes a shared cell (") + what + ")");
  }

  void check_ranges() {
    for (const auto& I : p_.code) {
      switch (I.code) {
        case Program::CONSTR:
        case Program::FILL:
          same_class_regs(I.dst, I.len, "constant fill");
          break;
        case Program::DYN_INDEX:
          same_class_regs(I.a + I.c, I.len, "indexed");
          break;
        case Program::CALL: {
          const Program::Call& call = p_.calls[(size_t)I.a];
          for (int k = 0; k < call.n_in; ++k)
            same_class_regs(call.in[k], call.in_len[k], "call input");
          same_class_regs(call.out, call.out_len, "call output");
          same_class_regs(call.scratch, call.scratch_len, "call scratch");
          break;
        }
        default:
          break;
      }
    }
    for (const auto& A : p_.adj.code) {
      switch (A.code) {
        case Program::CALL: {
          const Program::Call& call = p_.calls[(size_t)A.a];
          for (int k = 0; k < call.n_in; ++k)
            same_class_regs(call.bwd_value_in[k], call.in_len[k],
                            "call backward value");
          same_class_regs(call.bwd_value_out, call.out_len,
                          "call backward output");
          same_class_regs(call.scratch, call.scratch_len, "call scratch");
          for (int k = 0; k < call.n_in; ++k)
            if (call.input_adjoint_mask & (1u << k))
              same_class_cells(call.bwd_adj_in[k], call.in_len[k],
                               "call input adjoint");
          per_lane_cells(call.bwd_adj_out, call.out_len, "call output");
          break;
        }
        case Program::CONSTR:
        case Program::FILL:
          per_lane_cells(A.dst, A.len, "constant fill");
          break;
        case Program::DYN_INDEX:
          per_lane_cells(A.dst, 1, "indexed output");
          same_class_cells(A.a, A.len, "indexed");
          break;
        default:
          per_lane_cells(A.dst, 1, "output");
          break;
      }
    }
  }

  template <typename F>
  void each_body_read(const Program::Instr& I, F f) const {
    switch (I.code) {
      case Program::JMP:
        return;
      case Program::JZ:
        f(I.a, 1);
        return;
      case Program::DYN_INDEX:
        f(I.a + I.c, I.len);
        f(I.b, 1);
        return;
      case Program::CALL: {
        const Program::Call& call = p_.calls[(size_t)I.a];
        for (int k = 0; k < call.n_in; ++k) f(call.in[k], call.in_len[k]);
        return;
      }
      default:
        break;
    }
    const int operand[3] = {I.a, I.b, I.c};
    for (int k = 0; k < 3; ++k)
      if (program_reads(I, k)) f(operand[k], program_input_len(I, k));
  }

  void find_undefined_reads() {
    const auto& blocks = plan_.blocks;
    const size_t n_blocks = blocks.size();
    const size_t n_regs = (size_t)p_.n_regs;
    std::vector<int> writes((size_t)p_.n_regs, 0);
    each_body_write([&](int reg, int len) {
      for (int k = 0; k < len; ++k) {
        if (reg + k < 0 || reg + k >= p_.n_regs)
          refuse("register out of range");
        ++writes[(size_t)(reg + k)];
      }
    });
    writes_ = writes;
    undefined_read_.assign(n_regs, 0);
    std::vector<std::vector<char>> in(n_blocks);
    std::vector<char> reached(n_blocks, 0);
    reached[0] = 1;
    in[0].assign(n_regs, 0);
    for (size_t r = 0; r < n_regs; ++r) in[0][r] = writes[r] == 0;
    in[0][(size_t)p_.iter_reg] = 1;
    const auto meet = [&](size_t to, const std::vector<char>& from) {
      if (to >= n_blocks) return;
      if (!reached[to]) {
        reached[to] = 1;
        in[to] = from;
        return;
      }
      for (size_t k = 0; k < n_regs; ++k) in[to][k] &= from[k];
    };
    for (size_t b = 0; b < n_blocks; ++b) {
      if (!reached[b]) continue;
      std::vector<char> defined = in[b];
      for (int pc = blocks[b].begin; pc < blocks[b].end; ++pc) {
        const Program::Instr& I = p_.code[(size_t)pc];
        each_body_read(I, [&](int reg, int len) {
          for (int k = 0; k < len; ++k)
            if (!defined[(size_t)(reg + k)])
              undefined_read_[(size_t)(reg + k)] = 1;
        });
        switch (I.code) {
          case Program::JZ:
          case Program::JMP:
            break;
          case Program::CALL: {
            const Program::Call& call = p_.calls[(size_t)I.a];
            for (int k = 0; k < call.out_len; ++k)
              defined[(size_t)(call.out + k)] = 1;
            for (int k = 0; k < call.scratch_len; ++k)
              defined[(size_t)(call.scratch + k)] = 1;
            break;
          }
          case Program::CONSTR:
          case Program::FILL:
            for (int k = 0; k < I.len; ++k) defined[(size_t)(I.dst + k)] = 1;
            break;
          default:
            defined[(size_t)I.dst] = 1;
        }
      }
      if (blocks[b].taken >= 0) meet((size_t)blocks[b].taken, defined);
      if (blocks[b].fall >= 0) meet((size_t)blocks[b].fall, defined);
    }
  }

  void collect_seeds() {
    for (int r = 0; r < p_.n_regs; ++r) {
      const int s = plan_.reg_slot[(size_t)r];
      if (s >= 0 && undefined_read_[(size_t)r] && r != p_.iter_reg)
        plan_.seed_regs.push_back(r);
    }
  }

  bool shared_operand(int r) const {
    return r != p_.iter_reg &&
           (!writes_[(size_t)r] || invariant_reg_[(size_t)r]);
  }

  void analyze_invariance() {
    const int n = p_.n_regs;
    invariant_reg_.assign((size_t)n, 0);
    std::vector<char> excluded((size_t)n, 0);
    const auto exclude = [&](int base, int len) {
      if (len <= 1) return;
      for (int k = 0; k < len; ++k)
        if (base + k >= 0 && base + k < n) excluded[(size_t)(base + k)] = 1;
    };
    for (const auto& seg : p_.adj.segments)
      if (seg.guard >= 0 && seg.guard < n) excluded[(size_t)seg.guard] = 1;
    for (const auto& I : p_.code) {
      switch (I.code) {
        case Program::CONSTR:
        case Program::FILL:
          exclude(I.dst, I.len);
          break;
        case Program::DYN_INDEX:
          exclude(I.a + I.c, I.len);
          break;
        case Program::CALL: {
          const Program::Call& call = p_.calls[(size_t)I.a];
          for (int k = 0; k < call.n_in; ++k)
            exclude(call.in[k], call.in_len[k]);
          exclude(call.out, call.out_len);
          exclude(call.scratch, call.scratch_len);
          break;
        }
        default:
          break;
      }
    }
    for (const auto& A : p_.adj.code) {
      if (A.code != Program::CALL) continue;
      const Program::Call& call = p_.calls[(size_t)A.a];
      for (int k = 0; k < call.n_in; ++k)
        exclude(call.bwd_value_in[k], call.in_len[k]);
      exclude(call.bwd_value_out, call.out_len);
    }
    plan_.invariant.assign(p_.code.size(), 0);
    for (size_t pc = 0; pc < p_.code.size(); ++pc) {
      const Program::Instr& I = p_.code[pc];
      if (I.code == Program::JMP) continue;
      bool reads = false, shared = true;
      each_body_read(I, [&](int reg, int len) {
        for (int k = 0; k < len; ++k) {
          reads = true;
          if (!shared_operand(reg + k)) shared = false;
        }
      });
      if (!reads || !shared) continue;
      plan_.invariant[pc] = 1;
      switch (I.code) {
        case Program::JZ:
        case Program::CALL:
        case Program::CONSTR:
        case Program::FILL:
        case Program::DYN_INDEX:
          continue;
        default:
          break;
      }
      const size_t d = (size_t)I.dst;
      if (writes_[d] != 1 || excluded[d] || undefined_read_[d]) continue;
      invariant_reg_[d] = 1;
    }
  }

  void build_blocks() {
    const int n = (int)p_.code.size();
    std::vector<char> leader((size_t)n + 1, 0);
    leader[0] = 1;
    for (int pc = 0; pc < n; ++pc) {
      const auto& I = p_.code[(size_t)pc];
      if (I.code != Program::JZ && I.code != Program::JMP) continue;
      if (I.dst <= pc || I.dst > n) refuse("jump is not forward");
      leader[(size_t)(pc + 1)] = 1;
      leader[(size_t)I.dst] = 1;
    }
    std::vector<int> block_of((size_t)n + 1, 0);
    int blocks = 0;
    for (int pc = 0; pc < n; ++pc) {
      if (leader[(size_t)pc]) {
        ++blocks;
        plan_.blocks.emplace_back();
        plan_.blocks.back().begin = pc;
      }
      block_of[(size_t)pc] = blocks - 1;
      plan_.blocks.back().end = pc + 1;
    }
    block_of[(size_t)n] = blocks;
    for (auto& b : plan_.blocks) {
      const auto& last = p_.code[(size_t)(b.end - 1)];
      const bool jz = last.code == Program::JZ;
      const bool jmp = last.code == Program::JMP;
      if (jz || jmp) b.taken = block_of[(size_t)last.dst];
      b.fall = jmp ? -1 : block_of[(size_t)b.end];
    }
  }

  void build_windows() {
    plan_.calls.assign(p_.calls.size(), {});
    for (const auto& I : p_.code)
      if (I.code == Program::CALL) forward_window(I.a);
    for (const auto& A : p_.adj.code)
      if (A.code == Program::CALL) backward_window(A.a);
    for (const auto& w : plan_.calls)
      plan_.max_window = std::max({plan_.max_window, w.fwd_size, w.bwd_size});
    for (size_t i = 0; i < p_.calls.size(); ++i) {
      const Program::Call& call = p_.calls[i];
      auto& w = plan_.calls[i];
      for (int k = 0; k < call.n_in; ++k) {
        w.in_reg[k] = reg_offset(call.in[k]);
        w.val_in_reg[k] = reg_offset(call.bwd_value_in[k]);
        w.adj_in_cell[k] = cell_offset(call.bwd_adj_in[k]);
      }
      w.out_reg = reg_offset(call.out);
      w.scratch_reg = reg_offset(call.scratch);
      w.val_out_reg = reg_offset(call.bwd_value_out);
      w.adj_out_cell = cell_offset(call.bwd_adj_out);
      if (tile_calls_ && tile_callable(call, w)) {
        w.tile = tile_call_kind(call.opcode);
        ++plan_.tile_calls;
      }
    }
  }

  bool tile_callable(const Program::Call& call,
                     const RegionMapLanePlan::CallWindow& w) const {
    if (tile_call_kind(call.opcode) == TileCallKind::None) return false;
    if (call.forward == nullptr || call.backward == nullptr) return false;
    if (call.n_in < 1 || call.n_in > 6 || call.out_len != 1) return false;
    if (call.scratch_len != call.n_in + 1) return false;
    if (call.variant & 0x40u) return false;
    if (!call.idata.empty() || call.udata_owner) return false;
    for (int k = 0; k < call.n_in; ++k)
      if (call.in_len[k] != 1) return false;
    if (w.out_reg < 0 || w.scratch_reg < 0 || w.adj_out_cell < 0) return false;
    for (int k = 0; k < call.n_in; ++k)
      if ((call.input_adjoint_mask & (1u << k)) && w.adj_in_cell[k] < 0 &&
          !shared_adjoint_cell(call.bwd_adj_in[k]))
        return false;
    return true;
  }

  bool shared_adjoint_cell(int c) const {
    return c >= 0 && (size_t)c < plan_.cell_slot.size() &&
           plan_.cell_slot[(size_t)c] < 0;
  }

  void forward_window(int index) {
    const Program::Call& call = p_.calls[(size_t)index];
    auto& w = plan_.calls[(size_t)index];
    w.fwd = call;
    std::vector<Span> written = {{call.out, call.out_len},
                                 {call.scratch, call.scratch_len}};
    std::vector<Span> read;
    for (int k = 0; k < call.n_in; ++k)
      read.push_back({call.in[k], call.in_len[k]});
    for (size_t i = 0; i < written.size(); ++i) {
      for (size_t j = i + 1; j < written.size(); ++j)
        if (overlaps(written[i], written[j]))
          refuse("call output ranges overlap");
      for (const auto& r : read)
        if (overlaps(written[i], r)) refuse("call reads what it writes");
    }
    int at = 0;
    for (int k = 0; k < call.n_in; ++k) {
      w.in_off[k] = at;
      w.fwd.in[k] = at;
      at += call.in_len[k];
    }
    w.out_off = at;
    w.fwd.out = at;
    at += call.out_len;
    w.scratch_off = at;
    w.fwd.scratch = at;
    at += call.scratch_len;
    w.fwd_size = at;
  }

  void backward_window(int index) {
    const Program::Call& call = p_.calls[(size_t)index];
    auto& w = plan_.calls[(size_t)index];
    std::vector<Span> adj_spans;
    for (int k = 0; k < call.n_in; ++k)
      if (call.input_adjoint_mask & (1u << k))
        adj_spans.push_back({call.bwd_adj_in[k], call.in_len[k]});
    adj_spans.push_back({call.bwd_adj_out, call.out_len});
    for (size_t i = 0; i < adj_spans.size(); ++i)
      for (size_t j = i + 1; j < adj_spans.size(); ++j)
        if (overlaps(adj_spans[i], adj_spans[j]))
          refuse("call adjoint ranges overlap");
    const Span scratch{call.scratch, call.scratch_len};
    for (int k = 0; k < call.n_in; ++k)
      if (overlaps(scratch, {call.bwd_value_in[k], call.in_len[k]}))
        refuse("call scratch overlaps a value range");
    if (overlaps(scratch, {call.bwd_value_out, call.out_len}))
      refuse("call scratch overlaps a value range");
    int at = 0;
    for (int k = 0; k < call.n_in; ++k) {
      w.val_in_off[k] = at;
      at += call.in_len[k];
    }
    w.val_out_off = at;
    at += call.out_len;
    w.bwd_scratch_off = at;
    at += call.scratch_len;
    for (int k = 0; k < call.n_in; ++k) {
      w.adj_in_off[k] = at;
      if (call.input_adjoint_mask & (1u << k)) at += call.in_len[k];
    }
    w.adj_out_off = at;
    at += call.out_len;
    w.bwd_size = at;
  }

  void count_sites() {
    const auto operands = [&](const AdjInstr& A) -> std::vector<int> {
      switch (A.code) {
        case Program::MOV:
        case Program::NEG:
        case Program::EXP:
        case Program::LOG:
        case Program::LOG1P_EXP:
        case Program::SQRT:
        case Program::SQUARE:
        case Program::INV:
        case Program::FABS:
          return {A.a};
        case Program::ADD:
        case Program::SUB:
        case Program::MUL:
        case Program::DIV:
        case Program::POW:
        case Program::FMAX:
        case Program::FMIN:
        case Program::LSE2:
        case Program::LOG_DIFF_EXP:
          return {A.a, A.b};
        case Program::FMA:
        case Program::LOG_MIX:
          return {A.a, A.b, A.c};
        default:
          return {};
      }
    };
    for (const auto& A : p_.adj.code) {
      if (A.code == Program::DYN_INDEX) {
        if (shared_cell(A.a)) {
          ++plan_.max_sites;
          ++plan_.max_dynamic;
        }
      } else if (A.code == Program::CALL) {
        const Program::Call& call = p_.calls[(size_t)A.a];
        for (int k = 0; k < call.n_in; ++k)
          if ((call.input_adjoint_mask & (1u << k)) &&
              shared_cell(call.bwd_adj_in[k]))
            plan_.max_sites += call.in_len[k];
      } else {
        for (int c : operands(A))
          if (shared_cell(c)) ++plan_.max_sites;
      }
    }
  }

  void size_tiles() {
    plan_.tiles = (int)((p_.count + kRegionMapTile - 1) / kRegionMapTile);
    const int64_t adjoint = (int64_t)kRegionMapTile * plan_.adj_cells;
    const int64_t one_tile = (int64_t)kRegionMapTile * plan_.fwd_regs + adjoint;
    const int64_t all_tiles =
        (int64_t)plan_.tiles * kRegionMapTile * plan_.fwd_regs + adjoint;
    plan_.tile_recompute =
        plan_.fwd_regs > recompute_cells_ || all_tiles > limit_;
    plan_.storage = plan_.tile_recompute ? one_tile : all_tiles;
    if (plan_.storage > limit_) refuse("tile storage exceeds the save limit");
    plan_.mask_cells =
        (int64_t)plan_.segment_count * (plan_.tile_recompute ? 1 : plan_.tiles);
  }
};

using Mask = uint64_t;
constexpr int kTile = kRegionMapTile;

class TileState {
 public:
  TileState(const RegionMapProg& p, double* reg, double* fwd, int lanes)
      : plan_(p.lanes),
        reg_(reg),
        fwd_(fwd),
        n_(lanes),
        full_(lanes == kTile ? ~Mask{0} : (Mask{1} << lanes) - 1) {}

  Mask full() const { return full_; }
  const double* base() const { return fwd_; }
  double* mutable_base() const { return fwd_; }
  const double* shared_regs() const { return reg_; }

  double* slot(int r) const {
    return fwd_ + (size_t)plan_.reg_slot[(size_t)r] * kTile;
  }
  bool per_lane(int r) const { return plan_.reg_slot[(size_t)r] >= 0; }

  double value(int r, int l) const {
    return per_lane(r) ? slot(r)[l] : reg_[r];
  }

  struct Lane {
    const double* p;
    double operator[](int l) const { return p[l]; }
  };
  struct Shared {
    double v;
    double operator[](int) const { return v; }
  };

  template <typename F>
  void with(int32_t off, int r, F&& f) const {
    if (off >= 0)
      f(Lane{fwd_ + off});
    else
      f(Shared{reg_[r]});
  }

  template <typename F>
  void with(int32_t off_a, int a, int32_t off_b, int b, F&& f) const {
    with(off_a, a, [&](auto A) { with(off_b, b, [&](auto B) { f(A, B); }); });
  }

  template <typename F>
  void with(int32_t off_a, int a, int32_t off_b, int b, int32_t off_c, int c,
            F&& f) const {
    with(off_a, a, off_b, b,
         [&](auto A, auto B) { with(off_c, c, [&](auto C) { f(A, B, C); }); });
  }

  int lanes() const { return n_; }

  template <typename F>
  void each(Mask m, F&& f) const {
    if (m == full_) {
      for (int l = 0; l < n_; ++l) f(l);
    } else {
      for (; m; m &= m - 1) f(__builtin_ctzll(m));
    }
  }

  template <typename F>
  void binary(const RegionMapLanePlan::Operands& o, const Program::Instr& I,
              Mask m, F f) const {
    double* d = fwd_ + o.dst;
    with(o.a, I.a, o.b, I.b, [&](auto A, auto B) {
      if constexpr (std::is_same_v<decltype(A), Shared> &&
                    std::is_same_v<decltype(B), Shared>) {
        const double v = f(A.v, B.v);
        each(m, [&](int l) { d[l] = v; });
      } else {
        each(m, [&](int l) { d[l] = f(A[l], B[l]); });
      }
    });
  }

  template <typename F>
  void unary(const RegionMapLanePlan::Operands& o, const Program::Instr& I,
             Mask m, F f) const {
    double* d = fwd_ + o.dst;
    with(o.a, I.a, [&](auto A) {
      if constexpr (std::is_same_v<decltype(A), Shared>) {
        const double v = f(A.v);
        each(m, [&](int l) { d[l] = v; });
      } else {
        each(m, [&](int l) { d[l] = f(A[l]); });
      }
    });
  }

  template <typename F>
  void ternary(const RegionMapLanePlan::Operands& o, const Program::Instr& I,
               Mask m, F f) const {
    double* d = fwd_ + o.dst;
    with(o.a, I.a, o.b, I.b, o.c, I.c, [&](auto A, auto B, auto C) {
      each(m, [&](int l) { d[l] = f(A[l], B[l], C[l]); });
    });
  }

 private:
  const RegionMapLanePlan& plan_;
  double* reg_;
  double* fwd_;
  int n_;
  Mask full_;
};

std::atomic<uint64_t> tile_call_runs{0};

constexpr int kTileCallColumns = 20;

bool tile_call_forward(const RegionMapLanePlan::CallWindow& w,
                       const Program::Call& call, const TileState& t, Mask m,
                       KernelCtx& ctx) {
  const int n_in = call.n_in;
  const int n_scratch = n_in + 1;
  const double* const base = t.base();
  double* const rows = t.mutable_base();
  ctx.variant = tile_call_variant(w.tile, call.variant, n_in);
  if (t.lanes() == kTile && m == t.full()) {
    for (int k = 0; k < n_in; ++k)
      ctx.in[k] =
          w.in_reg[k] >= 0
              ? Desc{rows + w.in_reg[k], (int64_t)kTile}
              : Desc{const_cast<double*>(t.shared_regs() + call.in[k]), 1};
    ctx.out = Desc{rows + w.out_reg, (int64_t)kTile};
    ctx.scratch = rows + w.scratch_reg;
    try {
      call.forward(ctx);
    } catch (...) {
      return false;
    }
    tile_call_runs.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  int lane[kTile];
  int n = 0;
  for (Mask r = m; r; r &= r - 1) lane[n++] = __builtin_ctzll(r);
  double buf[kTileCallColumns * kTile];
  double* at = buf;
  for (int k = 0; k < n_in; ++k) {
    if (w.in_reg[k] >= 0) {
      const double* src = base + w.in_reg[k];
      for (int i = 0; i < n; ++i) at[i] = src[lane[i]];
      ctx.in[k] = Desc{at, (int64_t)n};
      at += n;
    } else {
      ctx.in[k] = Desc{const_cast<double*>(t.shared_regs() + call.in[k]), 1};
    }
  }
  double* const out = at;
  ctx.out = Desc{out, (int64_t)n};
  at += n;
  double* const scratch = at;
  for (int c = 0; c < n_scratch; ++c) {
    const double* src = base + w.scratch_reg + (size_t)c * kTile;
    for (int i = 0; i < n; ++i) scratch[(size_t)c * n + i] = src[lane[i]];
  }
  ctx.scratch = scratch;
  try {
    call.forward(ctx);
  } catch (...) {
    return false;
  }
  double* const out_row = rows + w.out_reg;
  for (int i = 0; i < n; ++i) out_row[lane[i]] = out[i];
  for (int c = 0; c < n_scratch; ++c) {
    double* dst = rows + w.scratch_reg + (size_t)c * kTile;
    for (int i = 0; i < n; ++i) dst[lane[i]] = scratch[(size_t)c * n + i];
  }
  tile_call_runs.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void lane_instruction(const RegionMapProg& p, const TileState& t, int pc,
                      Mask m, double* window, KernelCtx& call_ctx,
                      EvalState* state) {
  const Program::Instr& I = p.code[(size_t)pc];
  const auto& o = p.lanes.ops[(size_t)pc];
  switch (I.code) {
    case Program::CONST: {
      const double v = p.pool[(size_t)I.a];
      double* d = t.slot(I.dst);
      t.each(m, [&](int l) { d[l] = v; });
      break;
    }
    case Program::FILL: {
      const double v = p.pool[(size_t)I.a];
      for (int32_t i = 0; i < I.len; ++i) {
        double* d = t.slot(I.dst + i);
        t.each(m, [&](int l) { d[l] = v; });
      }
      break;
    }
    case Program::CONSTR:
      for (int32_t i = 0; i < I.len; ++i) {
        const double v = p.pool[(size_t)(I.a + i)];
        double* d = t.slot(I.dst + i);
        t.each(m, [&](int l) { d[l] = v; });
      }
      break;
    case Program::MOV:
      t.unary(o, I, m, [](double a) { return a; });
      break;
    case Program::ADD:
      t.binary(o, I, m, [](double a, double b) { return a + b; });
      break;
    case Program::SUB:
      t.binary(o, I, m, [](double a, double b) { return a - b; });
      break;
    case Program::MUL:
      t.binary(o, I, m, [](double a, double b) { return a * b; });
      break;
    case Program::DIV:
      t.binary(o, I, m, [](double a, double b) { return a / b; });
      break;
    case Program::IADD:
      t.binary(o, I, m, [](double a, double b) {
        return static_cast<double>(static_cast<int>(a) + static_cast<int>(b));
      });
      break;
    case Program::INEG:
      t.unary(o, I, m, [](double a) {
        return static_cast<double>(-static_cast<int>(a));
      });
      break;
    case Program::IMOD:
      t.binary(o, I, m, [](double a, double b) {
        return static_cast<double>(
            stan::math::modulus(static_cast<int>(a), static_cast<int>(b)));
      });
      break;
    case Program::POW: {
      const uint8_t law = static_cast<uint8_t>(I.len);
      t.binary(o, I, m, [law](double a, double b) {
        return program_pow<double>(law, a, b);
      });
      break;
    }
    case Program::FMAX:
    case Program::FMIN: {
      const uint8_t law = static_cast<uint8_t>(I.len);
      const bool maximum = I.code == Program::FMAX;
      t.binary(o, I, m, [law, maximum](double a, double b) {
        return program_extremum<double>(maximum, law, a, b);
      });
      break;
    }
    case Program::NEG:
      t.unary(o, I, m, [](double a) { return -a; });
      break;
    case Program::EXP:
      t.unary(o, I, m, [](double a) { return stan::math::exp(a); });
      break;
    case Program::LOG:
      t.unary(o, I, m, [](double a) { return stan::math::log(a); });
      break;
    case Program::LOG1P_EXP:
      t.unary(o, I, m, [](double a) { return stan::math::log1p_exp(a); });
      break;
    case Program::SQRT:
      t.unary(o, I, m, [](double a) { return stan::math::sqrt(a); });
      break;
    case Program::SQUARE:
      t.unary(o, I, m, [](double a) { return stan::math::square(a); });
      break;
    case Program::INV:
      t.unary(o, I, m, [](double a) { return stan::math::inv(a); });
      break;
    case Program::FABS:
      t.unary(o, I, m, [](double a) { return stan::math::fabs(a); });
      break;
    case Program::GT:
      t.binary(o, I, m, [](double a, double b) { return double(a > b); });
      break;
    case Program::GE:
      t.binary(o, I, m, [](double a, double b) { return double(a >= b); });
      break;
    case Program::LT:
      t.binary(o, I, m, [](double a, double b) { return double(a < b); });
      break;
    case Program::LE:
      t.binary(o, I, m, [](double a, double b) { return double(a <= b); });
      break;
    case Program::EQ:
      t.binary(o, I, m, [](double a, double b) { return double(a == b); });
      break;
    case Program::LSE2:
      t.binary(o, I, m, [](double a, double b) {
        return stan::math::log_sum_exp(a, b);
      });
      break;
    case Program::LOG_DIFF_EXP:
      t.binary(o, I, m, [](double a, double b) {
        return stan::math::log_diff_exp(a, b);
      });
      break;
    case Program::FMA:
      t.ternary(o, I, m, [](double a, double b, double c) {
        return stan::math::fma(a, b, c);
      });
      break;
    case Program::LOG_MIX:
      t.ternary(o, I, m, [](double a, double b, double c) {
        return stan::math::log_mix(a, b, c);
      });
      break;
    case Program::DYN_INDEX: {
      double* d = t.slot(I.dst);
      const int first = I.a + I.c;
      const double len = static_cast<double>(I.len);
      t.with(o.b, I.b, [&](auto idx) {
        const auto pick = [&](auto read) {
          t.each(m, [&](int l) {
            const double raw = idx[l];
            if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < 1.0 ||
                raw > len)
              throw std::out_of_range("register-program index out of range");
            d[l] = read(static_cast<int32_t>(raw) - 1, l);
          });
        };
        if (o.a >= 0) {
          const double* base = t.base() + o.a;
          pick([&](int32_t k, int l) { return base[(size_t)k * kTile + l]; });
        } else {
          const double* base = t.shared_regs() + first;
          pick([&](int32_t k, int) { return base[k]; });
        }
      });
      break;
    }
    case Program::CALL: {
      const auto& w = p.lanes.calls[(size_t)I.a];
      const Program::Call& call = p.calls[(size_t)I.a];
      if (call.forward == nullptr)
        throw std::logic_error("unbound Program::CALL forward");
      bind_call_fwd_ctx(w.fwd, window, call_ctx, state);
      if (w.tile != TileCallKind::None) {
        if (tile_call_forward(w, call, t, m, call_ctx)) break;
        bind_call_fwd_ctx(w.fwd, window, call_ctx, state);
      }
      const double* const base = t.base();
      for (int k = 0; k < call.n_in; ++k)
        if (w.in_reg[k] < 0)
          std::copy_n(t.shared_regs() + call.in[k], call.in_len[k],
                      window + w.in_off[k]);
      const auto in_regs = [&](int l) {
        for (int k = 0; k < call.n_in; ++k)
          if (w.in_reg[k] >= 0)
            for (int j = 0; j < call.in_len[k]; ++j)
              window[w.in_off[k] + j] = base[w.in_reg[k] + j * kTile + l];
      };
      t.each(m, [&](int l) {
        in_regs(l);
        for (int j = 0; j < call.out_len; ++j)
          window[w.out_off + j] = base[w.out_reg + j * kTile + l];
        for (int j = 0; j < call.scratch_len; ++j)
          window[w.scratch_off + j] = base[w.scratch_reg + j * kTile + l];
        call.forward(call_ctx);
        double* const out = t.mutable_base();
        for (int j = 0; j < call.out_len; ++j)
          out[w.out_reg + j * kTile + l] = window[w.out_off + j];
        for (int j = 0; j < call.scratch_len; ++j)
          out[w.scratch_reg + j * kTile + l] = window[w.scratch_off + j];
      });
      break;
    }
    default:
      throw std::logic_error("opcode outside the lane whitelist");
  }
}

int popcount(Mask m) { return __builtin_popcountll(m); }

void tally_mask(Mask m, Mask full, uint64_t& n_full, uint64_t& n_partial,
                uint64_t& n_empty) {
  if (!m)
    ++n_empty;
  else if (m == full)
    ++n_full;
  else
    ++n_partial;
}

void tally_block(const RegionMapProg& p, const RegionMapLanePlan::Block& block,
                 Mask m, RegionMapTally& tally) {
  const int lanes = popcount(m);
  for (int pc = block.begin; pc < block.end; ++pc) {
    const auto& I = p.code[(size_t)pc];
    if (I.code == Program::JMP || p.lanes.flag_store[(size_t)pc]) continue;
    const size_t op = (size_t)I.code;
    ++tally.fwd_exec;
    tally.fwd_lanes += (uint64_t)lanes;
    ++tally.fwd_op[op];
    tally.fwd_op_lanes[op] += (uint64_t)lanes;
    if ((I.code == Program::CONST || I.code == Program::CONSTR ||
         I.code == Program::FILL) &&
        p.lanes.guard_reg[(size_t)I.dst])
      ++tally.fwd_flag_stores;
    if (p.lanes.invariant[(size_t)pc]) {
      ++tally.fwd_invariant;
      ++tally.fwd_op_invariant[op];
    }
  }
}

void lane_tile_forward(const RegionMapProg& p, const TileState& t,
                       double* window, KernelCtx& call_ctx, EvalState* state,
                       std::vector<Mask>& entry, double* seg_masks,
                       RegionMapTally* tally) {
  const auto& blocks = p.lanes.blocks;
  entry.assign(blocks.size() + 1, 0);
  entry[0] = t.full();
  for (size_t b = 0; b < blocks.size(); ++b) {
    const Mask m = entry[b];
    if (tally) {
      tally_mask(m, t.full(), tally->block_full, tally->block_partial,
                 tally->block_empty);
      if (m) tally_block(p, blocks[b], m, *tally);
    }
    if (!m) continue;
    const auto& block = blocks[b];
    const int last = block.end - 1;
    const auto& tail = p.code[(size_t)last];
    const bool jump = tail.code == Program::JZ || tail.code == Program::JMP;
    for (int pc = block.begin; pc < (jump ? last : block.end); ++pc)
      if (!p.lanes.flag_store[(size_t)pc])
        lane_instruction(p, t, pc, m, window, call_ctx, state);
    if (tail.code == Program::JMP) {
      entry[(size_t)block.taken] |= m;
    } else if (tail.code == Program::JZ) {
      Mask taken = 0;
      t.with(p.lanes.ops[(size_t)last].a, tail.a, [&](auto flag) {
        t.each(m, [&](int l) {
          if (flag[l] == 0.0) taken |= Mask{1} << l;
        });
      });
      entry[(size_t)block.taken] |= taken;
      entry[(size_t)block.fall] |= m & ~taken;
    } else {
      entry[(size_t)block.fall] |= m;
    }
  }
  const auto& segment_blocks = p.lanes.segment_blocks;
  for (size_t s = 0; s < segment_blocks.size(); ++s) {
    Mask m = 0;
    for (int b : segment_blocks[s]) m |= entry[(size_t)b];
    std::memcpy(seg_masks + s, &m, sizeof m);
  }
}

class AdjointTile {
 public:
  AdjointTile(const RegionMapProg& p, const TileState& fwd, double* cells,
              double* shared, double* log)
      : plan_(p.lanes),
        fwd_(fwd),
        cells_(cells),
        shared_(shared),
        meta_(log),
        rows_(log + (size_t)2 * p.lanes.max_sites),
        idx_rows_(rows_ + (size_t)p.lanes.max_sites * kTile) {}

  bool shared_cell(int c) const { return plan_.cell_slot[(size_t)c] < 0; }

  double* cells() const { return cells_; }

  double* cell(int c) const {
    return cells_ + (size_t)plan_.cell_slot[(size_t)c] * kTile;
  }

  double* acc(int c, int32_t off) {
    if (off >= 0) return cells_ + off;
    return open_row(c);
  }

  double* acc_rows(int base, int len) {
    double* first = nullptr;
    for (int j = 0; j < len; ++j) {
      double* row = open_row(base + j);
      if (!first) first = row;
    }
    return first;
  }

  struct Dynamic {
    double* value;
    double* index;
  };

  Dynamic acc_dynamic() {
    if (n_sites_ >= plan_.max_sites || n_dynamic_ >= plan_.max_dynamic)
      throw std::logic_error("lane adjoint site capacity exceeded");
    Dynamic d{rows_ + (size_t)n_sites_ * kTile,
              idx_rows_ + (size_t)n_dynamic_ * kTile};
    std::fill_n(d.value, fwd_.lanes(), 0.0);
    std::fill_n(d.index, fwd_.lanes(), -1.0);
    meta_[2 * n_sites_] = -1.0;
    meta_[2 * n_sites_ + 1] = n_dynamic_;
    ++n_sites_;
    ++n_dynamic_;
    return d;
  }

  void replay() {
    for (int l = fwd_.lanes(); l-- > 0;) {
      for (int s = 0; s < n_sites_; ++s) {
        const double* row = rows_ + (size_t)s * kTile;
        if (meta_[2 * s] >= 0.0) {
          shared_[(size_t)meta_[2 * s]] += row[l];
        } else {
          const double idx = idx_rows_[(size_t)meta_[2 * s + 1] * kTile + l];
          if (idx >= 0.0) shared_[(size_t)idx] += row[l];
        }
      }
    }
  }

  const TileState& forward() const { return fwd_; }

  template <typename F>
  void with(int32_t off, int r, F&& f) const {
    fwd_.with(off, r, f);
  }
  template <typename F>
  void with(int32_t oa, int a, int32_t ob, int b, F&& f) const {
    fwd_.with(oa, a, ob, b, f);
  }
  template <typename F>
  void with(int32_t oa, int a, int32_t ob, int b, int32_t oc, int c,
            F&& f) const {
    fwd_.with(oa, a, ob, b, oc, c, f);
  }

  template <typename F>
  void each(Mask m, F&& f) const {
    if (m == fwd_.full()) {
      for (int l = fwd_.lanes(); l-- > 0;) f(l);
    } else {
      while (m) {
        const int l = 63 - __builtin_clzll(m);
        f(l);
        m &= ~(Mask{1} << l);
      }
    }
  }

 private:
  double* open_row(int c) {
    if (n_sites_ >= plan_.max_sites)
      throw std::logic_error("lane adjoint site capacity exceeded");
    double* row = rows_ + (size_t)n_sites_ * kTile;
    std::fill_n(row, fwd_.lanes(), 0.0);
    meta_[2 * n_sites_] = c;
    meta_[2 * n_sites_ + 1] = -1.0;
    ++n_sites_;
    return row;
  }

  const RegionMapLanePlan& plan_;
  const TileState& fwd_;
  double* cells_;
  double* shared_;
  double* meta_;
  double* rows_;
  double* idx_rows_;
  int n_sites_ = 0;
  int n_dynamic_ = 0;
};

void tile_call_backward(const RegionMapLanePlan::CallWindow& w,
                        const Program::Call& call, const AdjointTile& t, Mask m,
                        double* const rows[6], KernelCtx& ctx) {
  const TileState& f = t.forward();
  const int n_in = call.n_in;
  const int n_scratch = n_in + 1;
  const double* const fbase = f.base();
  double* const cbase = t.cells();
  ctx.n_in = n_in;
  ctx.variant = tile_call_variant(w.tile, call.variant, n_in);
  ctx.idata = call.idata.data();
  ctx.n_idata = (int64_t)call.idata.size();
  ctx.udata = call.udata_owner.get();
  ctx.out_adj = 0.0;
  const auto wants = [&](int k) {
    return (call.input_adjoint_mask & (1u << k)) != 0;
  };
  if (f.lanes() == kTile && m == f.full()) {
    for (int k = 0; k < n_in; ++k) {
      ctx.in[k] = w.val_in_reg[k] >= 0
                      ? Desc{f.mutable_base() + w.val_in_reg[k], (int64_t)kTile}
                      : Desc{const_cast<double*>(f.shared_regs() +
                                                 call.bwd_value_in[k]),
                             1};
      ctx.in_adj[k] = !wants(k)
                          ? Desc{nullptr, (int64_t)kTile}
                          : Desc{rows[k] ? rows[k] : cbase + w.adj_in_cell[k],
                                 (int64_t)kTile};
    }
    double* const out_adj = cbase + w.adj_out_cell;
    ctx.out = Desc{out_adj, (int64_t)kTile};
    ctx.out_adj_vec = Desc{out_adj, (int64_t)kTile};
    ctx.scratch = f.mutable_base() + w.scratch_reg;
    call.backward(ctx);
    std::fill_n(out_adj, kTile, 0.0);
    return;
  }
  int lane[kTile];
  int n = 0;
  for (Mask r = m; r; r &= r - 1) lane[n++] = __builtin_ctzll(r);
  double buf[kTileCallColumns * kTile];
  double* at = buf;
  for (int k = 0; k < n_in; ++k) {
    if (w.val_in_reg[k] >= 0) {
      const double* src = fbase + w.val_in_reg[k];
      for (int i = 0; i < n; ++i) at[i] = src[lane[i]];
      ctx.in[k] = Desc{at, (int64_t)n};
      at += n;
    } else {
      ctx.in[k] =
          Desc{const_cast<double*>(f.shared_regs() + call.bwd_value_in[k]), 1};
    }
  }
  double* adj[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  for (int k = 0; k < n_in; ++k) {
    if (!wants(k)) {
      ctx.in_adj[k] = Desc{nullptr, (int64_t)n};
      continue;
    }
    adj[k] = at;
    at += n;
    if (rows[k]) {
      std::fill_n(adj[k], n, 0.0);
    } else {
      const double* src = cbase + w.adj_in_cell[k];
      for (int i = 0; i < n; ++i) adj[k][i] = src[lane[i]];
    }
    ctx.in_adj[k] = Desc{adj[k], (int64_t)n};
  }
  double* const out_adj = at;
  at += n;
  for (int i = 0; i < n; ++i) out_adj[i] = cbase[w.adj_out_cell + lane[i]];
  ctx.out = Desc{out_adj, (int64_t)n};
  ctx.out_adj_vec = Desc{out_adj, (int64_t)n};
  double* const scratch = at;
  for (int c = 0; c < n_scratch; ++c) {
    const double* src = fbase + w.scratch_reg + (size_t)c * kTile;
    for (int i = 0; i < n; ++i) scratch[(size_t)c * n + i] = src[lane[i]];
  }
  ctx.scratch = scratch;
  call.backward(ctx);
  for (int k = 0; k < n_in; ++k) {
    if (!adj[k]) continue;
    double* const dst = rows[k] ? rows[k] : cbase + w.adj_in_cell[k];
    for (int i = 0; i < n; ++i) dst[lane[i]] = adj[k][i];
  }
  for (int i = 0; i < n; ++i) cbase[w.adj_out_cell + lane[i]] = 0.0;
}

void lane_adjoint_instruction(const RegionMapProg& p, AdjointTile& t, int pc,
                              Mask m, double* window, KernelCtx& ctx) {
  const AdjInstr& I = p.adj.code[(size_t)pc];
  const auto& o = p.lanes.adj_ops[(size_t)pc];
  const auto cells = [&](int32_t off) { return t.cells() + off; };
  switch (I.code) {
    case Program::CONST:
    case Program::GT:
    case Program::GE:
    case Program::LT:
    case Program::LE:
    case Program::EQ:
    case Program::IADD:
    case Program::IMOD:
    case Program::INEG: {
      double* const d = cells(o.dst);
      t.each(m, [&](int l) { d[l] = 0.0; });
      return;
    }
    case Program::FILL:
    case Program::CONSTR:
      for (int32_t k = 0; k < I.len; ++k) {
        double* const d = t.cell(I.dst + k);
        t.each(m, [&](int l) { d[l] = 0.0; });
      }
      return;
    case Program::MOV: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u;
      });
      return;
    }
    case Program::ADD: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      double* const b = t.acc(I.b, o.b);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u;
        b[l] += u;
      });
      return;
    }
    case Program::SUB: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      double* const b = t.acc(I.b, o.b);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u;
        b[l] -= u;
      });
      return;
    }
    case Program::MUL: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      double* const b = t.acc(I.b, o.b);
      t.with(o.va, I.va, o.vb, I.vb, [&](auto va, auto vb) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          a[l] += vb[l] * u;
          b[l] += va[l] * u;
        });
      });
      return;
    }
    case Program::FMA: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      double* const b = t.acc(I.b, o.b);
      double* const c = t.acc(I.c, o.c);
      t.with(o.va, I.va, o.vb, I.vb, [&](auto va, auto vb) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          a[l] += vb[l] * u;
          b[l] += va[l] * u;
          c[l] += u;
        });
      });
      return;
    }
    case Program::DIV: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      double* const b = t.acc(I.b, o.b);
      const bool safe = static_cast<uint8_t>(I.len) == kDivSafeGrouping;
      t.with(o.va, I.va, o.vb, I.vb, o.vd, I.vd,
             [&](auto va, auto vb, auto vd) {
               t.each(m, [&](int l) {
                 const double u = d[l];
                 d[l] = 0.0;
                 double da, db;
                 if (safe)
                   div_partials(u, vb[l], vd[l], &da, &db);
                 else
                   div_partials_replay(u, va[l], vb[l], &da, &db);
                 a[l] += da;
                 b[l] += db;
               });
             });
      return;
    }
    case Program::POW: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      double* const b = t.acc(I.b, o.b);
      const uint8_t law = static_cast<uint8_t>(I.len);
      t.with(o.va, I.va, o.vb, I.vb, o.vd, I.vd,
             [&](auto va, auto vb, auto vd) {
               t.each(m, [&](int l) {
                 const double u = d[l];
                 d[l] = 0.0;
                 pow_rule(law, u, va[l], vb[l], vd[l], a[l], b[l]);
               });
             });
      return;
    }
    case Program::FMAX:
    case Program::FMIN: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      double* const b = t.acc(I.b, o.b);
      const uint8_t law = static_cast<uint8_t>(I.len);
      const bool maximum = I.code == Program::FMAX;
      t.with(o.va, I.va, o.vb, I.vb, [&](auto va, auto vb) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          extremum_rule(maximum, law, u, va[l], vb[l], a[l], b[l]);
        });
      });
      return;
    }
    case Program::NEG: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] -= u;
      });
      return;
    }
    case Program::EXP: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.with(o.vd, I.vd, [&](auto vd) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          a[l] += u * vd[l];
        });
      });
      return;
    }
    case Program::LOG: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.with(o.va, I.va, [&](auto va) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          a[l] += u / va[l];
        });
      });
      return;
    }
    case Program::LOG1P_EXP: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.with(o.va, I.va, [&](auto va) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          a[l] += u * stan::math::inv_logit(va[l]);
        });
      });
      return;
    }
    case Program::SQRT: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.with(o.vd, I.vd, [&](auto vd) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          if (vd[l] != 0.0) a[l] += u / (2.0 * vd[l]);
        });
      });
      return;
    }
    case Program::SQUARE: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.with(o.va, I.va, [&](auto va) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          a[l] += u * 2.0 * va[l];
        });
      });
      return;
    }
    case Program::INV: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.with(o.va, I.va, [&](auto va) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          a[l] -= u / (va[l] * va[l]);
        });
      });
      return;
    }
    case Program::FABS: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      t.with(o.va, I.va, [&](auto va) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          fabs_rule(u, va[l], a[l]);
        });
      });
      return;
    }
    case Program::LSE2:
    case Program::LOG_DIFF_EXP: {
      double* const d = cells(o.dst);
      double* const a = t.acc(I.a, o.a);
      double* const b = t.acc(I.b, o.b);
      const bool lse = I.code == Program::LSE2;
      t.with(o.va, I.va, o.vb, I.vb, [&](auto va, auto vb) {
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          if (lse)
            lse2_rule(u, va[l], vb[l], a[l], b[l]);
          else
            log_diff_exp_rule(u, va[l], vb[l], a[l], b[l]);
        });
      });
      return;
    }
    case Program::LOG_MIX: {
      double* const d = cells(o.dst);
      double* const c = t.acc(I.c, o.c);
      double* const b = t.acc(I.b, o.b);
      double* const a = t.acc(I.a, o.a);
      t.with(o.va, I.va, o.vb, I.vb, o.vc, I.vc,
             [&](auto va, auto vb, auto vc) {
               t.each(m, [&](int l) {
                 const double u = d[l];
                 d[l] = 0.0;
                 log_mix_rule(u, va[l], vb[l], vc[l], a[l], b[l], c[l]);
               });
             });
      return;
    }
    case Program::DYN_INDEX: {
      double* const d = cells(o.dst);
      t.with(o.vb, I.vb, [&](auto vb) {
        if (o.a < 0) {
          const auto site = t.acc_dynamic();
          t.each(m, [&](int l) {
            const double u = d[l];
            d[l] = 0.0;
            site.value[l] = u;
            site.index[l] =
                static_cast<double>(I.a + static_cast<int32_t>(vb[l]) - 1);
          });
          return;
        }
        double* const base = t.cells() + o.a;
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          base[(size_t)(static_cast<int32_t>(vb[l]) - 1) * kTile + l] += u;
        });
      });
      return;
    }
    case Program::CALL: {
      const Program::Call& call = p.calls[(size_t)I.a];
      const auto& w = p.lanes.calls[(size_t)I.a];
      const TileState& f = t.forward();
      double* rows[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
      for (int k = 0; k < call.n_in; ++k)
        if ((call.input_adjoint_mask & (1u << k)) &&
            t.shared_cell(call.bwd_adj_in[k]))
          rows[k] = t.acc_rows(call.bwd_adj_in[k], call.in_len[k]);
      if (w.tile != TileCallKind::None) {
        tile_call_backward(w, call, t, m, rows, ctx);
        return;
      }
      const double* const fbase = f.base();
      double* const cbase = t.cells();
      for (int k = 0; k < call.n_in; ++k) {
        if (w.val_in_reg[k] < 0)
          std::copy_n(f.shared_regs() + call.bwd_value_in[k], call.in_len[k],
                      window + w.val_in_off[k]);
      }
      if (w.val_out_reg < 0)
        std::copy_n(f.shared_regs() + call.bwd_value_out, call.out_len,
                    window + w.val_out_off);
      ctx.n_in = call.n_in;
      for (int k = 0; k < call.n_in; ++k) {
        ctx.in[k] = Desc{window + w.val_in_off[k], call.in_len[k]};
        ctx.in_adj[k] = (call.input_adjoint_mask & (uint8_t)(1u << k))
                            ? Desc{window + w.adj_in_off[k], call.in_len[k]}
                            : Desc{nullptr, call.in_len[k]};
      }
      ctx.out = Desc{window + w.val_out_off, call.out_len};
      ctx.out_adj_vec = Desc{window + w.adj_out_off, call.out_len};
      ctx.variant = call.variant;
      ctx.scratch = window + w.bwd_scratch_off;
      ctx.idata = call.idata.data();
      ctx.n_idata = (int64_t)call.idata.size();
      ctx.udata = call.udata_owner.get();
      t.each(m, [&](int l) {
        for (int k = 0; k < call.n_in; ++k) {
          if (w.val_in_reg[k] >= 0)
            for (int j = 0; j < call.in_len[k]; ++j)
              window[w.val_in_off[k] + j] =
                  fbase[w.val_in_reg[k] + j * kTile + l];
          if (call.input_adjoint_mask & (1u << k)) {
            if (rows[k])
              std::fill_n(window + w.adj_in_off[k], call.in_len[k], 0.0);
            else
              for (int j = 0; j < call.in_len[k]; ++j)
                window[w.adj_in_off[k] + j] =
                    cbase[w.adj_in_cell[k] + j * kTile + l];
          }
        }
        if (w.val_out_reg >= 0)
          for (int j = 0; j < call.out_len; ++j)
            window[w.val_out_off + j] = fbase[w.val_out_reg + j * kTile + l];
        for (int j = 0; j < call.scratch_len; ++j)
          window[w.bwd_scratch_off + j] = fbase[w.scratch_reg + j * kTile + l];
        for (int j = 0; j < call.out_len; ++j)
          window[w.adj_out_off + j] = cbase[w.adj_out_cell + j * kTile + l];
        ctx.out_adj = call.out_len == 1 ? window[w.adj_out_off] : 0.0;
        call.backward(ctx);
        for (int k = 0; k < call.n_in; ++k)
          if (call.input_adjoint_mask & (1u << k)) {
            if (rows[k])
              for (int j = 0; j < call.in_len[k]; ++j)
                rows[k][(size_t)j * kTile + l] = window[w.adj_in_off[k] + j];
            else
              for (int j = 0; j < call.in_len[k]; ++j)
                cbase[w.adj_in_cell[k] + j * kTile + l] =
                    window[w.adj_in_off[k] + j];
          }
        double* const fout = f.mutable_base();
        for (int j = 0; j < call.scratch_len; ++j)
          fout[w.scratch_reg + j * kTile + l] = window[w.bwd_scratch_off + j];
        for (int j = 0; j < call.out_len; ++j)
          cbase[w.adj_out_cell + j * kTile + l] = 0.0;
      });
      return;
    }
    default:
      throw std::logic_error("adjoint opcode outside the lane whitelist");
  }
}

std::atomic<uint64_t> lane_runs{0};

void merge_tally(RegionMapProfile& profile, const RegionMapTally& tally) {
  std::lock_guard<std::mutex> lock(profile.mu);
  profile.total.add(tally);
}

#ifndef STANLI_NO_STDIO
std::string percent(uint64_t part, uint64_t whole) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f%%",
                whole ? 100.0 * (double)part / (double)whole : 0.0);
  return buf;
}

std::string occupancy(uint64_t lanes, uint64_t exec) {
  return percent(lanes, exec * (uint64_t)kTile);
}

void print_profile(const RegionMapProfile& pr) {
  const RegionMapTally& t = pr.total;
  std::fprintf(stderr,
               "region_map_profile %s: iterations=%lld body=%d form=%s "
               "hoisted=%d evaluations=%llu tiles=%llu\n",
               pr.label.c_str(), (long long)pr.iterations, pr.body,
               pr.form.c_str(), pr.hoisted, (unsigned long long)t.evaluations,
               (unsigned long long)t.tiles);
  std::fprintf(stderr,
               "  forward: instrs=%llu lanes=%llu occupancy=%s "
               "shared-operand-only=%llu (%s)\n",
               (unsigned long long)t.fwd_exec, (unsigned long long)t.fwd_lanes,
               occupancy(t.fwd_lanes, t.fwd_exec).c_str(),
               (unsigned long long)t.fwd_invariant,
               percent(t.fwd_invariant, t.fwd_exec).c_str());
  std::fprintf(stderr, "  adjoint: instrs=%llu lanes=%llu occupancy=%s\n",
               (unsigned long long)t.adj_exec, (unsigned long long)t.adj_lanes,
               occupancy(t.adj_lanes, t.adj_exec).c_str());
  std::fprintf(
      stderr,
      "  block entries: full=%llu partial=%llu empty=%llu | "
      "segment entries: full=%llu partial=%llu empty=%llu\n",
      (unsigned long long)t.block_full, (unsigned long long)t.block_partial,
      (unsigned long long)t.block_empty, (unsigned long long)t.seg_full,
      (unsigned long long)t.seg_partial, (unsigned long long)t.seg_empty);
  std::vector<int> codes;
  for (int c = 0; c < kRegionMapProfileCodes; ++c)
    if (t.fwd_op[c] || t.adj_op[c]) codes.push_back(c);
  std::sort(codes.begin(), codes.end(), [&](int a, int b) {
    return t.fwd_op[a] + t.adj_op[a] > t.fwd_op[b] + t.adj_op[b];
  });
  std::fprintf(stderr, "  %-14s %12s %7s %12s | %12s %7s\n", "opcode", "fwd",
               "occ", "shared-only", "adj", "occ");
  for (int c : codes)
    std::fprintf(stderr, "  %-14s %12llu %7s %12llu | %12llu %7s\n",
                 (size_t)c < program_code_count() ? kProgramOpSpecs[c].name
                                                  : std::to_string(c).c_str(),
                 (unsigned long long)t.fwd_op[c],
                 occupancy(t.fwd_op_lanes[c], t.fwd_op[c]).c_str(),
                 (unsigned long long)t.fwd_op_invariant[c],
                 (unsigned long long)t.adj_op[c],
                 occupancy(t.adj_op_lanes[c], t.adj_op[c]).c_str());
}
#endif

struct ProfileRegistry {
  std::mutex mu;
  std::vector<std::shared_ptr<RegionMapProfile>> all;
  ~ProfileRegistry() {
    // Exit-time summaries cannot use a host callback whose lifetime may
    // already have ended. Omit their stderr output in no-stdio builds.
#ifndef STANLI_NO_STDIO
    for (const auto& p : all) print_profile(*p);
#endif
  }
};

ProfileRegistry& profile_registry() {
  static ProfileRegistry registry;
  return registry;
}

std::shared_ptr<RegionMapProfile> make_profile(const RegionMapProg& p) {
  auto profile = std::make_shared<RegionMapProfile>();
  ProfileRegistry& registry = profile_registry();
  std::lock_guard<std::mutex> lock(registry.mu);
  profile->label = "map#" + std::to_string(registry.all.size()) +
                   (p.label.empty() ? "" : " (" + p.label + ")");
  profile->form =
      p.lanes.tile_recompute ? "lanes-tile-recompute" : "lanes-save";
  profile->body = (int)p.code.size();
  profile->iterations = p.count;
  registry.all.push_back(profile);
  return profile;
}

}  // namespace

void RegionMapTally::add(const RegionMapTally& o) {
  evaluations += o.evaluations;
  tiles += o.tiles;
  fwd_exec += o.fwd_exec;
  fwd_lanes += o.fwd_lanes;
  fwd_invariant += o.fwd_invariant;
  fwd_flag_stores += o.fwd_flag_stores;
  adj_exec += o.adj_exec;
  adj_lanes += o.adj_lanes;
  block_full += o.block_full;
  block_partial += o.block_partial;
  block_empty += o.block_empty;
  seg_full += o.seg_full;
  seg_partial += o.seg_partial;
  seg_empty += o.seg_empty;
  for (int c = 0; c < kRegionMapProfileCodes; ++c) {
    fwd_op[c] += o.fwd_op[c];
    fwd_op_lanes[c] += o.fwd_op_lanes[c];
    fwd_op_invariant[c] += o.fwd_op_invariant[c];
    adj_op[c] += o.adj_op[c];
    adj_op_lanes[c] += o.adj_op_lanes[c];
  }
}

std::string region_map_call_summary(const RegionMapProg& p) {
  std::map<std::string, int> counts;
  for (const auto& call : p.calls) ++counts[stanli::opcode_name(call.opcode)];
  std::string out = " calls=";
  for (const auto& [name, n] : counts)
    out += name + ":" + std::to_string(n) + ",";
  return out;
}

uint64_t region_map_tile_call_runs() {
  return tile_call_runs.load(std::memory_order_relaxed);
}

uint64_t region_map_lane_runs() {
  return lane_runs.load(std::memory_order_relaxed);
}

int64_t region_map_lane_cells(const RegionMapProg& p) {
  if (!p.lanes.active) return 0;
  const auto& l = p.lanes;
  return l.storage + l.max_window +
         (int64_t)l.max_sites * (2 + kRegionMapTile) +
         (int64_t)l.max_dynamic * kRegionMapTile + l.mask_cells;
}

static double* segment_masks(const RegionMapLanePlan& plan, double* region,
                             int tile) {
  double* base = region + plan.storage + plan.max_window +
                 (int64_t)plan.max_sites * (2 + kRegionMapTile) +
                 (int64_t)plan.max_dynamic * kRegionMapTile;
  return plan.tile_recompute ? base : base + (size_t)tile * plan.segment_count;
}

static double* tile_base(const RegionMapLanePlan& plan, double* tiles,
                         int tile) {
  return plan.tile_recompute ? tiles
                             : tiles + (size_t)tile * kTile * plan.fwd_regs;
}

static TileState seeded_tile(const RegionMapProg& p, double* reg, double* base,
                             int64_t first, int lanes) {
  const auto& plan = p.lanes;
  for (const int32_t r : plan.seed_regs)
    std::fill_n(base + (size_t)plan.reg_slot[(size_t)r] * kTile, kTile, reg[r]);
  double* iter = base + (size_t)plan.reg_slot[(size_t)p.iter_reg] * kTile;
  for (int l = 0; l < lanes; ++l)
    iter[l] = static_cast<double>(p.lo + first + l);
  return TileState(p, reg, base, lanes);
}

void region_map_lanes_forward(const RegionMapProg& p, KernelCtx& ctx,
                              double* region) {
  lane_runs.fetch_add(1, std::memory_order_relaxed);
  const auto& plan = p.lanes;
  double* tiles = region;
  double* window = region + plan.storage;
  double* reg = ctx.scratch;
  KernelCtx call_ctx;
  std::vector<Mask> entry;
  entry.reserve(plan.blocks.size() + 1);
  double total = 0.0;
  const int target = p.out_regs[0];
  RegionMapTally tally;
  RegionMapTally* counting = plan.profile ? &tally : nullptr;
  for (int tile = 0; tile < plan.tiles; ++tile) {
    const int64_t first = (int64_t)tile * kTile;
    const int lanes = (int)std::min<int64_t>(kTile, p.count - first);
    double* base = tile_base(plan, tiles, tile);
    TileState t = seeded_tile(p, reg, base, first, lanes);
    lane_tile_forward(p, t, window, call_ctx, ctx.eval_state, entry,
                      segment_masks(plan, region, tile), counting);
    const double* out = t.slot(target);
    for (int l = 0; l < lanes; ++l) total += out[l];
  }
  if (counting) {
    tally.evaluations = 1;
    tally.tiles = (uint64_t)plan.tiles;
    merge_tally(*plan.profile, tally);
  }
  ctx.out.data[0] = total;
}

void region_map_lanes_backward(const RegionMapProg& p, KernelCtx& ctx,
                               double* region, double* adj) {
  const auto& plan = p.lanes;
  const auto& map = p.adj.adj_reg;
  const double seed = ctx.out_adj_vec.data[0];
  const int target = map[(size_t)p.out_regs[0]];
  double* fwd_tiles = region;
  double* adj_tile = region + (size_t)(plan.tile_recompute ? 1 : plan.tiles) *
                                  kTile * plan.fwd_regs;
  double* window = region + plan.storage;
  double* log = window + plan.max_window;
  KernelCtx call_ctx;
  std::vector<Mask> entry;
  entry.reserve(plan.blocks.size() + 1);
  const bool verify = std::getenv("STANLI_REGION_MAP_CHECK_CLEAN") != nullptr;
  std::vector<char> exempt;
  if (verify) exempt = region_map_clean_exempt_cells(p);
  std::fill_n(adj_tile, (size_t)plan.adj_cells * kTile, 0.0);
  RegionMapTally tally;
  RegionMapTally* counting = plan.profile ? &tally : nullptr;
  for (int tile = plan.tiles; tile-- > 0;) {
    const int64_t first = (int64_t)tile * kTile;
    const int lanes = (int)std::min<int64_t>(kTile, p.count - first);
    double* base = tile_base(plan, fwd_tiles, tile);
    TileState fwd = plan.tile_recompute
                        ? seeded_tile(p, ctx.scratch, base, first, lanes)
                        : TileState(p, ctx.scratch, base, lanes);
    if (plan.tile_recompute)
      lane_tile_forward(p, fwd, window, call_ctx, ctx.eval_state, entry,
                        segment_masks(plan, region, tile), counting);
    AdjointTile t(p, fwd, adj_tile, adj, log);
    const auto seed_cell = t.cell(target);
    for (int l = 0; l < lanes; ++l) seed_cell[l] += seed;
    const auto run = [&](int begin, int end, Mask m) {
      if (counting) {
        const int n = popcount(m);
        for (int pc = begin; pc < end; ++pc) {
          if (plan.adj_skip[(size_t)pc]) continue;
          const size_t op = (size_t)p.adj.code[(size_t)pc].code;
          ++counting->adj_exec;
          counting->adj_lanes += (uint64_t)n;
          ++counting->adj_op[op];
          counting->adj_op_lanes[op] += (uint64_t)n;
        }
      }
      for (int pc = begin; pc < end; ++pc)
        if (!plan.adj_skip[(size_t)pc])
          lane_adjoint_instruction(p, t, pc, m, window, call_ctx);
    };
    if (p.adj.segments.empty()) {
      run(0, (int)p.adj.code.size(), fwd.full());
    } else {
      const double* masks = segment_masks(plan, region, tile);
      for (size_t s = 0; s < p.adj.segments.size(); ++s) {
        const auto& seg = p.adj.segments[s];
        Mask m;
        std::memcpy(&m, masks + s, sizeof m);
        if (counting)
          tally_mask(m, fwd.full(), counting->seg_full, counting->seg_partial,
                     counting->seg_empty);
        if (m) run(seg.begin, seg.end, m);
      }
    }
    t.replay();
    if (verify) {
      const auto& cell_slot = plan.cell_slot;
      for (size_t cell = 0; cell < cell_slot.size(); ++cell) {
        if (cell_slot[cell] < 0 || exempt[cell] || plan.adj_residue[cell])
          continue;
        const double* row = adj_tile + (size_t)cell_slot[cell] * kTile;
        for (int l = 0; l < lanes; ++l)
          if (row[l] != 0.0)
            throw std::logic_error("region_map_check_clean: adjoint cell " +
                                   std::to_string(cell) + " holds " +
                                   std::to_string(row[l]) +
                                   " after the sweep of iteration " +
                                   std::to_string(p.lo + first + l));
      }
    }
  }
  if (counting) merge_tally(*plan.profile, tally);
}

void plan_region_map_lanes(RegionMapProg& p, bool enabled,
                           int64_t storage_limit) {
  p.lanes = RegionMapLanePlan{};
  if (!enabled) {
    p.lanes.refusal = "off";
    return;
  }
  if (p.recompute) {
    p.lanes.refusal = "recompute";
    return;
  }
  try {
    int64_t recompute_cells = std::numeric_limits<int64_t>::max();
    if (const char* v = std::getenv("STANLI_REGION_MAP_TILE_CELLS"))
      recompute_cells = std::strtoll(v, nullptr, 10);
    const char* tile_env = std::getenv("STANLI_REGION_MAP_TILE_CALLS");
    const bool tile_calls = !(tile_env && std::string_view(tile_env) == "0");
    Analysis(p, storage_limit, recompute_cells, tile_calls).run();
    p.lanes.active = true;
    if (std::getenv("STANLI_REGION_MAP_PROFILE"))
      p.lanes.profile = make_profile(p);
  } catch (const Refusal& r) {
    p.lanes = RegionMapLanePlan{};
    p.lanes.refusal = r.why;
  }
}

}  // namespace stanli
