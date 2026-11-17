// Tuples exist only in the MIR the compiler sends. This pass rewrites them
// into ordinary variables before any engine runs, so a tuple costs nothing at
// run time and every engine supports it alike.
//
// A tuple variable becomes one variable per leaf, and an array of tuples
// becomes a tuple of arrays: `array[N] tuple(real, vector[2]) p` is `p.1`, an
// array[N] real, and `p.2`, an array[N] vector[2], so `p[i].2[j]` is
// `p.2[i, j]`. A leaf is named by its CmdStan data key, which contains a dot
// and so cannot collide with a Stan identifier; nothing reads the structure
// back out of a name, it is recorded in Program::tuple_leaves. A function that
// returns a tuple is inlined at each call; a tuple argument becomes one
// argument per leaf.
#include "mir_reader_internal.hpp"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stanli {
namespace mir {
namespace detail {
namespace {

[[noreturn]] void unsupported(const std::string& what) {
  throw MirError("mir: " + what + " is not supported yet");
}

bool is_tuple(const UnsizedView& view) {
  return view.leaf == UnsizedLeaf::Tuple;
}

bool is_tuple(const Expr& e) { return is_tuple(e.unsized); }

// A value of tuple type broken into its leaves.
struct Value {
  Expr leaf;  // when components is empty
  std::vector<Value> components;
};

template <class F>
void for_each_leaf(Value& value, F f) {
  if (value.components.empty()) return f(value.leaf);
  for (Value& component : value.components) for_each_leaf(component, f);
}

std::vector<Expr> leaves_of(const Value& value) {
  std::vector<Expr> out;
  Value copy = value;
  for_each_leaf(copy, [&](Expr& leaf) { out.push_back(leaf); });
  return out;
}

// The views of a tuple view's leaves, depth first, with the outer array
// depth moved onto each leaf.
void leaf_views(const UnsizedType& view, uint8_t outer,
                std::vector<UnsizedView>& out) {
  if (!is_tuple(view)) {
    UnsizedView leaf = view;
    leaf.depth = static_cast<uint8_t>(leaf.depth + outer);
    out.push_back(leaf);
    return;
  }
  for (const UnsizedType& component : view.components)
    leaf_views(component, static_cast<uint8_t>(outer + view.depth), out);
}

std::vector<UnsizedView> leaf_views(const UnsizedType& view) {
  std::vector<UnsizedView> out;
  leaf_views(view, 0, out);
  return out;
}

Expr literal_int(long value) {
  Expr e;
  e.kind = Expr::LitInt;
  e.lit_i = value;
  e.lit = static_cast<double>(value);
  e.unsized = {0, UnsizedLeaf::Int};
  e.data_only = true;
  return e;
}

Expr variable(const std::string& name, UnsizedType view, bool data_only) {
  Expr e;
  e.kind = Expr::Var;
  e.name = name;
  set_unsized_type(e, std::move(view));
  e.data_only = data_only;
  return e;
}

Stmt assignment(const std::string& lhs, std::vector<Expr> indices, Expr rhs) {
  Stmt s;
  s.kind = Stmt::Assignment;
  s.lhs = lhs;
  s.lhs_idx = std::move(indices);
  s.rhs = std::move(rhs);
  return s;
}

Stmt declaration(const std::string& name, SizedType type, bool data_only) {
  Stmt s;
  s.kind = Stmt::Decl;
  s.decl_id = name;
  s.decl_type = std::move(type);
  s.decl_data_only = data_only;
  return s;
}

void set_decl_leaf_data_only(Stmt& s, const std::vector<bool>& levels) {
  if (!levels.empty()) s.tuple.fill().decl_leaf_data_only = levels;
}

// The view of `view` indexed by `indices`, each addressing the next
// dimension: a single index drops its dimension, any other keeps it.
UnsizedView indexed_view(const UnsizedView& view,
                         const std::vector<Expr>& indices) {
  enum Dim { Array, Row, Column, Element };
  std::vector<Dim> dims(view.depth, Array);
  if (view.leaf == UnsizedLeaf::Matrix) {
    dims.push_back(Row);
    dims.push_back(Column);
  } else if (view.leaf == UnsizedLeaf::Vector ||
             view.leaf == UnsizedLeaf::RowVector) {
    dims.push_back(Element);
  }
  if (indices.size() > dims.size())
    throw MirError("mir: more indices than a tuple leaf has dimensions");
  std::vector<Dim> kept;
  for (size_t k = 0; k < dims.size(); ++k)
    if (k >= indices.size() || indices[k].kind != Expr::IndexSingle)
      kept.push_back(dims[k]);
  UnsizedView out = view;
  out.depth = 0;
  bool row = false, column = false, element = false;
  for (Dim d : kept) {
    if (d == Array) ++out.depth;
    row = row || d == Row;
    column = column || d == Column;
    element = element || d == Element;
  }
  if (view.leaf == UnsizedLeaf::Matrix)
    out.leaf = row && column ? UnsizedLeaf::Matrix
               : row         ? UnsizedLeaf::Vector
               : column      ? UnsizedLeaf::RowVector
                             : UnsizedLeaf::Real;
  else if ((view.leaf == UnsizedLeaf::Vector ||
            view.leaf == UnsizedLeaf::RowVector) &&
           !element)
    out.leaf = UnsizedLeaf::Real;
  return out;
}

bool all_single(const std::vector<Expr>& indices, size_t begin) {
  for (size_t k = begin; k < indices.size(); ++k)
    if (indices[k].kind != Expr::IndexSingle) return false;
  return true;
}

// `leaf[indices]`. Indices following only single ones join the same index
// list, since `a[i][j]` is `a[i, j]` then.
Expr index_leaf(const Expr& leaf, const std::vector<Expr>& indices) {
  if (indices.empty()) return leaf;
  Expr out;
  out.kind = Expr::Indexed;
  out.data_only = leaf.data_only;
  if (leaf.kind == Expr::Indexed && all_single(leaf.args, 1)) {
    out.args = leaf.args;
    std::vector<Expr> joined(leaf.args.begin() + 1, leaf.args.end());
    joined.insert(joined.end(), indices.begin(), indices.end());
    out.unsized = indexed_view(leaf.args[0].unsized, joined);
  } else {
    out.args = {leaf};
    out.unsized = indexed_view(leaf.unsized, indices);
  }
  out.args.insert(out.args.end(), indices.begin(), indices.end());
  return out;
}

bool effectful(const Expr& e) {
  if (e.kind == Expr::FunApp &&
      (e.fn_lib == Expr::Lib::UserDefined || e.fn_suffix == FnSuffix::Rng))
    return true;
  for (const Expr& arg : e.args)
    if (effectful(arg)) return true;
  return false;
}

bool mentions(const Expr& e, const std::set<std::string>& names) {
  if (e.kind == Expr::Var && names.count(e.name)) return true;
  for (const Expr& arg : e.args)
    if (mentions(arg, names)) return true;
  return false;
}

// The leaves of a tuple type, each with its SoA type and slot path.
struct Leaf {
  std::string name;
  SizedType type;
  UnsizedView view;
  std::vector<TupleLeaf::Slot> slots;
};

SizedType array_of(SizedType type, const std::vector<Expr>& outer) {
  if (outer.empty()) return type;
  if (!type.base.empty() && type.base != "SArray") {
    type.elem_base = type.base;
    type.base = "SArray";
  }
  type.dims.insert(type.dims.begin(), outer.begin(), outer.end());
  return type;
}

void sized_leaves(const SizedType& type, const std::string& name,
                  std::vector<Expr> outer, std::vector<TupleLeaf::Slot> slots,
                  std::vector<Leaf>& out) {
  if (!mir::is_tuple(type)) {
    SizedType leaf = array_of(type, outer);
    out.push_back({name, leaf, {}, std::move(slots)});
    return;
  }
  if (type.base == "SArray")
    outer.insert(outer.end(), type.dims.begin(), type.dims.end());
  const std::vector<SizedType>& parts = components(type);
  for (size_t k = 0; k < parts.size(); ++k) {
    std::vector<TupleLeaf::Slot> path = slots;
    path.push_back({outer.size(), static_cast<int>(k + 1)});
    sized_leaves(parts[k], name + "." + std::to_string(k + 1), outer,
                 std::move(path), out);
  }
}

void view_leaves(const UnsizedType& view, const std::string& name,
                 uint8_t outer, std::vector<TupleLeaf::Slot> slots,
                 std::vector<Leaf>& out) {
  if (!is_tuple(view)) {
    UnsizedView leaf = view;
    leaf.depth = static_cast<uint8_t>(leaf.depth + outer);
    out.push_back(
        {name, unsized_declaration(leaf, ""), leaf, std::move(slots)});
    return;
  }
  const uint8_t depth = static_cast<uint8_t>(outer + view.depth);
  for (size_t k = 0; k < view.components.size(); ++k) {
    std::vector<TupleLeaf::Slot> path = slots;
    path.push_back({depth, static_cast<int>(k + 1)});
    view_leaves(view.components[k], name + "." + std::to_string(k + 1), depth,
                std::move(path), out);
  }
}

UnsizedView declared_leaf_view(const SizedType& type) {
  if (type.base.empty()) return type.unsized;
  const std::string& base = type.base == "SArray" ? type.elem_base : type.base;
  UnsizedView view;
  size_t rank = 0;
  if (base == "SInt") view.leaf = UnsizedLeaf::Int;
  if (base == "SReal") view.leaf = UnsizedLeaf::Real;
  if (base == "SComplex") view.leaf = UnsizedLeaf::Complex;
  if (base == "SVector") view.leaf = UnsizedLeaf::Vector, rank = 1;
  if (base == "SRowVector") view.leaf = UnsizedLeaf::RowVector, rank = 1;
  if (base == "SMatrix") view.leaf = UnsizedLeaf::Matrix, rank = 2;
  if (type.base == "SArray")
    view.depth = static_cast<uint8_t>(type.dims.size() - rank);
  return view;
}

// The tree of a tuple variable's leaf variables.
Value leaf_tree(const UnsizedType& view, std::vector<Leaf>::const_iterator& at,
                std::vector<bool>::const_iterator& data_only) {
  Value value;
  if (!is_tuple(view)) {
    value.leaf = variable(at->name, at->view, *data_only);
    ++at;
    ++data_only;
    return value;
  }
  for (const UnsizedType& component : view.components)
    value.components.push_back(leaf_tree(component, at, data_only));
  return value;
}

class TupleEliminator {
 public:
  explicit TupleEliminator(Program& program) : program_(program) {
    for (const FunDef& f : program.fun_defs)
      if (returns_tuple(f)) inlined_[function_key(f)] = &f;
  }

