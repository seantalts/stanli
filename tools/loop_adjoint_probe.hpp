// Developer-only loop-aware reverse experiment. Basic blocks use the existing
// register evaluator and gen_adjoint rules. Each executed definition gets a
// value version; copies share versions, including across branches/iterations.
// Reverse instructions bind directly to those shared adjoint cells, preserving
// Stan's accumulation order instead of summing per-block input gradients.
#ifndef STANLI_TOOLS_LOOP_ADJOINT_PROBE_HPP
#define STANLI_TOOLS_LOOP_ADJOINT_PROBE_HPP
#include <stanli/island.hpp>
#include <stanli/ode_prog.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

class LoopAdjointProbe {
  using Program = stanli::Program;
  struct Binding {
    int global, local;
  };
  struct Block {
    Program forward;
    stanli::AdjProgram reverse;
    std::vector<Binding> inputs, outputs;
    std::vector<int> saved;
    stanli::AdjProgram selective_reverse;
    int next = -1, zero = -1, condition = -1;
  };
  std::vector<Block> blocks_;
  std::vector<int> inputs_, outputs_;
  int registers_ = 0;
  bool cached_ = std::getenv("STANLI_LOOP_HISTORY") &&
                 std::string(std::getenv("STANLI_LOOP_HISTORY")) == "cached";
  struct CachedPath {
    Program forward;
    stanli::AdjProgram reverse;
    int success = -1;
  };
  bool selective_ =
      std::getenv("STANLI_LOOP_HISTORY") &&
      std::string(std::getenv("STANLI_LOOP_HISTORY")) != "expanded";
  static int index(size_t n) {
    if (n > static_cast<size_t>(std::numeric_limits<int>::max()))
      throw std::length_error("loop adjoint history exceeds index range");
    return static_cast<int>(n);
  }
  static bool scalar(Program::Code c) {
    switch (c) {
      case Program::ADD:
      case Program::SUB:
      case Program::MUL:
      case Program::DIV:
      case Program::NEG:
      case Program::EXP:
      case Program::LOG:
      case Program::SQRT:
      case Program::SQUARE:
      case Program::INV:
      case Program::FABS:
      case Program::INV_LOGIT:
      case Program::LOG1M:
      case Program::LOG1P_EXP:
      case Program::TANH:
      case Program::POW:
      case Program::FMA:
      case Program::LSE2:
      case Program::LOG_MIX:
      case Program::GT:
      case Program::GE:
      case Program::LT:
      case Program::LE:
      case Program::EQ:
      case Program::NE:
      case Program::IADD:
      case Program::ISUB:
      case Program::IMUL:
      case Program::INEG:
      case Program::IABS:
      case Program::IDIV:
      case Program::IMOD:
        return true;
      default:
        return false;
    }
  }

