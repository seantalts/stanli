// Developer probe for understanding a candidate structured-loop lowering.
//
// This intentionally has no CMake target: it is useful with large, private
// MIR/data attachments and is compiled on demand against an existing build:
//
//   c++ -std=c++17 -Iruntime/include -Iruntime/src \
//     tools/inspect_scan_mir.cpp runtime/src/mir_decode.cpp \
//     runtime/src/mir_reader.cpp runtime/src/portable_mir_v2_reader.cpp \
//     -o /tmp/inspect_scan_mir
//   /tmp/inspect_scan_mir transformed.mir observation previous_observation

#include <stanli/mir_decode.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using stanli::mir::Expr;
using stanli::mir::Stmt;

namespace {

std::string slurp(const char* path) {
  std::ifstream stream(path);
  std::ostringstream text;
  text << stream.rdbuf();
  return text.str();
}

std::string expr(const Expr& e, int depth = 0) {
  if (depth > 5) return "...";
  switch (e.kind) {
    case Expr::Var:
      return e.name;
    case Expr::LitInt:
      return std::to_string(e.lit_i);
    case Expr::LitReal: {
      std::array<char, 40> buffer{};
      std::snprintf(buffer.data(), buffer.size(), "%g", e.lit);
      return buffer.data();
    }
    case Expr::LitStr:
      return '"' + e.lit_s + '"';
    case Expr::Indexed: {
      std::string out = expr(e.args.at(0), depth + 1) + "[";
      for (size_t k = 1; k < e.args.size(); ++k) {
        if (k != 1) out += ',';
        out += e.args[k].name;
        if (!e.args[k].args.empty()) {
          out += '(';
          for (size_t j = 0; j < e.args[k].args.size(); ++j) {
            if (j != 0) out += ',';
            out += expr(e.args[k].args[j], depth + 1);
          }
          out += ')';
        }
      }
      return out + ']';
    }
    case Expr::TernaryIf:
      return "ternary(" + expr(e.args.at(0), depth + 1) + ',' +
             expr(e.args.at(1), depth + 1) + ',' +
             expr(e.args.at(2), depth + 1) + ')';
    case Expr::EOr:
    case Expr::EAnd:
    case Expr::FunApp:
    case Expr::Promotion: {
      std::string out = e.name.empty() ? "promotion" : e.name;
      out += '(';
      for (size_t k = 0; k < e.args.size(); ++k) {
        if (k != 0) out += ',';
        out += expr(e.args[k], depth + 1);
      }
      return out + ')';
    }
    default:
      return "unsupported";
  }
}

const char* kind_name(Stmt::Kind kind) {
  switch (kind) {
    case Stmt::Decl:
      return "Decl";
    case Stmt::Assignment:
      return "Assignment";
    case Stmt::TargetPE:
      return "TargetPE";
    case Stmt::Block:
      return "Block";
    case Stmt::SList:
      return "SList";
    case Stmt::For:
      return "For";
    case Stmt::IfElse:
      return "IfElse";
    case Stmt::While:
      return "While";
    case Stmt::NRFunApp:
      return "NRFunApp";
    case Stmt::Return:
      return "Return";
    case Stmt::Break:
      return "Break";
    case Stmt::Continue:
      return "Continue";
    case Stmt::Skip:
      return "Skip";
    case Stmt::Unsupported:
      return "Unsupported";
  }
  return "?";
}

bool references_any(const Expr& e, const std::set<std::string>& names) {
  if (e.kind == Expr::Var && names.count(e.name) != 0) return true;
  for (const Expr& arg : e.args)
    if (references_any(arg, names)) return true;
  return false;
}

std::string base_name(const Expr& e) {
  const Expr* base = &e;
  while (base->kind == Expr::Indexed && !base->args.empty())
    base = &base->args[0];
  return base->kind == Expr::Var ? base->name : expr(*base);
}

void collect_reads(const Expr& e, std::set<std::string>* out) {
  if (e.kind == Expr::Var) out->insert(e.name);
  for (const Expr& arg : e.args) collect_reads(arg, out);
}

struct Report {
  std::map<std::string, size_t> statement_counts;
  std::set<std::string> declared;
  std::set<std::string> assigned;
  std::set<std::string> reads;
  std::set<std::string> row_reads;
  std::set<std::string> indexed_writes;
  std::vector<std::string> loops;
  std::vector<std::string> whiles;
};

void scan_expr(const Expr& e, const std::set<std::string>& row_names,
               Report* report) {
  collect_reads(e, &report->reads);
  if (e.kind == Expr::Indexed && e.args.size() > 1) {
    bool row_varying = false;
    for (size_t k = 1; k < e.args.size(); ++k)
      row_varying = row_varying || references_any(e.args[k], row_names);
    if (row_varying)
      report->row_reads.insert(base_name(e) + " <- " + expr(e) +
                               (e.data_only ? " [data]" : " [autodiff]"));
  }
  for (const Expr& arg : e.args) scan_expr(arg, row_names, report);
}

void scan_stmt(const Stmt& s, const std::set<std::string>& row_names,
               const std::string& path, Report* report) {
  ++report->statement_counts[kind_name(s.kind)];
  switch (s.kind) {
    case Stmt::Decl:
      report->declared.insert(s.decl_id);
      for (const Expr& dim : s.decl_type.dims)
        scan_expr(dim, row_names, report);
      if (s.has_init) scan_expr(s.init, row_names, report);
      break;
    case Stmt::Assignment: {
      report->assigned.insert(s.lhs);
      bool row_varying = false;
      for (const Expr& index : s.lhs_idx) {
        scan_expr(index, row_names, report);
        row_varying = row_varying || references_any(index, row_names);
      }
      scan_expr(s.rhs, row_names, report);
      if (!s.lhs_idx.empty() && row_varying)
        report->indexed_writes.insert(s.lhs + " <- " + expr(s.rhs));
      break;
    }
    case Stmt::For:
      scan_expr(s.lower, row_names, report);
      scan_expr(s.upper, row_names, report);
      report->loops.push_back(path + ": for " + s.loopvar + " in " +
                              expr(s.lower) + ':' + expr(s.upper));
      break;
    case Stmt::While:
      scan_expr(s.cond, row_names, report);
      report->whiles.push_back(path + ": while " + expr(s.cond) +
                               (s.cond.data_only ? " [data]" : " [autodiff]"));
      break;
    case Stmt::IfElse:
      scan_expr(s.cond, row_names, report);
      break;
    case Stmt::TargetPE:
      scan_expr(s.target, row_names, report);
      break;
    case Stmt::NRFunApp:
      for (const Expr& arg : s.fn_args) scan_expr(arg, row_names, report);
      break;
    case Stmt::Return:
      if (s.has_init) scan_expr(s.rhs, row_names, report);
      break;
    default:
      break;
  }
  for (size_t k = 0; k < s.body.size(); ++k)
    scan_stmt(s.body[k], row_names,
              path + '/' + kind_name(s.kind) + '[' + std::to_string(k) + ']',
              report);
}

const Stmt* find_loop(const std::vector<Stmt>& stmts,
                      const std::string& loopvar) {
  for (const Stmt& stmt : stmts) {
    if (stmt.kind == Stmt::For && stmt.loopvar == loopvar) return &stmt;
    if (const Stmt* found = find_loop(stmt.body, loopvar)) return found;
  }
  return nullptr;
}

std::string direct_summary(const Stmt& stmt) {
  std::string out = kind_name(stmt.kind);
  if (stmt.kind == Stmt::Decl) out += ' ' + stmt.decl_id;
  if (stmt.kind == Stmt::Assignment) {
    out += ' ' + stmt.lhs;
    if (!stmt.lhs_idx.empty()) out += "[indexed]";
    out += " <- " + expr(stmt.rhs);
  }
  if (stmt.kind == Stmt::IfElse)
    out += " cond=" + expr(stmt.cond) +
           (stmt.cond.data_only ? " [data]" : " [autodiff]");
  if (stmt.kind == Stmt::For)
    out +=
        ' ' + stmt.loopvar + " in " + expr(stmt.lower) + ':' + expr(stmt.upper);
  if (stmt.kind == Stmt::While) out += " cond=" + expr(stmt.cond);
  if (!stmt.body.empty())
    out += " children=" + std::to_string(stmt.body.size());
  return out;
}

void print_set(const char* title, const std::set<std::string>& values) {
  std::printf("\n%s (%zu)\n", title, values.size());
  for (const std::string& value : values) std::printf("  %s\n", value.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: inspect_scan_mir MIR LOOPVAR [INDEX_VAR ...]\n");
    return 2;
  }
  const std::string loopvar = argv[2];
  try {
    const stanli::mir::Program program = stanli::decode_program(slurp(argv[1]));
    const Stmt* loop = find_loop(program.log_prob, loopvar);
    if (loop == nullptr) {
      std::fprintf(stderr, "loop %s not found in log_prob\n", loopvar.c_str());
      return 1;
    }

    std::printf("for %s in %s:%s, direct-body=%zu\n", loopvar.c_str(),
                expr(loop->lower).c_str(), expr(loop->upper).c_str(),
                loop->body.size());
    for (size_t k = 0; k < loop->body.size(); ++k) {
      std::printf("  body[%zu] %s\n", k, direct_summary(loop->body[k]).c_str());
      for (size_t j = 0; j < loop->body[k].body.size(); ++j)
        std::printf("    child[%zu] %s\n", j,
                    direct_summary(loop->body[k].body[j]).c_str());
    }

    // Inventory reads indexed by the selected iterator or aliases supplied
    // by the caller. This diagnostic does not prove dependency provenance or
    // control compiler eligibility, and has no model-specific identifier list.
    std::set<std::string> row_names{loopvar};
    for (int argument = 3; argument < argc; ++argument)
      row_names.insert(argv[argument]);
    Report report;
    for (size_t k = 0; k < loop->body.size(); ++k)
      scan_stmt(loop->body[k], row_names, "body[" + std::to_string(k) + ']',
                &report);

    std::printf("\nSTATEMENT CENSUS\n");
    for (const auto& item : report.statement_counts)
      std::printf("  %-12s %zu\n", item.first.c_str(), item.second);

    std::set<std::string> external_mutations;
    std::set_difference(
        report.assigned.begin(), report.assigned.end(), report.declared.begin(),
        report.declared.end(),
        std::inserter(external_mutations, external_mutations.begin()));
    std::set<std::string> recurrence_candidates;
    std::set_intersection(
        external_mutations.begin(), external_mutations.end(),
        report.reads.begin(), report.reads.end(),
        std::inserter(recurrence_candidates, recurrence_candidates.begin()));
    print_set("EXTERNAL MUTATIONS", external_mutations);
    print_set("CONSERVATIVE RECURRENCE CANDIDATES", recurrence_candidates);
    print_set("ROW-VARYING INDEXED READS", report.row_reads);
    print_set("ROW-VARYING INDEXED WRITES", report.indexed_writes);

    std::printf("\nNESTED FOR LOOPS (%zu)\n", report.loops.size());
    for (const std::string& item : report.loops)
      std::printf("  %s\n", item.c_str());
    std::printf("\nNESTED WHILE LOOPS (%zu)\n", report.whiles.size());
    for (const std::string& item : report.whiles)
      std::printf("  %s\n", item.c_str());
  } catch (const std::exception& error) {
    std::fprintf(stderr, "inspect_scan_mir: %s\n", error.what());
    return 1;
  }
  return 0;
}