  void run() {
    // A function returning a tuple is rewritten at each call it is inlined
    // into, and then dropped.
    std::vector<FunDef> kept;
    for (FunDef& f : program_.fun_defs) {
      if (inlined_.count(function_key(f))) continue;
      rewrite_function(f);
      kept.push_back(std::move(f));
    }
    std::vector<std::pair<std::string, SizedType>> inputs;
    Scope globals(scopes_);
    for (auto& [name, type] : program_.input_vars) {
      if (!mir::is_tuple(type)) {
        inputs.emplace_back(name, type);
        continue;
      }
      // Data is read under the leaves' names, which are its JSON keys.
      for (const Leaf& leaf : bind(name, type, {}, true))
        inputs.emplace_back(leaf.name, leaf.type);
    }
    program_.input_vars = std::move(inputs);
    program_.prepare_data = rewrite_block(program_.prepare_data);
    // Transformed data stays visible in the later blocks.
    for (auto* block : {&program_.log_prob, &program_.transform_inits}) {
      Scope local(scopes_);
      *block = rewrite_block(*block);
    }
    Scope local(scopes_);
    rewrite_write_array();
    program_.fun_defs = std::move(kept);
  }

  // write_array's sections are rewritten one at a time so each keeps its
  // first statement.
  void rewrite_write_array() {
    std::vector<Stmt>& body = program_.generate_quantities;
    size_t* begins[] = {&program_.transformed_parameters_begin,
                        &program_.generated_quantities_begin};
    const size_t ends[] = {*begins[0], *begins[1], body.size()};
    std::vector<Stmt> out;
    size_t begin = 0;
    for (size_t k = 0; k < 3; ++k) {
      for (Stmt& s : rewrite_block(
               std::vector<Stmt>(body.begin() + begin, body.begin() + ends[k])))
        out.push_back(std::move(s));
      begin = ends[k];
      if (k < 2) *begins[k] = out.size();
    }
    body = std::move(out);
  }

