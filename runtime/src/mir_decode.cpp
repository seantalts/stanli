#include <stanli/compile.hpp>
#include <stanli/large_stack.hpp>
#include <stanli/mir_decode.hpp>

#include "mir_reader_internal.hpp"

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace stanli {
namespace {

bool is_space(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

// stanc reuses its generated names in nested blocks: a foreach inside a
// foreach declares `tmp1__` and loops over `idx1__` at both levels, which is
// fine for block-scoped C++. stanli's interpreters and lowering keep one
// binding per name, so the inner declaration would overwrite the outer one
// for the rest of the outer loop. Give each declaration that hides a visible
// name a fresh name of its own, with the same scopes the reader validates.
class ShadowRenamer {
 public:
  void section(std::vector<mir::Stmt>& body,
               const std::vector<std::string>& visible = {}) {
    frames_.clear();
    frames_.emplace_back();
    for (const std::string& name : visible) frames_.back()[name] = name;
    list(body);
  }

 private:
  std::vector<std::map<std::string, std::string>> frames_;
  int fresh_ = 0;

  const std::string* spelled(const std::string& name) const {
    for (auto frame = frames_.rbegin(); frame != frames_.rend(); ++frame) {
      auto it = frame->find(name);
      if (it != frame->end()) return &it->second;
    }
    return nullptr;
  }
  void declare(std::string* name) {
    const std::string original = *name;
    if (spelled(original))
      *name = original + "shadow" + std::to_string(++fresh_) + "__";
    frames_.back()[original] = *name;
  }
  void expr(mir::Expr& e) {
    if (e.kind == mir::Expr::Var)
      if (const std::string* name = spelled(e.name)) e.name = *name;
    for (mir::Expr& arg : e.args) expr(arg);
  }
  void list(std::vector<mir::Stmt>& body) {
    for (mir::Stmt& s : body) stmt(s);
  }
  void nested(std::vector<mir::Stmt>& body, std::string* loopvar = nullptr) {
    frames_.emplace_back();
    if (loopvar) declare(loopvar);
    list(body);
    frames_.pop_back();
  }
  void stmt(mir::Stmt& s) {
    mir::for_each_expr(s, [&](mir::Expr& e) { expr(e); });
    if (!s.lhs.empty())
      if (const std::string* name = spelled(s.lhs)) s.lhs = *name;
    if (s.kind == mir::Stmt::Decl) declare(&s.decl_id);
    if (s.kind == mir::Stmt::SList) {
      list(s.body);
    } else if (s.kind == mir::Stmt::IfElse) {
      for (mir::Stmt& arm : s.body) {
        frames_.emplace_back();
        stmt(arm);
        frames_.pop_back();
      }
    } else if (!s.body.empty()) {
      nested(s.body, s.kind == mir::Stmt::For ? &s.loopvar : nullptr);
    }
  }
};

mir::Program rename_shadowed(mir::Program program) {
  ShadowRenamer renamer;
  for (auto* section : {&program.prepare_data, &program.log_prob,
                        &program.generate_quantities, &program.transform_inits})
    renamer.section(*section);
  for (mir::FunDef& f : program.fun_defs) renamer.section(f.body, f.arg_names);
  return program;
}

mir::Program decode_text(std::string_view text) {
  constexpr std::string_view magic = "STANLI5:";
  if (text.substr(0, magic.size()) == magic)
    return rename_shadowed(mir::detail::read_portable_program(text));
  if (text.substr(0, 6) == "STANLI")
    throw std::runtime_error(
        "mir: portable MIR " + std::string(text.substr(0, 8)) +
        " is from a different stanli compiler; this runtime reads STANLI5: "
        "recompile the model");

  size_t first = 0;
  while (first < text.size() && is_space(text[first])) ++first;
  if (first == text.size()) throw mir::MirError("mir: empty input");
  throw mir::MirError(
      "mir: not portable MIR. stanli reads only the STANLI5 portable MIR "
      "that its own compiler writes: stanli-compile, stan_to_mir() in "
      "Python, or the embedded compiler. Stock stanc's s-expression MIR "
      "(--debug-optimized-mir) is no longer read");
}

}  // namespace

mir::Program decode_program(std::string_view text) {
  try {
    return decode_text(text);
  } catch (const NestingTooDeep&) {
  }
  try {
    return run_on_large_stack([&] { return decode_text(text); });
  } catch (const NestingTooDeep& e) {
    throw CompileError(e.what());
  }
}

}  // namespace stanli
