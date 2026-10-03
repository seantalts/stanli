#include <stanli/adjoint.hpp>
#include <stanli/adjoint_rules.hpp>
#include <stanli/program_density.hpp>
#include <stanli/region_map.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <set>
#include <string>

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
  Analysis(RegionMapProg& p, int64_t storage_limit)
      : p_(p), plan_(p.lanes), limit_(storage_limit) {}

  void run() {
    check_opcodes();
    classify_registers();
    classify_cells();
    check_ranges();
    build_blocks();
    build_windows();
    count_sites();
    size_tiles();
  }

 private:
  RegionMapProg& p_;
  RegionMapLanePlan& plan_;
  int64_t limit_;
  int n_cells_ = 0;

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
  void each_body_write(F f) const {
    for (const auto& I : p_.code) {
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
  }

  void classify_registers() {
    const int n = p_.n_regs;
    std::vector<char> per_lane((size_t)n, 0);
    each_body_write([&](int reg, int len) {
      for (int k = 0; k < len; ++k) {
        if (reg + k < 0 || reg + k >= n) refuse("register out of range");
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
        refuse(std::string(what) + " range mixes shared and per-lane registers");
  }

  void same_class_cells(int base, int len, const char* what) const {
    if (len <= 0) return;
    if (base < 0 || base + len > n_cells_)
      refuse(std::string(what) + " cell range out of bounds");
    const bool s = shared_cell(base);
    for (int k = 1; k < len; ++k)
      if (shared_cell(base + k) != s)
        refuse(std::string(what) + " cell range mixes shared and per-lane cells");
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
  }

  void forward_window(int index) {
    const Program::Call& call = p_.calls[(size_t)index];
    auto& w = plan_.calls[(size_t)index];
    w.fwd = call;
    std::vector<Span> written = {{call.out, call.out_len},
                                 {call.scratch, call.scratch_len}};
    std::vector<Span> read;
    for (int k = 0; k < call.n_in; ++k) read.push_back({call.in[k], call.in_len[k]});
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
    plan_.tiles =
        (int)((p_.count + kRegionMapTile - 1) / kRegionMapTile);
    plan_.storage =
        (int64_t)plan_.tiles * kRegionMapTile * plan_.fwd_regs +
        (int64_t)kRegionMapTile * plan_.adj_cells;
    if (plan_.storage > limit_) refuse("tile storage exceeds the save limit");
  }
};


using Mask = uint64_t;
constexpr int kTile = kRegionMapTile;

class TileState {
 public:
  TileState(const RegionMapProg& p, double* reg, double* fwd, int lanes)
      : plan_(p.lanes), reg_(reg), fwd_(fwd), n_(lanes),
        full_(lanes == kTile ? ~Mask{0} : (Mask{1} << lanes) - 1) {}

  Mask full() const { return full_; }

  double* slot(int r) const {
    return fwd_ + (size_t)plan_.reg_slot[(size_t)r] * kTile;
  }
  bool per_lane(int r) const { return plan_.reg_slot[(size_t)r] >= 0; }

  const double* read(int r, double* broadcast) const {
    if (per_lane(r)) return slot(r);
    std::fill_n(broadcast, n_, reg_[r]);
    return broadcast;
  }

  double value(int r, int l) const {
    return per_lane(r) ? slot(r)[l] : reg_[r];
  }

  struct View {
    double* p;
    size_t stride;
    double& operator[](int l) const { return p[(size_t)l * stride]; }
  };

  View view(int r) const {
    return per_lane(r) ? View{slot(r), 1} : View{reg_ + r, 0};
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
  void binary(const Program::Instr& I, Mask m, F f) const {
    double b0[kTile], b1[kTile];
    const double* a = read(I.a, b0);
    const double* b = read(I.b, b1);
    double* d = slot(I.dst);
    each(m, [&](int l) { d[l] = f(a[l], b[l]); });
  }

  template <typename F>
  void unary(const Program::Instr& I, Mask m, F f) const {
    double b0[kTile];
    const double* a = read(I.a, b0);
    double* d = slot(I.dst);
    each(m, [&](int l) { d[l] = f(a[l]); });
  }

 private:
  const RegionMapLanePlan& plan_;
  double* reg_;
  double* fwd_;
  int n_;
  Mask full_;
};

void lane_instruction(const RegionMapProg& p, const TileState& t,
                      const Program::Instr& I, Mask m, double* window,
                      KernelCtx& call_ctx, EvalState* state) {
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
      t.unary(I, m, [](double a) { return a; });
      break;
    case Program::ADD:
      t.binary(I, m, [](double a, double b) { return a + b; });
      break;
    case Program::SUB:
      t.binary(I, m, [](double a, double b) { return a - b; });
      break;
    case Program::MUL:
      t.binary(I, m, [](double a, double b) { return a * b; });
      break;
    case Program::DIV:
      t.binary(I, m, [](double a, double b) { return a / b; });
      break;
    case Program::IADD:
      t.binary(I, m, [](double a, double b) {
        return static_cast<double>(static_cast<int>(a) + static_cast<int>(b));
      });
      break;
    case Program::INEG:
      t.unary(I, m, [](double a) {
        return static_cast<double>(-static_cast<int>(a));
      });
      break;
    case Program::IMOD:
      t.binary(I, m, [](double a, double b) {
        return static_cast<double>(stan::math::modulus(static_cast<int>(a),
                                                       static_cast<int>(b)));
      });
      break;
    case Program::POW: {
      const uint8_t law = static_cast<uint8_t>(I.len);
      t.binary(I, m, [law](double a, double b) {
        return program_pow<double>(law, a, b);
      });
      break;
    }
    case Program::FMAX:
    case Program::FMIN: {
      const uint8_t law = static_cast<uint8_t>(I.len);
      const bool maximum = I.code == Program::FMAX;
      t.binary(I, m, [law, maximum](double a, double b) {
        return program_extremum<double>(maximum, law, a, b);
      });
      break;
    }
    case Program::NEG:
      t.unary(I, m, [](double a) { return -a; });
      break;
    case Program::EXP:
      t.unary(I, m, [](double a) { return stan::math::exp(a); });
      break;
    case Program::LOG:
      t.unary(I, m, [](double a) { return stan::math::log(a); });
      break;
    case Program::SQRT:
      t.unary(I, m, [](double a) { return stan::math::sqrt(a); });
      break;
    case Program::SQUARE:
      t.unary(I, m, [](double a) { return stan::math::square(a); });
      break;
    case Program::INV:
      t.unary(I, m, [](double a) { return stan::math::inv(a); });
      break;
    case Program::FABS:
      t.unary(I, m, [](double a) { return stan::math::fabs(a); });
      break;
    case Program::GT:
      t.binary(I, m, [](double a, double b) { return double(a > b); });
      break;
    case Program::GE:
      t.binary(I, m, [](double a, double b) { return double(a >= b); });
      break;
    case Program::LT:
      t.binary(I, m, [](double a, double b) { return double(a < b); });
      break;
    case Program::LE:
      t.binary(I, m, [](double a, double b) { return double(a <= b); });
      break;
    case Program::EQ:
      t.binary(I, m, [](double a, double b) { return double(a == b); });
      break;
    case Program::LSE2:
      t.binary(I, m, [](double a, double b) {
        return stan::math::log_sum_exp(a, b);
      });
      break;
    case Program::LOG_DIFF_EXP:
      t.binary(I, m, [](double a, double b) {
        return stan::math::log_diff_exp(a, b);
      });
      break;
    case Program::FMA:
    case Program::LOG_MIX: {
      double b0[kTile], b1[kTile], b2[kTile];
      const double* a = t.read(I.a, b0);
      const double* b = t.read(I.b, b1);
      const double* c = t.read(I.c, b2);
      double* d = t.slot(I.dst);
      if (I.code == Program::FMA)
        t.each(m, [&](int l) { d[l] = stan::math::fma(a[l], b[l], c[l]); });
      else
        t.each(m, [&](int l) { d[l] = stan::math::log_mix(a[l], b[l], c[l]); });
      break;
    }
    case Program::DYN_INDEX: {
      double b1[kTile];
      const double* idx = t.read(I.b, b1);
      double* d = t.slot(I.dst);
      const int first = I.a + I.c;
      t.each(m, [&](int l) {
        const double raw = idx[l];
        if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < 1.0 ||
            raw > static_cast<double>(I.len))
          throw std::out_of_range("register-program index out of range");
        d[l] = t.value(first + static_cast<int32_t>(raw) - 1, l);
      });
      break;
    }
    case Program::CALL: {
      const auto& w = p.lanes.calls[(size_t)I.a];
      const Program::Call& call = p.calls[(size_t)I.a];
      t.each(m, [&](int l) {
        for (int k = 0; k < call.n_in; ++k)
          for (int j = 0; j < call.in_len[k]; ++j)
            window[w.in_off[k] + j] = t.value(call.in[k] + j, l);
        for (int j = 0; j < call.out_len; ++j)
          window[w.out_off + j] = t.value(call.out + j, l);
        for (int j = 0; j < call.scratch_len; ++j)
          window[w.scratch_off + j] = t.value(call.scratch + j, l);
        run_call(w.fwd, window, call_ctx, state);
        for (int j = 0; j < call.out_len; ++j)
          t.slot(call.out + j)[l] = window[w.out_off + j];
        for (int j = 0; j < call.scratch_len; ++j)
          t.slot(call.scratch + j)[l] = window[w.scratch_off + j];
      });
      break;
    }
    default:
      throw std::logic_error("opcode outside the lane whitelist");
  }
}

void lane_tile_forward(const RegionMapProg& p, const TileState& t,
                       double* window, KernelCtx& call_ctx,
                       EvalState* state) {
  const auto& blocks = p.lanes.blocks;
  std::vector<Mask> entry(blocks.size() + 1, 0);
  entry[0] = t.full();
  for (size_t b = 0; b < blocks.size(); ++b) {
    const Mask m = entry[b];
    if (!m) continue;
    const auto& block = blocks[b];
    const int last = block.end - 1;
    const auto& tail = p.code[(size_t)last];
    const bool jump = tail.code == Program::JZ || tail.code == Program::JMP;
    for (int pc = block.begin; pc < (jump ? last : block.end); ++pc)
      lane_instruction(p, t, p.code[(size_t)pc], m, window, call_ctx, state);
    if (tail.code == Program::JMP) {
      entry[(size_t)block.taken] |= m;
    } else if (tail.code == Program::JZ) {
      double b0[kTile];
      const double* flag = t.read(tail.a, b0);
      Mask taken = 0;
      t.each(m, [&](int l) {
        if (flag[l] == 0.0) taken |= Mask{1} << l;
      });
      entry[(size_t)block.taken] |= taken;
      entry[(size_t)block.fall] |= m & ~taken;
    } else {
      entry[(size_t)block.fall] |= m;
    }
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

  using Cell = TileState::View;

  bool shared_cell(int c) const { return plan_.cell_slot[(size_t)c] < 0; }

  Cell cell(int c) const {
    const int s = plan_.cell_slot[(size_t)c];
    return s >= 0 ? Cell{cells_ + (size_t)s * kTile, 1}
                  : Cell{shared_ + c, 0};
  }

  Cell acc(int c) {
    const int s = plan_.cell_slot[(size_t)c];
    if (s >= 0) return Cell{cells_ + (size_t)s * kTile, 1};
    return Cell{open_row(c), 1};
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
          const double idx =
              idx_rows_[(size_t)meta_[2 * s + 1] * kTile + l];
          if (idx >= 0.0) shared_[(size_t)idx] += row[l];
        }
      }
    }
  }

  TileState::View val(int r) const { return fwd_.view(r); }
  const TileState& forward() const { return fwd_; }

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

void lane_adjoint_instruction(const RegionMapProg& p, AdjointTile& t,
                              const AdjInstr& I, Mask m, double* window,
                              KernelCtx& ctx) {
  using Cell = AdjointTile::Cell;
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
      const Cell d = t.cell(I.dst);
      t.each(m, [&](int l) { d[l] = 0.0; });
      return;
    }
    case Program::FILL:
    case Program::CONSTR:
      for (int32_t k = 0; k < I.len; ++k) {
        const Cell d = t.cell(I.dst + k);
        t.each(m, [&](int l) { d[l] = 0.0; });
      }
      return;
    case Program::MOV: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u;
      });
      return;
    }
    case Program::ADD: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a), b = t.acc(I.b);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u;
        b[l] += u;
      });
      return;
    }
    case Program::SUB: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a), b = t.acc(I.b);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u;
        b[l] -= u;
      });
      return;
    }
    case Program::MUL: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a), b = t.acc(I.b);
      const auto va = t.val(I.va), vb = t.val(I.vb);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += vb[l] * u;
        b[l] += va[l] * u;
      });
      return;
    }
    case Program::FMA: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a), b = t.acc(I.b),
                 c = t.acc(I.c);
      const auto va = t.val(I.va), vb = t.val(I.vb);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += vb[l] * u;
        b[l] += va[l] * u;
        c[l] += u;
      });
      return;
    }
    case Program::DIV: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a), b = t.acc(I.b);
      const auto va = t.val(I.va), vb = t.val(I.vb), vd = t.val(I.vd);
      const bool safe = static_cast<uint8_t>(I.len) == kDivSafeGrouping;
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
      return;
    }
    case Program::POW: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a), b = t.acc(I.b);
      const auto va = t.val(I.va), vb = t.val(I.vb), vd = t.val(I.vd);
      const uint8_t law = static_cast<uint8_t>(I.len);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        pow_rule(law, u, va[l], vb[l], vd[l], a[l], b[l]);
      });
      return;
    }
    case Program::FMAX:
    case Program::FMIN: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a), b = t.acc(I.b);
      const auto va = t.val(I.va), vb = t.val(I.vb);
      const uint8_t law = static_cast<uint8_t>(I.len);
      const bool maximum = I.code == Program::FMAX;
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        extremum_rule(maximum, law, u, va[l], vb[l], a[l], b[l]);
      });
      return;
    }
    case Program::NEG: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] -= u;
      });
      return;
    }
    case Program::EXP: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a);
      const auto vd = t.val(I.vd);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u * vd[l];
      });
      return;
    }
    case Program::LOG: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a);
      const auto va = t.val(I.va);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u / va[l];
      });
      return;
    }
    case Program::SQRT: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a);
      const auto vd = t.val(I.vd);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        if (vd[l] != 0.0) a[l] += u / (2.0 * vd[l]);
      });
      return;
    }
    case Program::SQUARE: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a);
      const auto va = t.val(I.va);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] += u * 2.0 * va[l];
      });
      return;
    }
    case Program::INV: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a);
      const auto va = t.val(I.va);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        a[l] -= u / (va[l] * va[l]);
      });
      return;
    }
    case Program::FABS: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a);
      const auto va = t.val(I.va);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        fabs_rule(u, va[l], a[l]);
      });
      return;
    }
    case Program::LSE2:
    case Program::LOG_DIFF_EXP: {
      const Cell d = t.cell(I.dst), a = t.acc(I.a), b = t.acc(I.b);
      const auto va = t.val(I.va), vb = t.val(I.vb);
      const bool lse = I.code == Program::LSE2;
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        if (lse)
          lse2_rule(u, va[l], vb[l], a[l], b[l]);
        else
          log_diff_exp_rule(u, va[l], vb[l], a[l], b[l]);
      });
      return;
    }
    case Program::LOG_MIX: {
      const Cell d = t.cell(I.dst), c = t.acc(I.c), b = t.acc(I.b),
                 a = t.acc(I.a);
      const auto va = t.val(I.va), vb = t.val(I.vb), vc = t.val(I.vc);
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        log_mix_rule(u, va[l], vb[l], vc[l], a[l], b[l], c[l]);
      });
      return;
    }
    case Program::DYN_INDEX: {
      const Cell d = t.cell(I.dst);
      const auto vb = t.val(I.vb);
      if (t.shared_cell(I.a)) {
        const auto site = t.acc_dynamic();
        t.each(m, [&](int l) {
          const double u = d[l];
          d[l] = 0.0;
          site.value[l] = u;
          site.index[l] = static_cast<double>(I.a + static_cast<int32_t>(vb[l]) - 1);
        });
        return;
      }
      t.each(m, [&](int l) {
        const double u = d[l];
        d[l] = 0.0;
        t.cell(I.a + static_cast<int32_t>(vb[l]) - 1)[l] += u;
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
      t.each(m, [&](int l) {
        for (int k = 0; k < call.n_in; ++k)
          for (int j = 0; j < call.in_len[k]; ++j)
            window[w.val_in_off[k] + j] = f.value(call.bwd_value_in[k] + j, l);
        for (int j = 0; j < call.out_len; ++j)
          window[w.val_out_off + j] = f.value(call.bwd_value_out + j, l);
        for (int j = 0; j < call.scratch_len; ++j)
          window[w.bwd_scratch_off + j] = f.value(call.scratch + j, l);
        for (int k = 0; k < call.n_in; ++k)
          if (call.input_adjoint_mask & (1u << k))
            for (int j = 0; j < call.in_len[k]; ++j)
              window[w.adj_in_off[k] + j] =
                  rows[k] ? 0.0 : t.cell(call.bwd_adj_in[k] + j)[l];
        for (int j = 0; j < call.out_len; ++j)
          window[w.adj_out_off + j] = t.cell(call.bwd_adj_out + j)[l];
        ctx.n_in = call.n_in;
        for (int k = 0; k < call.n_in; ++k) {
          ctx.in[k] = Desc{window + w.val_in_off[k], call.in_len[k]};
          ctx.in_adj[k] = (call.input_adjoint_mask & (uint8_t)(1u << k))
                              ? Desc{window + w.adj_in_off[k], call.in_len[k]}
                              : Desc{nullptr, call.in_len[k]};
        }
        ctx.out = Desc{window + w.val_out_off, call.out_len};
        ctx.out_adj_vec = Desc{window + w.adj_out_off, call.out_len};
        ctx.out_adj = call.out_len == 1 ? window[w.adj_out_off] : 0.0;
        ctx.variant = call.variant;
        ctx.scratch = window + w.bwd_scratch_off;
        ctx.idata = call.idata.data();
        ctx.n_idata = (int64_t)call.idata.size();
        ctx.udata = call.udata_owner.get();
        call.backward(ctx);
        for (int k = 0; k < call.n_in; ++k)
          if (call.input_adjoint_mask & (1u << k))
            for (int j = 0; j < call.in_len[k]; ++j) {
              if (rows[k])
                rows[k][(size_t)j * kTile + l] = window[w.adj_in_off[k] + j];
              else
                t.cell(call.bwd_adj_in[k] + j)[l] = window[w.adj_in_off[k] + j];
            }
        for (int j = 0; j < call.scratch_len; ++j)
          f.slot(call.scratch + j)[l] = window[w.bwd_scratch_off + j];
        for (int j = 0; j < call.out_len; ++j)
          t.cell(call.bwd_adj_out + j)[l] = 0.0;
      });
      return;
    }
    default:
      throw std::logic_error("adjoint opcode outside the lane whitelist");
  }
}