  // The bound tuple, or null when `name` is not a tuple here.
  const Value* lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) return found->second ? &*found->second : nullptr;
    }
    return nullptr;
  }

 private:
  using Frame = std::map<std::string, std::optional<Value>>;

  struct Scope {
    explicit Scope(std::vector<Frame>& scopes) : scopes_(scopes) {
      scopes_.emplace_back();
    }
    ~Scope() { scopes_.pop_back(); }
    std::vector<Frame>& scopes_;
  };

  static bool returns_tuple(const FunDef& f) {
    bool found = false;
    std::function<void(const Stmt&)> visit = [&](const Stmt& s) {
      if (s.kind == Stmt::Return && s.has_init && is_tuple(s.rhs)) found = true;
      for (const Stmt& child : s.body) visit(child);
    };
    for (const Stmt& s : f.body) visit(s);
    return found;
  }

  std::string fresh() { return "tuple." + std::to_string(++counter_); }

  // A fresh name for a temporary that holds part of a statement's work.
  std::string temporary() {
    std::string name = fresh();
    temps_.insert(name);
    return name;
  }

  // Bind a tuple variable and return its leaves to declare.
  std::vector<Leaf> bind(const std::string& name, const SizedType& type,
                         std::vector<bool> data_only, bool default_data) {
    std::vector<Leaf> leaves;
    if (type.base.empty())
      view_leaves(unsized_type(type), name, 0, {}, leaves);
    else
      sized_leaves(type, name, {}, {}, leaves);
    for (Leaf& leaf : leaves) {
      if (leaf.view.leaf == UnsizedLeaf::Unknown)
        leaf.view = declared_leaf_view(leaf.type);
      program_.tuple_leaves[leaf.name] = TupleLeaf{name, leaf.slots};
      if (temps_.count(name)) temps_.insert(leaf.name);
    }
    if (data_only.size() != leaves.size())
      data_only.assign(leaves.size(), default_data);
    const UnsizedType view =
        type.base.empty() ? unsized_type(type) : tuple_view(type);
    auto at = leaves.cbegin();
    auto level = data_only.cbegin();
    scopes_.back()[name] = leaf_tree(view, at, level);
    for (size_t k = 0; k < leaves.size(); ++k)
      leaf_data_only_[leaves[k].name] = data_only[k];
    return leaves;
  }

  static UnsizedType tuple_view(const SizedType& type) {
    if (!mir::is_tuple(type)) return declared_leaf_view(type);
    UnsizedType view;
    view.leaf = UnsizedLeaf::Tuple;
    if (type.base == "SArray")
      view.depth = static_cast<uint8_t>(type.dims.size());
    for (const SizedType& component : components(type))
      view.components.push_back(tuple_view(component));
    return view;
  }

  void shadow(const std::string& name) { scopes_.back()[name] = std::nullopt; }

  // ---- expressions -------------------------------------------------------

  std::vector<Expr> rewrite_all(const std::vector<Expr>& es,
                                std::vector<Stmt>& pre) {
    std::vector<Expr> out;
    out.reserve(es.size());
    for (const Expr& e : es) out.push_back(rewrite(e, pre));
    return out;
  }

  // Arguments of a call, with each tuple argument spread into its leaves.
  std::vector<Expr> spread(const std::vector<Expr>& args,
                           std::vector<Stmt>& pre) {
    std::vector<Expr> out;
    for (const Expr& arg : args) {
      if (!is_tuple(arg)) {
        out.push_back(rewrite(arg, pre));
        continue;
      }
      for (Expr& leaf : leaves_of(value_of(arg, pre)))
        out.push_back(std::move(leaf));
    }
    return out;
  }

