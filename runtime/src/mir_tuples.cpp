#include "mir_tuples.hpp"

#include "mir_reader_internal.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stanli {
namespace mir {
namespace detail {
namespace {

[[noreturn]] void malformed(const std::string& what) {
  throw MirError("mir: malformed tuple interface: " + what);
}

SizedType array_of(SizedType type, const std::vector<Expr>& outer) {
  if (outer.empty()) return type;
  if (type.base != "SArray") {
    type.elem_base = type.base;
    type.base = "SArray";
  }
  type.dims.insert(type.dims.begin(), outer.begin(), outer.end());
  return type;
}

void leaves_of(const TupleType& type, const std::string& base,
               const std::string& key, std::vector<Expr> outer,
               const std::vector<TupleSlot>& path,
               std::vector<TupleLeaf>& out) {
  if (!is_tuple(type.type)) {
    if (!type.components.empty()) malformed("components of a non-tuple type");
    out.push_back({"", base, path, array_of(type.type, outer), key});
    return;
  }
  if (type.components.empty()) malformed("a tuple without components");
  if (type.type.base == "SArray")
    outer.insert(outer.end(), type.type.dims.begin(), type.type.dims.end());
  else if (!type.type.dims.empty())
    malformed("a tuple with dimensions");
  for (size_t k = 0; k < type.components.size(); ++k) {
    std::vector<TupleSlot> step = path;
    step.push_back({outer.size(), static_cast<int>(k + 1)});
    leaves_of(type.components[k], base, key + "." + std::to_string(k + 1),
              outer, step, out);
  }
}

using SlotKey = std::vector<std::pair<size_t, int>>;

SlotKey slot_key(const std::vector<TupleSlot>& path) {
  SlotKey key;
  for (const TupleSlot& slot : path)
    key.emplace_back(slot.before, slot.component);
  return key;
}

void check_read_keys(const Stmt& s, const TupleInterface& tuples) {
  if (internal_name(s) == "FnReadData" && s.fn_args.size() >= 3 &&
      s.fn_args[0].kind == Expr::Var) {
    const TupleLeaf* leaf = tuples.leaf(s.fn_args[0].name);
    if (leaf &&
        (s.fn_args[2].kind != Expr::LitStr || s.fn_args[2].lit_s != leaf->key))
      malformed("data leaf " + leaf->variable + " is not read under its key " +
                leaf->key);
  }
  for (const Stmt& child : s.body) check_read_keys(child, tuples);
}

}  // namespace

void attach_tuple_interface(
    Program& program, std::map<std::string, TupleType> tuple_types,
    std::vector<std::pair<size_t, std::string>> tuple_inputs,
    std::vector<WireTupleLeaf> wire_leaves) {
  if (tuple_types.empty() && wire_leaves.empty()) return;
  auto tuples = std::make_shared<TupleInterface>();
  std::map<std::pair<std::string, SlotKey>, TupleLeaf> expected;
  std::map<std::string, std::vector<SlotKey>> order;
  for (const auto& [name, type] : tuple_types) {
    std::vector<TupleLeaf> leaves;
    leaves_of(type, name, name, {}, {}, leaves);
    for (TupleLeaf& leaf : leaves) {
      SlotKey key = slot_key(leaf.path);
      order[name].push_back(key);
      expected.emplace(std::make_pair(name, std::move(key)), std::move(leaf));
    }
  }
  std::map<std::pair<std::string, SlotKey>, std::string> named;
  for (WireTupleLeaf& wire : wire_leaves) {
    auto id = std::make_pair(wire.base, slot_key(wire.path));
    const auto found = expected.find(id);
    if (found == expected.end())
      malformed("leaf " + wire.variable + " is no leaf of a tuple variable " +
                wire.base);
    if (!named.emplace(id, wire.variable).second)
      malformed("two variables for one leaf of " + wire.base);
    TupleLeaf leaf = found->second;
    leaf.variable = wire.variable;
    if (!tuples->leaves.emplace(wire.variable, std::move(leaf)).second)
      malformed("leaf variable " + wire.variable + " named twice");
  }
  for (const auto& [name, keys] : order)
    for (const SlotKey& key : keys) {
      const auto found = named.find({name, key});
      if (found == named.end())
        malformed("a leaf of " + name + " has no variable");
      tuples->leaves_of[name].push_back(found->second);
    }
  std::vector<std::pair<std::string, SizedType>> inputs;
  size_t next = 0;
  for (size_t k = 0; k < program.input_vars.size(); ++k) {
    if (next < tuple_inputs.size() && tuple_inputs[next].first == k) {
      for (const std::string& leaf :
           tuples->leaves_of.at(tuple_inputs[next].second))
        inputs.emplace_back(leaf, tuples->leaves.at(leaf).type);
      ++next;
      continue;
    }
    inputs.push_back(std::move(program.input_vars[k]));
  }
  program.input_vars = std::move(inputs);
  for (const Stmt& s : program.prepare_data) check_read_keys(s, *tuples);
  program.tuples = std::move(tuples);
}

namespace {

// The functions computing each result of a decomposition, in the order the
// tuple holds them.
const std::vector<const char*>* decomposition_parts(const Expr& call) {
  static const std::map<std::string, std::vector<const char*>> parts = {
      {"qr", {"qr_Q", "qr_R"}},
      {"qr_thin", {"qr_thin_Q", "qr_thin_R"}},
      {"eigendecompose_sym", {"eigenvectors_sym", "eigenvalues_sym"}}};
  if (call.kind != Expr::FunApp || call.fn_lib != Expr::Lib::StanLib ||
      call.args.size() != 1 || !is_unsized(call.args[0], UnsizedLeaf::Matrix))
    return nullptr;
  const auto found = parts.find(call.name);
  return found == parts.end() ? nullptr : &found->second;
}

Stmt split_decomposition(const Stmt& s, const std::vector<const char*>& parts) {
  if (s.fn_args.size() != parts.size())
    throw MirError("mir: " + s.rhs.name + " returns " +
                   std::to_string(parts.size()) + " values, not " +
                   std::to_string(s.fn_args.size()));
  Stmt out;
  out.kind = Stmt::SList;
  Expr argument = s.rhs.args[0];
  if (argument.kind != Expr::Var) {
    // Evaluate the argument once, into a local beside the targets.
    const std::string local = s.fn_args[0].name + ".argument";
    out.kind = Stmt::Block;
    Stmt decl;
    decl.kind = Stmt::Decl;
    decl.decl_id = local;
    decl.decl_type = unsized_declaration(argument.unsized, "");
    decl.decl_data_only = argument.data_only;
    out.body.push_back(std::move(decl));
    Stmt assign;
    assign.kind = Stmt::Assignment;
    assign.lhs = local;
    assign.rhs = argument;
    out.body.push_back(std::move(assign));
    Expr var;
    var.kind = Expr::Var;
    var.name = local;
    var.unsized = argument.unsized;
    var.data_only = argument.data_only;
    argument = std::move(var);
  }
  for (size_t k = 0; k < parts.size(); ++k) {
    Stmt assign;
    assign.kind = Stmt::Assignment;
    assign.lhs = s.fn_args[k].name;
    assign.rhs.kind = Expr::FunApp;
    assign.rhs.name = parts[k];
    assign.rhs.args = {argument};
    assign.rhs.unsized = s.fn_args[k].unsized;
    assign.rhs.data_only = argument.data_only;
    assign.raw = s.raw;
    out.body.push_back(std::move(assign));
  }
  return out;
}

// The declarations seen so far, by name. The compiler gives each
// MultiAssign target a fresh name declared before it, so the last
// declaration of the name is the target's.
using Declared = std::map<std::string, std::pair<UnsizedView, bool>>;

void finish_multi_assigns(Stmt& s, Declared& declared) {
  if (s.kind == Stmt::Decl)
    declared[s.decl_id] = {declaration_view(s.decl_type), s.decl_data_only};
  if (s.kind == Stmt::For) declared[s.loopvar] = {{0, UnsizedLeaf::Int}, true};
  for (Stmt& child : s.body) finish_multi_assigns(child, declared);
  if (s.kind != Stmt::MultiAssign) return;
  for (Expr& target : s.fn_args) {
    const auto found = declared.find(target.name);
    if (found == declared.end()) continue;  // the validator reports it
    target.unsized = found->second.first;
    target.data_only = found->second.second;
  }
  if (const auto* parts = decomposition_parts(s.rhs))
    s = split_decomposition(s, *parts);
}

}  // namespace

void finish_multi_assigns(Program& program) {
  Declared data;
  for (const auto& [name, type] : program.input_vars)
    data[name] = {declaration_view(type), true};
  for (Stmt& s : program.prepare_data) finish_multi_assigns(s, data);
  for (auto* body : {&program.log_prob, &program.generate_quantities,
                     &program.transform_inits}) {
    Declared declared = data;
    for (Stmt& s : *body) finish_multi_assigns(s, declared);
  }
  for (FunDef& f : program.fun_defs) {
    Declared declared;
    for (size_t k = 0; k < f.arg_names.size() && k < f.arg_views.size(); ++k)
      declared[f.arg_names[k]] = {
          f.arg_views[k], k < f.arg_data_only.size() && f.arg_data_only[k]};
    for (Stmt& s : f.body) finish_multi_assigns(s, declared);
  }
}

}  // namespace detail
}  // namespace mir
}  // namespace stanli