 public:
  struct Workspace {
    std::vector<double> values, adjoints, live, scratch;
    size_t adjoint_count = 0;
    struct Visit {
      int block;
      bool zero;
    };
    std::vector<Visit> visits;
    std::vector<CachedPath> cache;
    std::vector<double> cached_values;
    const LoopAdjointProbe* cache_owner = nullptr;
    int cache_active = -1;
    size_t cache_hits = 0, cache_misses = 0;
    std::vector<int> versions, local_versions;
    stanli::AdjProgram history;
    size_t blocks_executed = 0;
    bool ready = false;
    size_t cached_plan_bytes() const {
      size_t bytes = 0;
      for (const auto& p : cache)
        bytes += p.forward.code.size() * sizeof(Program::Instr) +
                 p.forward.pool.size() * sizeof(double) +
                 p.forward.out_regs.size() * sizeof(int) +
                 p.reverse.code.size() * sizeof(stanli::AdjInstr) +
                 p.reverse.adj_reg.size() * sizeof(int);
      return bytes;
    }
    size_t retained_bytes() const {
      return cached_plan_bytes() + visits.size() * sizeof(Visit) +
             (values.size() + live.size() + scratch.size() +
              cached_values.size()) *
                 sizeof(double) +
             adjoints.size() * sizeof(double) +
             history.code.size() * sizeof(stanli::AdjInstr) +
             (versions.size() + local_versions.size()) * sizeof(int);
    }
  };
  LoopAdjointProbe(const Program& source, std::vector<int> inputs)
      : inputs_(std::move(inputs)),
        outputs_(source.out_regs),
        registers_(source.n_regs) {
    if (const char* mode = std::getenv("STANLI_LOOP_HISTORY")) {
      const std::string name(mode);
      if (name != "expanded" && name != "selective" && name != "cached")
        throw std::invalid_argument("unknown developer loop history mode");
    }
    if (registers_ < 0) throw std::invalid_argument("negative register count");
    const int end = index(source.code.size());
    std::set<int> leaders{0, end};
    const auto check = [&](int r) {
      if (r < 0 || r >= registers_)
        throw std::invalid_argument("invalid register binding");
    };
    for (int r : inputs_) check(r);
    for (int r : outputs_) check(r);
    for (int pc = 0; pc < end; ++pc) {
      const auto& i = source.code[pc];
      if (i.code == Program::JMP || i.code == Program::JZ) {
        if (i.dst < 0 || i.dst > end)
          throw std::invalid_argument("invalid jump target");
        if (i.code == Program::JZ) check(i.a);
        leaders.insert(i.dst);
        leaders.insert(pc + 1);
      } else if (!scalar(i.code) && i.code != Program::CONST &&
                 i.code != Program::CONSTR && i.code != Program::FILL &&
                 i.code != Program::MOV && i.code != Program::MOVR) {
        throw std::invalid_argument(std::string("loop adjoint probe refuses ") +
                                    stanli::program_code_spec(i.code).name);
      }
    }
    std::vector<int> starts(leaders.begin(), leaders.end());
    std::map<int, int> block_at;
    for (size_t i = 0; i < starts.size(); ++i) block_at[starts[i]] = index(i);
    for (size_t b = 0; b + 1 < starts.size(); ++b) {
      Block block;
      stanli::IslandProg generated;
      generated.pool = source.pool;
      std::map<int, int> current;
      std::set<int> written;
      const auto read = [&](int r) {
        check(r);
        auto it = current.find(r);
        if (it != current.end()) return it->second;
        int local = generated.n_regs++;
        current[r] = local;
        block.inputs.push_back({r, local});
        generated.ins.push_back({local, 1, -1, 0, true});
        return local;
      };
      const auto write = [&](int r, int local) {
        check(r);
        current[r] = local;
        written.insert(r);
      };
      block.next = index(b + 1);
      for (int pc = starts[b]; pc < starts[b + 1]; ++pc) {
        const auto& i = source.code[pc];
        if (i.code == Program::JMP) {
          block.next = block_at.at(i.dst);
          continue;
        }
        if (i.code == Program::JZ) {
          block.condition = i.a;
          block.zero = block_at.at(i.dst);
          continue;
        }
        if (i.code == Program::MOV || i.code == Program::MOVR) {
          const int len = i.code == Program::MOV ? 1 : i.len;
          if (len < 0) throw std::invalid_argument("negative copy width");
          // Original MOVR copies in ascending order, including overlap.
          for (int k = 0; k < len; ++k) write(i.dst + k, read(i.a + k));
          continue;
        }
        if (i.code == Program::CONST || i.code == Program::CONSTR ||
            i.code == Program::FILL) {
          const int len = i.code == Program::CONST ? 1 : i.len;
          if (len < 0) throw std::invalid_argument("negative constant width");
          for (int k = 0; k < len; ++k) {
            int pool = i.a + (i.code == Program::CONSTR ? k : 0);
            if (pool < 0 || static_cast<size_t>(pool) >= source.pool.size())
              throw std::invalid_argument("invalid constant index");
            int dst = generated.n_regs++;
            write(i.dst + k, dst);
            generated.code.push_back({Program::CONST, dst, pool});
          }
          continue;
        }
        auto instruction = i;
        instruction.a = stanli::program_reads(i, 0) ? read(i.a) : 0;
        instruction.b = stanli::program_reads(i, 1) ? read(i.b) : 0;
        instruction.c = stanli::program_reads(i, 2) ? read(i.c) : 0;
        instruction.dst = generated.n_regs++;
        write(i.dst, instruction.dst);
        generated.code.push_back(instruction);
      }
      for (int r : written) {
        block.outputs.push_back({r, current.at(r)});
        generated.out_regs.push_back(current.at(r));
      }
      block.forward = static_cast<const Program&>(generated);
      // Integer division/modulus create independent var values in Program.
      // The existing zero-derivative integer rule suffices for their reverse;
      // the canonical integer instruction still executes in the forward.
      for (auto& i : generated.code)
        if (i.code == Program::IDIV || i.code == Program::IMOD)
          i.code = Program::IADD;
      if (generated.n_regs && !stanli::gen_adjoint(generated))
        throw std::invalid_argument("straight-line reverse refused SSA block");
      if (generated.n_regs != block.forward.n_regs)
        throw std::logic_error("SSA block unexpectedly needs checkpoints");
      for (size_t i = 0; i < generated.adj.adj_reg.size(); ++i)
        if (generated.adj.adj_reg[i] != static_cast<int>(i))
          throw std::logic_error("SSA reverse changed adjoint identity");
      // With private-clear elision disabled, CONST reverse rules retain pool
      // indices in unused operand fields. They are not register bindings.
      for (auto& rule : generated.adj.code)
        if (stanli::program_code_spec(rule.code).has(stanli::kProgramNoInputs))
          rule.a = rule.b = rule.c = rule.va = rule.vb = rule.vc = rule.vd = 0;
      block.reverse = std::move(generated.adj);
      block.selective_reverse = block.reverse;
      std::map<int, int> saved;
      const auto retain = [&](int r) {
        auto it = saved.find(r);
        if (it != saved.end()) return it->second;
        int slot = index(block.saved.size());
        block.saved.push_back(r);
        saved[r] = slot;
        return slot;
      };
      for (auto& rule : block.selective_reverse.code) {
        const auto& spec = stanli::program_code_spec(rule.code);
        rule.va = spec.has(stanli::kProgramSaveA) ? retain(rule.va) : 0;
        rule.vb = spec.has(stanli::kProgramSaveB) ? retain(rule.vb) : 0;
        rule.vc = spec.has(stanli::kProgramSaveC) ? retain(rule.vc) : 0;
        rule.vd = spec.has(stanli::kProgramSaveOut) ? retain(rule.vd) : 0;
      }
      blocks_.push_back(std::move(block));
    }
  }
  size_t blocks() const { return blocks_.size(); }
  size_t prepared_instructions() const {
    size_t n = 0;
    for (const auto& b : blocks_)
      n += b.forward.code.size() + b.reverse.code.size();
    return n;
  }
  void prepare_path(Workspace& w) const {
    // Bound speculative expansion independently of any model identity.
    size_t size = inputs_.size();
    for (const auto& visit : w.visits)
      size += blocks_[visit.block].forward.n_regs;
    if (size > 100000) return;
    stanli::IslandProg trace;
    if (!blocks_.empty()) trace.pool = blocks_[0].forward.pool;
    trace.n_regs = index(inputs_.size());
    std::vector<int> versions(registers_, -1);
    for (size_t k = 0; k < inputs_.size(); ++k) {
      versions[inputs_[k]] = index(k);
      trace.ins.push_back({index(k), 1, -1, 0, true});
    }
    struct Guard {
      size_t after;
      int condition;
      bool zero;
    };
    std::vector<Guard> guards;
    for (const auto& visit : w.visits) {
      const auto& b = blocks_[visit.block];
      std::vector<int> local(b.forward.n_regs, -1);
      for (const auto& in : b.inputs) local[in.local] = versions[in.global];
      for (const auto& original : b.forward.code) {
        auto instruction = original;
        if (stanli::program_reads(original, 0))
          instruction.a = local[original.a];
        if (stanli::program_reads(original, 1))
          instruction.b = local[original.b];
        if (stanli::program_reads(original, 2))
          instruction.c = local[original.c];
        instruction.dst = trace.n_regs++;
        local[original.dst] = instruction.dst;
        trace.code.push_back(instruction);
      }
      for (const auto& out : b.outputs) versions[out.global] = local[out.local];
      if (b.condition >= 0 && b.zero != b.next)
        guards.push_back(
            {trace.code.size(), versions[b.condition], visit.zero});
    }
    for (int r : outputs_) trace.out_regs.push_back(versions[r]);
    const Program forward = trace;
    for (auto& i : trace.code)
      if (i.code == Program::IDIV || i.code == Program::IMOD)
        i.code = Program::IADD;
    if (!stanli::gen_adjoint(trace)) return;
    if (trace.n_regs != forward.n_regs ||
        trace.code.size() != forward.code.size())
      return;
    for (size_t i = 0; i < trace.adj.adj_reg.size(); ++i)
      if (trace.adj.adj_reg[i] != index(i)) return;
    CachedPath path;
    path.reverse = std::move(trace.adj);
    path.forward = forward;
    path.forward.code.clear();
    path.success = path.forward.n_regs++;
    size_t guard = 0;
    std::vector<int> fail_jumps;
    for (size_t pc = 0; pc <= forward.code.size(); ++pc) {
      while (guard < guards.size() && guards[guard].after == pc) {
        const auto& g = guards[guard++];
        if (g.zero) {
          path.forward.code.push_back(
              {Program::JZ, index(path.forward.code.size() + 2), g.condition});
          fail_jumps.push_back(index(path.forward.code.size()));
          path.forward.code.push_back({Program::JMP, 0});
        } else {
          fail_jumps.push_back(index(path.forward.code.size()));
          path.forward.code.push_back({Program::JZ, 0, g.condition});
        }
      }
      if (pc < forward.code.size())
        path.forward.code.push_back(forward.code[pc]);
    }
    const int one = index(path.forward.pool.size());
    path.forward.pool.push_back(1);
    const int zero = index(path.forward.pool.size());
    path.forward.pool.push_back(0);
    path.forward.code.push_back({Program::CONST, path.success, one});
    path.forward.code.push_back(
        {Program::JMP, index(path.forward.code.size() + 2)});
    const int failed = index(path.forward.code.size());
    path.forward.code.push_back({Program::CONST, path.success, zero});
    for (int pc : fail_jumps) path.forward.code[pc].dst = failed;
    if (w.cache.size() == 4) w.cache.erase(w.cache.begin());
    w.cache.push_back(std::move(path));
  }
  bool try_path(const std::vector<double>& input, Workspace& w) const {
    w.ready = false;
    w.cache_active = -1;
    if (input.size() < static_cast<size_t>(registers_))
      throw std::invalid_argument("short input file");
    if (w.cache_owner != this) {
      w.cache.clear();
      w.cache_owner = this;
    }
    for (size_t i = 0; i < w.cache.size(); ++i) {
      const auto& path = w.cache[i];
      w.cached_values.resize(path.forward.n_regs);
      for (size_t k = 0; k < inputs_.size(); ++k)
        w.cached_values[k] = input[inputs_[k]];
      stanli::run_program(path.forward, w.cached_values.data());
      if (w.cached_values[path.success] == 1) {
        w.cache_active = index(i);
        w.ready = true;
        ++w.cache_hits;
        return true;
      }
    }
    ++w.cache_misses;
    return false;
  }
  void forward_selective(const std::vector<double>& input, Workspace& w) const {
    w.ready = false;
    if (input.size() < static_cast<size_t>(registers_))
      throw std::invalid_argument("short input file");
    w.values.assign(1, 0);  // valid sentinel for unused value fields
    w.history.code.clear();
    w.blocks_executed = 0;
    w.visits.clear();
    w.live = input;
    w.versions.assign(registers_, -1);
    w.adjoint_count =
        inputs_.size() + 1;  // constant sink has no reverse reader
    for (size_t k = 0; k < inputs_.size(); ++k) {
      if (w.versions[inputs_[k]] >= 0)
        throw std::invalid_argument("duplicate input binding");
      w.versions[inputs_[k]] = index(k);
    }
    int pc = 0;
    while (pc < static_cast<int>(blocks_.size())) {
      const Block& b = blocks_[pc];
      ++w.blocks_executed;
      w.scratch.resize(b.forward.n_regs);
      w.local_versions.assign(b.forward.n_regs, index(inputs_.size()));
      for (const auto& rule : b.reverse.code)
        w.local_versions[rule.dst] = index(w.adjoint_count++);
      for (const auto& in : b.inputs) {
        if (w.versions[in.global] < 0)
          throw std::logic_error("read before definition in callback");
        w.scratch[in.local] = w.live[in.global];
        w.local_versions[in.local] = w.versions[in.global];
      }
      if (b.forward.n_regs) stanli::run_program(b.forward, w.scratch.data());
      const int offset = index(w.values.size());
      for (int r : b.saved) w.values.push_back(w.scratch[r]);
      for (auto it = b.selective_reverse.code.rbegin();
           it != b.selective_reverse.code.rend(); ++it) {
        auto rule = *it;
        rule.dst = w.local_versions[rule.dst];
        rule.a = w.local_versions[rule.a];
        rule.b = w.local_versions[rule.b];
        rule.c = w.local_versions[rule.c];
        rule.va += offset;
        rule.vb += offset;
        rule.vc += offset;
        rule.vd += offset;
        w.history.code.push_back(rule);
      }
      for (const auto& out : b.outputs) {
        w.live[out.global] = w.scratch[out.local];
        w.versions[out.global] = w.local_versions[out.local];
      }
      if (b.condition >= 0) {
        if (w.versions[b.condition] < 0)
          throw std::logic_error("undefined condition");
        if (cached_) w.visits.push_back({pc, w.live[b.condition] == 0});
        pc = w.live[b.condition] == 0 ? b.zero : b.next;
      } else {
        if (cached_) w.visits.push_back({pc, false});
        pc = b.next;
      }
    }
    for (int r : outputs_)
      if (w.versions[r] < 0) throw std::logic_error("undefined output");
    std::reverse(w.history.code.begin(), w.history.code.end());
    w.ready = true;
  }
  void forward(const std::vector<double>& input, Workspace& w) const {
    if (cached_ && try_path(input, w)) return;
    if (selective_) {
      forward_selective(input, w);
      if (cached_) {
        try {
          prepare_path(w);
        } catch (...) {
          w.ready = false;
          throw;
        }
      }
      return;
    }
    w.ready = false;
    w.values.clear();
    w.history.code.clear();
    w.blocks_executed = 0;
    w.versions.assign(registers_, -1);
    if (input.size() < static_cast<size_t>(registers_))
      throw std::invalid_argument("short input file");
    for (int r : inputs_) {
      if (w.versions[r] >= 0)
        throw std::invalid_argument("duplicate input binding");
      w.versions[r] = index(w.values.size());
      w.values.push_back(input[r]);
    }
    int pc = 0;
    while (pc < static_cast<int>(blocks_.size())) {
      const Block& b = blocks_[pc];
      ++w.blocks_executed;
      int offset = index(w.values.size());
      index(w.values.size() + b.forward.n_regs);
      w.values.resize(w.values.size() + b.forward.n_regs);
      w.local_versions.resize(b.forward.n_regs);
      for (int k = 0; k < b.forward.n_regs; ++k)
        w.local_versions[k] = offset + k;
      for (const auto& in : b.inputs) {
        const int version = w.versions[in.global];
        if (version < 0)
          throw std::logic_error("read before definition in callback");
        w.values[offset + in.local] = w.values[version];
        w.local_versions[in.local] = version;
      }
      if (b.forward.n_regs)
        stanli::run_program(b.forward, w.values.data() + offset);
      // Record in execution order, then reverse the complete stream once.
      for (auto it = b.reverse.code.rbegin(); it != b.reverse.code.rend();
           ++it) {
        auto rule = *it;
        rule.dst = w.local_versions[rule.dst];
        rule.a = w.local_versions[rule.a];
        rule.b = w.local_versions[rule.b];
        rule.c = w.local_versions[rule.c];
        rule.va += offset;
        rule.vb += offset;
        rule.vc += offset;
        rule.vd += offset;
        w.history.code.push_back(rule);
      }
      for (const auto& out : b.outputs)
        w.versions[out.global] = w.local_versions[out.local];
      if (b.condition >= 0) {
        int version = w.versions[b.condition];
        if (version < 0) throw std::logic_error("undefined condition");
        pc = w.values[version] == 0 ? b.zero : b.next;
      } else
        pc = b.next;
    }
    for (int r : outputs_)
      if (w.versions[r] < 0) throw std::logic_error("undefined output");
    std::reverse(w.history.code.begin(), w.history.code.end());
    w.ready = true;
  }
  double output(const Workspace& w, size_t i) const {
    if (!w.ready) throw std::logic_error("no successful forward");
    if (cached_ && w.cache_active >= 0)
      return w.cached_values[w.cache[w.cache_active].forward.out_regs.at(i)];
    return selective_ ? w.live[outputs_.at(i)]
                      : w.values[w.versions[outputs_.at(i)]];
  }
  void reverse(Workspace& w, const std::vector<double>& seed,
               std::vector<double>& gradient) const {
    if (!w.ready || seed.size() != outputs_.size())
      throw std::logic_error("invalid reverse seed");
    if (cached_ && w.cache_active >= 0) {
      const auto& path = w.cache[w.cache_active];
      w.adjoints.assign(path.reverse.n_regs, 0);
      for (size_t i = 0; i < seed.size(); ++i)
        w.adjoints[path.forward.out_regs[i]] += seed[i];
      stanli::run_adjoint(path.forward, path.reverse, w.cached_values.data(),
                          w.adjoints.data());
      gradient.assign(w.adjoints.begin(), w.adjoints.begin() + inputs_.size());
      return;
    }
    w.adjoints.assign(selective_ ? w.adjoint_count : w.values.size(), 0);
    for (size_t i = 0; i < seed.size(); ++i)
      w.adjoints[w.versions[outputs_[i]]] += seed[i];
    static const Program no_calls;
    stanli::run_adjoint(no_calls, w.history, w.values.data(),
                        w.adjoints.data());
    gradient.assign(w.adjoints.begin(), w.adjoints.begin() + inputs_.size());
  }
};