  // A non-tuple expression with every tuple inside it gone.
  Expr rewrite(const Expr& e, std::vector<Stmt>& pre) {
    if (is_tuple(e)) unsupported("a tuple value in this position");
    if (e.kind == Expr::TupleProjection) {
      Value value = value_of(e, pre);
      Expr leaf = std::move(value.leaf);
      leaf.unsized = e.unsized;
      return leaf;
    }
    if (e.kind == Expr::Indexed && !e.args.empty() && is_tuple(e.args[0]))
      throw MirError("mir: indexing a tuple gave a non-tuple value");
    if (e.kind == Expr::Indexed && !e.args.empty() &&
        e.args[0].kind == Expr::TupleProjection) {
      // `p[i].2[j]` is `p.2[i, j]`, the form an ordinary array is indexed in.
      Expr out =
          index_leaf(rewrite(e.args[0], pre),
                     rewrite_all({e.args.begin() + 1, e.args.end()}, pre));
      out.unsized = e.unsized;
      out.data_only = e.data_only;
      return out;
    }
    Expr out = e;
    if (e.kind == Expr::Var && e.fn_type) {
      std::vector<UnsizedType> args;
      for (const UnsizedType& view : e.fn_type->args)
        for (UnsizedView leaf : leaf_views(view)) args.push_back(leaf);
      out.fn_type->args = std::move(args);
      return out;
    }
    if (e.kind != Expr::FunApp) {
      out.args = rewrite_all(e.args, pre);
      return out;
    }
    bool tuple_arg = false;
    for (const Expr& arg : e.args) tuple_arg = tuple_arg || is_tuple(arg);
    if (!tuple_arg) {
      out.args = rewrite_all(e.args, pre);
      return out;
    }
    const bool length =
        (e.fn_lib == Expr::Lib::Internal && e.name == "FnLength") ||
        (e.fn_lib == Expr::Lib::StanLib && e.name == "size");
    if (length && e.args.size() == 1) {
      // An array of tuples is as long as each of its leaves.
      out.args = {leaves_of(value_of(e.args[0], pre)).at(0)};
      return out;
    }
    if (e.fn_lib == Expr::Lib::StanLib && is_reduce_sum(e) &&
        e.args.size() > 1 && is_tuple(e.args[1]))
      unsupported("slicing an array of tuples in reduce_sum");
    if (e.fn_lib == Expr::Lib::UserDefined || higher_order_call(e)) {
      out.args = spread(e.args, pre);
      return out;
    }
    // No engine has the remaining library functions taking a tuple (the
    // laplace family); leave the call for the engine to refuse by name, so
    // only the part of the model that calls it fails.
    return e;
  }

  Expr hoist(Expr value, std::vector<Stmt>& pre) {
    const std::string name = temporary();
    const UnsizedView view = value.unsized;
    const bool data_only = value.data_only;
    pre.push_back(declaration(name, unsized_declaration(view, ""), data_only));
    pre.push_back(assignment(name, {}, std::move(value)));
    return variable(name, view, data_only);
  }

  // The leaves of `e`, which may be of tuple type, with any statement that
  // must run first appended to `pre`.
  Value value_of(const Expr& e, std::vector<Stmt>& pre) {
    Value value = component_values(e, pre);
    if (e.promoted && is_tuple(e)) {
      // A promotion retypes each leaf to the promoted component.
      std::vector<UnsizedView> views = leaf_views(unsized_type(e));
      const std::vector<bool>& levels = leaf_data_only(e);
      size_t k = 0;
      for_each_leaf(value, [&](Expr& leaf) {
        if (k < views.size()) leaf.unsized = views[k];
        if (k < levels.size()) leaf.data_only = levels[k];
        leaf.promoted = true;
        ++k;
      });
    }
    return value;
  }

  Value component_values(const Expr& e, std::vector<Stmt>& pre) {
    Value value;
    if (!is_tuple(e) && e.kind != Expr::TupleProjection) {
      value.leaf = rewrite(e, pre);
      return value;
    }
    switch (e.kind) {
      case Expr::Var: {
        const Value* bound = lookup(e.name);
        if (!bound) throw MirError("mir: unknown tuple variable " + e.name);
        return *bound;
      }
      case Expr::TupleProjection: {
        const Expr& tuple = e.args.at(0);
        const bool direct = tuple.kind == Expr::FunApp &&
                            tuple.fn_lib == Expr::Lib::Internal &&
                            tuple.name == "FnMakeTuple";
        if (direct && e.component >= 1 &&
            e.component <= (int)tuple.args.size()) {
          // Every component still runs, in order, for its draws and effects.
          Value picked;
          for (size_t k = 0; k < tuple.args.size(); ++k) {
            Value part = value_of(tuple.args[k], pre);
            if (effectful(tuple.args[k]))
              part = hoist_value(std::move(part), tuple.args[k], pre);
            if (k + 1 == (size_t)e.component) picked = std::move(part);
          }
          return picked;
        }
        Value whole = value_of(tuple, pre);
        if (e.component < 1 || e.component > (int)whole.components.size())
          throw MirError("mir: tuple projection out of range");
        return whole.components[e.component - 1];
      }
      case Expr::Indexed: {
        Value base = value_of(e.args.at(0), pre);
        std::vector<Expr> indices;
        for (size_t k = 1; k < e.args.size(); ++k) {
          Expr index = e.args[k];
          for (Expr& bound : index.args) {
            bound = rewrite(bound, pre);
            if (effectful(bound)) bound = hoist(std::move(bound), pre);
          }
          indices.push_back(std::move(index));
        }
        for_each_leaf(base,
                      [&](Expr& leaf) { leaf = index_leaf(leaf, indices); });
        return base;
      }
      case Expr::FunApp:
        if (e.fn_lib == Expr::Lib::Internal && e.name == "FnMakeTuple") {
          for (const Expr& arg : e.args)
            value.components.push_back(value_of(arg, pre));
          return value;
        }
        if (e.fn_lib == Expr::Lib::Internal && e.name == "FnMakeArray")
          return array_of_tuples(e, pre);
        if (e.fn_lib == Expr::Lib::UserDefined) return inline_call(e, pre);
        if (e.fn_lib == Expr::Lib::StanLib) return library_parts(e, pre);
        unsupported("calling " + e.name + ", which returns a tuple,");
      case Expr::TernaryIf: {
        // Each branch's statements run only when it is taken.
        Expr cond = rewrite(e.args.at(0), pre);
        Value result = temporaries(e, pre);
        Stmt branch;
        branch.kind = Stmt::IfElse;
        branch.cond = std::move(cond);
        for (size_t k = 1; k < 3; ++k) {
          Stmt arm;
          arm.kind = Stmt::Block;
          assign_leaves(result, value_of(e.args.at(k), arm.body), arm.body);
          branch.body.push_back(std::move(arm));
        }
        pre.push_back(std::move(branch));
        return result;
      }
      default:
        unsupported("this kind of tuple expression");
    }
  }

