#ifndef STANLI_LOWER_WORK_HPP
#define STANLI_LOWER_WORK_HPP

#include <stanli/data.hpp>
#include <stanli/mir.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace stanli {
namespace lower_detail {

class LoweringWork {
 public:
  static constexpr uint64_t kUnknownTrips = 8;

  LoweringWork(const mir::Program& program,
               const std::map<std::string, long>& ints,
               const std::map<std::string, DataMap::Entry>& env, uint64_t limit)
      : program_(program), ints_(ints), env_(env), limit_(limit) {}

  uint64_t of(const std::vector<mir::Stmt>& section) {
    return statements(section, 0);
  }

 private:
  uint64_t add(uint64_t a, uint64_t b) const {
    return a >= limit_ - b ? limit_ : a + b;
  }
  uint64_t mul(uint64_t a, uint64_t b) const {
    return b != 0 && a >= limit_ / b ? limit_ : a * b;
  }

  std::optional<int64_t> value(const mir::Expr& e) const {
    switch (e.kind) {
      case mir::Expr::LitInt:
        return e.lit_i;
      case mir::Expr::Var: {
        const auto it = ints_.find(e.name);
        if (it == ints_.end()) return std::nullopt;
        return it->second;
      }
      case mir::Expr::Promotion:
        return e.args.empty() ? std::nullopt : value(e.args[0]);
      case mir::Expr::FunApp: {
        if (e.args.size() == 1 && e.args[0].kind == mir::Expr::Var &&
            (e.name == "size" || e.name == "rows" ||
             e.name == "num_elements")) {
          const auto it = env_.find(e.args[0].name);
          if (it == env_.end() || it->second.dims.empty()) return std::nullopt;
          if (e.name == "num_elements") {
            int64_t n = 1;
            for (int64_t d : it->second.dims) n *= d;
            return n;
          }
          return it->second.dims[0];
        }
        if (e.args.size() != 2) return std::nullopt;
        const auto a = value(e.args[0]);
        const auto b = value(e.args[1]);
        if (!a || !b) return std::nullopt;
        if (e.name == "Plus__") return *a + *b;
        if (e.name == "Minus__") return *a - *b;
        if (e.name == "Times__") return *a * *b;
        return std::nullopt;
      }
      default:
        return std::nullopt;
    }
  }

  uint64_t expression(const mir::Expr& e, int depth) {
    uint64_t n = 1;
    for (const auto& a : e.args) {
      n = add(n, expression(a, depth));
      if (n >= limit_) return limit_;
    }
    if (e.kind == mir::Expr::FunApp &&
        e.fn_lib == mir::Expr::Lib::UserDefined && depth < 4)
      n = add(n, function(e.name, depth + 1));
    return n;
  }

  uint64_t function(const std::string& name, int depth) {
    const auto cached = functions_.find(name);
    if (cached != functions_.end()) return cached->second;
    uint64_t n = 0;
    for (const auto& f : program_.fun_defs)
      if (f.name == name) {
        n = statements(f.body, depth);
        break;
      }
    functions_[name] = n;
    return n;
  }

  uint64_t statements(const std::vector<mir::Stmt>& list, int depth) {
    uint64_t n = 0;
    for (const auto& s : list) {
      n = add(n, statement(s, depth));
      if (n >= limit_) return limit_;
    }
    return n;
  }

  uint64_t statement(const mir::Stmt& s, int depth) {
    uint64_t n = 1;
    switch (s.kind) {
      case mir::Stmt::Decl:
        if (s.has_init) n = add(n, expression(s.init, depth));
        for (const auto& d : s.decl_type.dims) n = add(n, expression(d, depth));
        return n;
      case mir::Stmt::Assignment:
        for (const auto& i : s.lhs_idx) n = add(n, expression(i, depth));
        return add(n, expression(s.rhs, depth));
      case mir::Stmt::TargetPE:
        return add(n, expression(s.target, depth));
      case mir::Stmt::NRFunApp:
        for (const auto& a : s.fn_args) n = add(n, expression(a, depth));
        return n;
      case mir::Stmt::Return:
        return s.has_init ? add(n, expression(s.rhs, depth)) : n;
      case mir::Stmt::For: {
        const auto lo = value(s.lower);
        const auto hi = value(s.upper);
        const uint64_t trips =
            lo && hi
                ? static_cast<uint64_t>(std::max<int64_t>(0, *hi - *lo + 1))
                : kUnknownTrips;
        n = add(n, add(expression(s.lower, depth), expression(s.upper, depth)));
        return add(n, mul(trips, statements(s.body, depth)));
      }
      case mir::Stmt::While:
        n = add(n, expression(s.cond, depth));
        return add(n, mul(kUnknownTrips, statements(s.body, depth)));
      case mir::Stmt::IfElse:
        n = add(n, expression(s.cond, depth));
        return add(n, statements(s.body, depth));
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        return statements(s.body, depth);
      default:
        return n;
    }
  }

  const mir::Program& program_;
  const std::map<std::string, long>& ints_;
  const std::map<std::string, DataMap::Entry>& env_;
  const uint64_t limit_;
  std::map<std::string, uint64_t> functions_;
};

}  // namespace lower_detail
}  // namespace stanli

#endif