class LoopRhsProbe {
  stanli::RhsProgram rhs_;
  LoopAdjointProbe plan_;
  LoopAdjointProbe::Workspace workspace_;
  std::vector<double> input_, gradient_, seed_;
  static std::vector<int> inputs(const stanli::RhsProgram& p) {
    std::vector<int> out{p.t_reg};
    for (int k = 0; k < p.n_y; ++k) out.push_back(p.y0 + k);
    for (int k = 0; k < p.n_th; ++k) out.push_back(p.th0 + k);
    for (int k = 0; k < p.n_xr; ++k) out.push_back(p.xr0 + k);
    return out;
  }

 public:
  explicit LoopRhsProbe(const stanli::RhsProgram& rhs)
      : rhs_(rhs), plan_(rhs, inputs(rhs)), input_(rhs.n_regs), seed_(rhs.n_y) {
    if (!rhs.ok || rhs.n_yp)
      throw std::invalid_argument("probe requires compiled ODE callback");
  }
  void forward(double t, const double* y, const std::vector<double>& theta,
               const double* xr) {
    stanli::detail::seed_rhs_regs<double>(rhs_, t, y, theta.data(),
                                          theta.size(), xr, input_);
    plan_.forward(input_, workspace_);
  }
  void reverse(const std::vector<double>& seed) {
    plan_.reverse(workspace_, seed, gradient_);
  }
  double output(size_t i) const { return plan_.output(workspace_, i); }
  const std::vector<double>& gradient() const { return gradient_; }
  const LoopAdjointProbe::Workspace& workspace() const { return workspace_; }
  const LoopAdjointProbe& plan() const { return plan_; }
  void evaluate(double t, const double* y, const std::vector<double>& theta,
                const double* xr, bool theta_derivatives, double* f, double* jy,
                double* jt) {
    forward(t, y, theta, xr);
    for (int o = 0; o < rhs_.n_y; ++o) {
      f[o] = output(o);
      std::fill(seed_.begin(), seed_.end(), 0);
      seed_[o] = 1;
      reverse(seed_);
      for (int k = 0; k < rhs_.n_y; ++k)
        jy[o * rhs_.n_y + k] = gradient_[1 + k];
      if (theta_derivatives)
        for (int k = 0; k < rhs_.n_th; ++k)
          jt[o * theta.size() + k] = gradient_[1 + rhs_.n_y + k];
    }
  }
};
#endif