  // A library function returning a tuple of real matrices and vectors is
  // the functions that compute each part.
  Value library_parts(const Expr& e, std::vector<Stmt>& pre) {
    static const std::map<std::string, std::vector<const char*>> parts = {
        {"qr", {"qr_Q", "qr_R"}},
        {"qr_thin", {"qr_thin_Q", "qr_thin_R"}},
        {"eigendecompose_sym", {"eigenvectors_sym", "eigenvalues_sym"}}};
    const auto found = parts.find(e.name);
    if (found == parts.end() || e.args.size() != 1 ||
        !is_unsized(e.args[0], UnsizedLeaf::Matrix))
      unsupported("calling " + e.name + ", which returns a tuple,");
    Expr matrix = rewrite(e.args[0], pre);
    if (effectful(matrix)) matrix = hoist(std::move(matrix), pre);
    const std::vector<UnsizedView> views = leaf_views(unsized_type(e));
    Value value;
    for (size_t k = 0; k < found->second.size(); ++k) {
      Value part;
      part.leaf.kind = Expr::FunApp;
      part.leaf.name = found->second[k];
      part.leaf.args = {matrix};
      part.leaf.unsized = views.at(k);
      part.leaf.data_only = matrix.data_only;
      value.components.push_back(std::move(part));
    }
    return value;
  }

  // `{t1, t2}`: an array of each leaf of the elements.
  Value array_of_tuples(const Expr& e, std::vector<Stmt>& pre) {
    std::vector<std::vector<Expr>> elements;
    for (const Expr& arg : e.args)
      elements.push_back(leaves_of(value_of(arg, pre)));
    const std::vector<UnsizedView> views = leaf_views(unsized_type(e));
    Value result = shape_of(unsized_type(e));
    size_t k = 0;
    for_each_leaf(result, [&](Expr& leaf) {
      leaf.kind = Expr::FunApp;
      leaf.fn_lib = Expr::Lib::Internal;
      leaf.name = "FnMakeArray";
      leaf.unsized = views.at(k);
      leaf.data_only = true;
      for (std::vector<Expr>& element : elements) {
        if (element.size() != views.size())
          throw MirError("mir: array elements of different tuple types");
        leaf.data_only = leaf.data_only && element[k].data_only;
        leaf.args.push_back(std::move(element[k]));
      }
      ++k;
    });
    return result;
  }

  // Temporaries for a tuple value of `e`'s type.
  Value temporaries(const Expr& e, std::vector<Stmt>& pre) {
    const UnsizedType type = unsized_type(e);
    std::vector<UnsizedView> views = leaf_views(type);
    const std::vector<bool>& levels = leaf_data_only(e);
    Value tree = shape_of(type);
    size_t k = 0;
    for_each_leaf(tree, [&](Expr& leaf) {
      const std::string name = temporary();
      const bool data_only = k < levels.size() ? levels[k] : false;
      pre.push_back(
          declaration(name, unsized_declaration(views[k], ""), data_only));
      leaf = variable(name, views[k], data_only);
      ++k;
    });
    return tree;
  }

  static Value shape_of(const UnsizedType& view) {
    Value value;
    if (!is_tuple(view)) return value;
    for (const UnsizedType& component : view.components)
      value.components.push_back(shape_of(component));
    if (value.components.empty()) throw MirError("mir: empty tuple type");
    return value;
  }

  // Evaluate a value once, into temporaries, for its effects or reuse.
  Value hoist_value(Value value, const Expr& e, std::vector<Stmt>& pre) {
    if (value.components.empty()) {
      value.leaf = hoist(std::move(value.leaf), pre);
      return value;
    }
    Value result = temporaries(e, pre);
    assign_leaves(result, value, pre);
    return result;
  }

  void assign_leaves(const Value& target, const Value& value,
                     std::vector<Stmt>& out) {
    std::vector<Expr> lhs = leaves_of(target), rhs = leaves_of(value);
    if (lhs.size() != rhs.size())
      throw MirError("mir: tuple assignment shapes disagree");
    for (size_t k = 0; k < lhs.size(); ++k)
      out.push_back(store(lhs[k], rhs[k]));
  }

  // `target = value`, where target is a leaf variable, possibly indexed.
  static Stmt store(const Expr& target, Expr value) {
    if (target.kind == Expr::Var)
      return assignment(target.name, {}, std::move(value));
    if (target.kind == Expr::Indexed && target.args.at(0).kind == Expr::Var)
      return assignment(
          target.args[0].name,
          std::vector<Expr>(target.args.begin() + 1, target.args.end()),
          std::move(value));
    unsupported("assigning through nested indexing of a tuple");
  }

  // ---- functions ---------------------------------------------------------