std::atomic<uint64_t> lane_runs{0};

}  // namespace

uint64_t region_map_lane_runs() {
  return lane_runs.load(std::memory_order_relaxed);
}

int64_t region_map_lane_cells(const RegionMapProg& p) {
  if (!p.lanes.active) return 0;
  const auto& l = p.lanes;
  return l.storage + l.max_window + (int64_t)l.max_sites * (2 + kRegionMapTile) +
         (int64_t)l.max_dynamic * kRegionMapTile;
}

void region_map_lanes_forward(const RegionMapProg& p, KernelCtx& ctx,
                              double* region) {
  lane_runs.fetch_add(1, std::memory_order_relaxed);
  const auto& plan = p.lanes;
  double* tiles = region;
  double* window = region + plan.storage;
  double* reg = ctx.scratch;
  KernelCtx call_ctx;
  double total = 0.0;
  const int target = p.out_regs[0];
  for (int tile = 0; tile < plan.tiles; ++tile) {
    const int64_t first = (int64_t)tile * kTile;
    const int lanes = (int)std::min<int64_t>(kTile, p.count - first);
    double* base = tiles + (size_t)tile * kTile * plan.fwd_regs;
    for (int r = 0; r < p.n_regs; ++r) {
      const int s = plan.reg_slot[(size_t)r];
      if (s >= 0) std::fill_n(base + (size_t)s * kTile, kTile, reg[r]);
    }
    double* iter = base + (size_t)plan.reg_slot[(size_t)p.iter_reg] * kTile;
    for (int l = 0; l < lanes; ++l)
      iter[l] = static_cast<double>(p.lo + first + l);
    TileState t(p, reg, base, lanes);
    lane_tile_forward(p, t, window, call_ctx, ctx.eval_state);
    const double* out = t.slot(target);
    for (int l = 0; l < lanes; ++l) total += out[l];
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
  double* adj_tile = region + (size_t)plan.tiles * kTile * plan.fwd_regs;
  double* window = region + plan.storage;
  double* log = window + plan.max_window;
  KernelCtx call_ctx;
  for (int tile = plan.tiles; tile-- > 0;) {
    const int64_t first = (int64_t)tile * kTile;
    const int lanes = (int)std::min<int64_t>(kTile, p.count - first);
    double* base = fwd_tiles + (size_t)tile * kTile * plan.fwd_regs;
    std::fill_n(adj_tile, (size_t)plan.adj_cells * kTile, 0.0);
    TileState fwd(p, ctx.scratch, base, lanes);
    AdjointTile t(p, fwd, adj_tile, adj, log);
    const auto seed_cell = t.cell(target);
    for (int l = 0; l < lanes; ++l) seed_cell[l] += seed;
    const auto run = [&](int begin, int end, Mask m) {
      for (int pc = begin; pc < end; ++pc)
        lane_adjoint_instruction(p, t, p.adj.code[(size_t)pc], m, window,
                                 call_ctx);
    };
    if (p.adj.segments.empty()) {
      run(0, (int)p.adj.code.size(), fwd.full());
    } else {
      for (const auto& seg : p.adj.segments) {
        double b0[kTile];
        const double* flag = fwd.read(seg.guard, b0);
        Mask m = 0;
        for (int l = 0; l < lanes; ++l)
          if (flag[l] != 0.0) m |= Mask{1} << l;
        if (m) run(seg.begin, seg.end, m);
      }
    }
    t.replay();
  }
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
    Analysis(p, storage_limit).run();
    p.lanes.active = true;
  } catch (const Refusal& r) {
    p.lanes = RegionMapLanePlan{};
    p.lanes.refusal = r.why;
  }
}

}  // namespace stanli