  void rewrite_function(FunDef& f) {
    Scope scope(scopes_);
    std::vector<std::string> names;
    std::vector<UnsizedType> views;
    std::vector<bool> data_only;
    for (size_t i = 0; i < f.arg_names.size(); ++i) {
      if (!is_tuple(f.arg_views[i])) {
        shadow(f.arg_names[i]);
        names.push_back(f.arg_names[i]);
        views.push_back(f.arg_views[i]);
        data_only.push_back(f.arg_data_only[i]);
        continue;
      }
      SizedType type = unsized_declaration(f.arg_views[i], "");
      std::vector<bool> levels = i < f.arg_leaf_data_only.size()
                                     ? f.arg_leaf_data_only[i]
                                     : std::vector<bool>{};
      for (const Leaf& leaf : bind(f.arg_names[i], type, levels, false)) {
        names.push_back(leaf.name);
        views.push_back(leaf.view);
        data_only.push_back(leaf_data_only_[leaf.name]);
      }
    }
    f.arg_names = std::move(names);
    f.arg_views = std::move(views);
    f.arg_data_only = std::move(data_only);
    f.arg_leaf_data_only.assign(f.arg_names.size(), {});
    f.body = rewrite_block(f.body);
  }

  // A body's own names, renamed apart from the caller's.
  static void rename(Stmt& s, const std::string& prefix) {
    for (std::string* name : {&s.decl_id, &s.loopvar, &s.lhs})
      if (!name->empty()) *name = prefix + *name;
    for_each_expr(s, [&](Expr& e) { rename(e, prefix); });
    for (Stmt& child : s.body) rename(child, prefix);
  }

  static void rename(Expr& e, const std::string& prefix) {
    if (e.kind == Expr::Var && !e.fn_type) e.name = prefix + e.name;
    for (Expr& arg : e.args) rename(arg, prefix);
  }

  static bool contains_return(const Stmt& s) {
    if (s.kind == Stmt::Return) return true;
    for (const Stmt& child : s.body)
      if (contains_return(child)) return true;
    return false;
  }

  // A return becomes an assignment to the result and a break out of the
  // single pass the body runs in; a loop holding one also breaks when
  // `exited` is set.
  static Stmt returns_to_breaks(const Stmt& s, const Expr& result,
                                const std::string& exited) {
    Stmt leave;
    leave.kind = Stmt::Break;
    if (s.kind != Stmt::Return) {
      Stmt out = s;
      for (Stmt& child : out.body)
        child = returns_to_breaks(child, result, exited);
      if ((s.kind != Stmt::For && s.kind != Stmt::While) || !contains_return(s))
        return out;
      Stmt check;
      check.kind = Stmt::IfElse;
      check.cond = variable(exited, {0, UnsizedLeaf::Int}, true);
      check.body.push_back(leave);
      Stmt both;
      both.kind = Stmt::SList;
      both.body.push_back(std::move(out));
      both.body.push_back(std::move(check));
      return both;
    }
    if (!s.has_init) throw MirError("mir: return without a tuple value");
    Stmt block;
    block.kind = Stmt::Block;
    block.body.push_back(assignment(exited, {}, literal_int(1)));
    block.body.push_back(assignment(result.name, {}, s.rhs));
    block.body.push_back(leave);
    return block;
  }

  // `f(args)` for a function returning a tuple: the body runs in place, and
  // its result is the tuple variable returned.
  Value inline_call(const Expr& call, std::vector<Stmt>& pre) {
    const auto found = inlined_.find(function_key(call));
    if (found == inlined_.end() || found->second->body.empty())
      throw MirError("mir: no definition of " + call.name +
                     ", which returns a tuple");
    if (inline_depth_ > 32)
      unsupported("recursion in " + call.name + ", which returns a tuple,");
    ++inline_depth_;
    const FunDef& f = *found->second;
    if (call.args.size() != f.arg_names.size())
      throw MirError("mir: user function arity mismatch for " + call.name);
    const std::string prefix = fresh() + ".";
    std::vector<Stmt> stmts;
    const UnsizedType result_type = unsized_type(call);
    const Expr result = variable(prefix + "result", result_type, false);
    temps_.insert(result.name);
    temps_.insert(prefix + "exited");
    Stmt result_decl =
        declaration(result.name, unsized_declaration(result_type, ""), false);
    set_decl_leaf_data_only(result_decl, leaf_data_only(call));
    stmts.push_back(std::move(result_decl));
    const std::string exited = prefix + "exited";
    stmts.push_back(declaration(
        exited, unsized_declaration({0, UnsizedLeaf::Int}, ""), true));
    stmts.push_back(assignment(exited, {}, literal_int(0)));
    Stmt body;
    body.kind = Stmt::Block;
    for (size_t i = 0; i < f.arg_names.size(); ++i) {
      const Expr& actual = call.args[i];
      Stmt formal = declaration(prefix + f.arg_names[i],
                                unsized_declaration(unsized_type(actual), ""),
                                actual.data_only);
      set_decl_leaf_data_only(formal, leaf_data_only(actual));
      body.body.push_back(std::move(formal));
      body.body.push_back(assignment(prefix + f.arg_names[i], {}, actual));
    }
    Stmt pass;
    pass.kind = Stmt::For;
    pass.loopvar = prefix + "pass";
    pass.lower = literal_int(1);
    pass.upper = literal_int(1);
    Stmt inner;
    inner.kind = Stmt::Block;
    for (Stmt s : f.body) {
      rename(s, prefix);
      inner.body.push_back(returns_to_breaks(s, result, exited));
    }
    pass.body.push_back(std::move(inner));
    body.body.push_back(std::move(pass));
    stmts.push_back(std::move(body));
    // The actual arguments are the caller's: rename only the body.
    for (Stmt& s : stmts) rewrite_in_place(s, pre);
    --inline_depth_;
    return *lookup(result.name);
  }

  // ---- statements --------------------------------------------------------

  std::vector<Stmt> rewrite_block(const std::vector<Stmt>& body) {
    std::vector<Stmt> out;
    for (const Stmt& s : body) rewrite_stmt(s, out);
    return fold_reads(std::move(out));
  }

  // stanc reads a tuple parameter one component at a time into the declared
  // tuple; the leaf's declaration takes the read, as an ordinary
  // parameter's does.
  static std::vector<Stmt> fold_reads(std::vector<Stmt> body) {
    std::map<std::string, size_t> declared;
    std::vector<bool> drop(body.size(), false);
    for (size_t k = 0; k < body.size(); ++k) {
      Stmt& s = body[k];
      if (s.kind == Stmt::Decl && !s.has_init) declared[s.decl_id] = k;
      if (s.kind != Stmt::Assignment || !s.read_transform) continue;
      const auto decl = declared.find(s.lhs);
      if (!s.lhs_idx.empty() || decl == declared.end())
        unsupported("an array of tuples as a parameter");
      Stmt read = body[decl->second];
      read.has_init = true;
      read.init = std::move(s.rhs);
      read.read_transform = std::move(s.read_transform);
      read.read_dims = std::move(s.read_dims);
      drop[decl->second] = true;
      s = std::move(read);
    }
    std::vector<Stmt> out;
    for (size_t k = 0; k < body.size(); ++k)
      if (!drop[k]) out.push_back(std::move(body[k]));
    return out;
  }

  void rewrite_children(Stmt& s) {
    for (Stmt& child : s.body) {
      Scope scope(scopes_);
      if (s.kind == Stmt::For) shadow(s.loopvar);
      std::vector<Stmt> body;
      if (child.kind == Stmt::Block || child.kind == Stmt::SList) {
        body = rewrite_block(child.body);
        child.body = std::move(body);
      } else {
        rewrite_stmt(child, body);
        child = block_of(std::move(body));
      }
    }
  }

  static Stmt block_of(std::vector<Stmt> body) {
    if (body.size() == 1) return std::move(body[0]);
    Stmt block;
    block.kind = Stmt::Block;
    block.body = std::move(body);
    return block;
  }

  void rewrite_decl(const Stmt& s, std::vector<Stmt>& out) {
    const bool tuple =
        mir::is_tuple(s.decl_type) || is_tuple(s.decl_type.unsized);
    if (!tuple) {
      Stmt d = s;
      rewrite_exprs(d, out);
      shadow(d.decl_id);
      out.push_back(std::move(d));
      return;
    }
    if (s.read_transform) unsupported("reading a whole tuple parameter");
    std::optional<Value> init;
    if (s.has_init) init = value_of(s.init, out);
    SizedType type = s.decl_type;
    for_each_dim(type, [&](Expr& dim) { dim = rewrite(dim, out); });
    for (const Leaf& leaf :
         bind(s.decl_id, type, decl_leaf_data_only(s), s.decl_data_only))
      out.push_back(
          declaration(leaf.name, leaf.type, leaf_data_only_[leaf.name]));
    if (init) assign_leaves(*lookup(s.decl_id), *init, out);
  }

  // The leaves an assignment's left side names.
  Value target_of(const Stmt& s, std::vector<Stmt>& pre) {
    const Value* root = lookup(s.lhs);
    if (!root) throw MirError("mir: projection of non-tuple " + s.lhs);
    Value target = *root;
    std::vector<Expr> indices = rewrite_all(s.lhs_idx, pre);
    const auto apply = [&](const std::vector<Expr>& more) {
      for_each_leaf(target, [&](Expr& leaf) {
        if (leaf.kind == Expr::Indexed && !all_single(leaf.args, 1))
          unsupported("indexing after a multiple index of a tuple");
        leaf = index_leaf(leaf, more);
      });
    };
    apply(indices);
    for (const Stmt::TupleStep& step : lhs_steps(s)) {
      if (step.component < 1 || step.component > (int)target.components.size())
        throw MirError("mir: tuple assignment component out of range");
      Value picked = std::move(target.components[step.component - 1]);
      target = std::move(picked);
      apply(rewrite_all(step.indices, pre));
    }
    return target;
  }

  void rewrite_exprs(Stmt& s, std::vector<Stmt>& pre) {
    for_each_expr(s, [&](Expr& e) { e = rewrite(e, pre); });
  }

  static const std::string& base_name(const Expr& leaf) {
    return leaf.kind == Expr::Var ? leaf.name : leaf.args.at(0).name;
  }

  void rewrite_assignment(const Stmt& s, std::vector<Stmt>& out) {
    if (!lookup(s.lhs)) {
      if (!lhs_steps(s).empty())
        throw MirError("mir: projection of non-tuple " + s.lhs);
      Stmt a = s;
      rewrite_exprs(a, out);
      out.push_back(std::move(a));
      return;
    }
    Value target = target_of(s, out);
    if (s.read_transform) {
      if (!target.components.empty())
        throw MirError("mir: parameter read into a whole tuple");
      Stmt read = store(target.leaf, s.rhs);
      read.read_transform = s.read_transform;
      read.read_dims = s.read_dims;
      out.push_back(std::move(read));
      return;
    }
    Value value = value_of(s.rhs, out);
    std::set<std::string> written;
    for (const Expr& leaf : leaves_of(target)) written.insert(base_name(leaf));
    bool aliased = false;
    for (const Expr& leaf : leaves_of(value))
      aliased = aliased || mentions(leaf, written);
    if (aliased && !target.components.empty())
      value = hoist_value(std::move(value), s.rhs, out);
    assign_leaves(target, value, out);
  }

  // print and reject spell a tuple as stan_print does: (a,b).
  void spell(const Value& value, std::vector<Expr>& out) {
    if (value.components.empty()) {
      out.push_back(value.leaf);
      return;
    }
    const auto text = [](const char* s) {
      Expr e;
      e.kind = Expr::LitStr;
      e.lit_s = s;
      e.data_only = true;
      return e;
    };
    out.push_back(text("("));
    for (size_t k = 0; k < value.components.size(); ++k) {
      if (k) out.push_back(text(","));
      spell(value.components[k], out);
    }
    out.push_back(text(")"));
  }

  void rewrite_call_stmt(const Stmt& s, std::vector<Stmt>& out) {
    Stmt c = s;
    const std::string_view internal = internal_name(s);
    if (internal == "FnReadData" && !s.fn_args.empty() &&
        is_tuple(s.fn_args[0])) {
      // CmdStan's JSON reader names a tuple's leaves by their slots.
      const Value* bound = lookup(s.fn_args[0].name);
      if (s.fn_args[0].kind != Expr::Var || !bound || s.fn_args.size() != 3 ||
          s.fn_args[2].kind != Expr::LitStr)
        throw MirError("mir: malformed tuple FnReadData");
      for (const Expr& leaf : leaves_of(*bound)) {
        Stmt read = s;
        read.fn_args[0] = leaf;
        const TupleLeaf& path = program_.tuple_leaves.at(leaf.name);
        for (const TupleLeaf::Slot& slot : path.slots)
          read.fn_args[2].lit_s += "." + std::to_string(slot.slot);
        out.push_back(std::move(read));
      }
      return;
    }
    if ((internal == "FnPrint" || internal == "FnReject" ||
         internal == "FnFatalError")) {
      c.fn_args.clear();
      for (const Expr& arg : s.fn_args) {
        if (!is_tuple(arg)) {
          c.fn_args.push_back(rewrite(arg, out));
          continue;
        }
        if (arg.unsized.depth > 0) unsupported("printing an array of tuples");
        spell(value_of(arg, out), c.fn_args);
      }
    } else if (s.fn_lib == Expr::Lib::UserDefined) {
      c.fn_args = spread(s.fn_args, out);
    }
    rewrite_exprs(c, out);
    out.push_back(std::move(c));
  }

  // A statement, after the statements its expressions need first. When
  // those declare temporaries, they and the statement go in a block of
  // their own, so a temporary never becomes transformed data; a
  // declaration the statement makes stays outside the block.
  void rewrite_stmt(const Stmt& s, std::vector<Stmt>& out) {
    std::vector<Stmt> local;
    rewrite_in_place(s, local);
    bool hoisted = false;
    for (const Stmt& t : local)
      hoisted = hoisted || (t.kind == Stmt::Decl && temps_.count(t.decl_id));
    if (!hoisted) {
      for (Stmt& t : local) out.push_back(std::move(t));
      return;
    }
    Stmt block;
    block.kind = Stmt::Block;
    for (Stmt& t : local) {
      if (t.kind != Stmt::Decl || temps_.count(t.decl_id)) {
        block.body.push_back(std::move(t));
        continue;
      }
      bool sized_by_temporary = false;
      for_each_dim(t.decl_type, [&](const Expr& dim) {
        sized_by_temporary = sized_by_temporary || mentions(dim, temps_);
      });
      if (sized_by_temporary)
        unsupported("a size computed from a function returning a tuple");
      if (t.has_init) {
        block.body.push_back(assignment(t.decl_id, {}, std::move(t.init)));
        t.has_init = false;
        t.init = Expr{};
      }
      out.push_back(std::move(t));
    }
    out.push_back(std::move(block));
  }

  void rewrite_in_place(const Stmt& s, std::vector<Stmt>& out) {
    switch (s.kind) {
      case Stmt::Decl:
        return rewrite_decl(s, out);
      case Stmt::Assignment:
        return rewrite_assignment(s, out);
      case Stmt::NRFunApp:
        return rewrite_call_stmt(s, out);
      case Stmt::While: {
        Stmt w = s;
        std::vector<Stmt> pre;
        w.cond = rewrite(s.cond, pre);
        if (!pre.empty())
          unsupported(
              "a while condition that calls a function returning a tuple");
        rewrite_children(w);
        out.push_back(std::move(w));
        return;
      }
      case Stmt::SList: {
        for (const Stmt& child : s.body) rewrite_stmt(child, out);
        return;
      }
      case Stmt::Block: {
        Scope scope(scopes_);
        Stmt b = s;
        b.body = rewrite_block(s.body);
        out.push_back(std::move(b));
        return;
      }
      default: {
        Stmt c = s;
        rewrite_exprs(c, out);
        rewrite_children(c);
        out.push_back(std::move(c));
        return;
      }
    }
  }

  Program& program_;
  std::map<std::string, const FunDef*> inlined_;
  std::vector<Frame> scopes_;
  std::map<std::string, bool> leaf_data_only_;
  std::set<std::string> temps_;
  unsigned counter_ = 0;
  unsigned inline_depth_ = 0;
};

}  // namespace

void eliminate_tuples(Program& program) {
  if (!program.has_tuples) return;
  TupleEliminator(program).run();
}

}  // namespace detail
}  // namespace mir
}  // namespace stanli
