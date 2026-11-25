// ProgramCompiler: values known while the program is compiled (shape queries,
// compile-time integers and reals).
#include <stanli/mir_prog.hpp>

namespace stanli {

bool ProgramCompiler::is_shape_query(const mir::Expr& e) {
  if (e.args.size() != 1) return false;
  if (e.name == "FnLength") return true;
  const BuiltinSpec* query =
      shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::ShapeQuery);
  return query != nullptr && query->shape_query != BuiltinShapeQueryKind::Dims;
}

long ProgramCompiler::shape_query(const mir::Expr& e, Range v) {
  if (v.kind == ViewKind::Array && v.dims.empty()) v.dims = {v.len};
  const BuiltinSpec* query =
      shaped_builtin_spec(e.name == "FnLength" ? "size" : e.name, 1,
                          BuiltinShapePolicy::ShapeQuery);
  try {
    return (long)builtin_shape_query(*query,
                                     builtin_argument_shape(e.args[0], v))
        .front();
  } catch (const std::invalid_argument& error) {
    bail(e.name + ": " + std::string(error.what()));
  }
}

int64_t ProgramCompiler::checked_shape_product(const std::vector<int64_t>& dims,
                                               const std::string& what) {
  int64_t product = 1;
  for (int64_t extent : dims) {
    if (extent < 0) bail(what + " has a negative logical extent");
    if (product != 0 && extent != 0 && product > (int64_t)kMaxRegs / extent)
      bail(what + " has too many logical elements");
    product *= extent;
  }
  return product;
}

std::vector<int64_t> ProgramCompiler::validated_int_array_dims(
    const std::vector<long>& values, std::vector<int64_t> dims,
    const std::string& what) {
  if (values.size() > (size_t)kMaxRegs)
    bail(what + " has too many stored elements");
  if (dims.empty()) dims = {(int64_t)values.size()};
  if (checked_shape_product(dims, what) != (int64_t)values.size())
    bail(what + " storage and logical shape disagree");
  return dims;
}

mir::Expr ProgramCompiler::int_array_literal(const std::vector<long>& values,
                                             const std::vector<int64_t>& dims) {
  const std::vector<int64_t> shape =
      validated_int_array_dims(values, dims, "integer array literal");
  std::vector<int64_t> stride(shape.size(), 1);
  for (size_t d = 1; d < shape.size(); ++d)
    stride[d] = stride[d - 1] * shape[d - 1];
  std::function<mir::Expr(size_t, int64_t)> make = [&](size_t d,
                                                       int64_t offset) {
    if (d == shape.size()) {
      mir::Expr cell;
      cell.kind = mir::Expr::LitInt;
      cell.lit_i = values.at((size_t)offset);
      cell.unsized = {0, mir::UnsizedLeaf::Int};
      cell.data_only = true;
      return cell;
    }
    mir::Expr literal;
    literal.kind = mir::Expr::FunApp;
    literal.fn_lib = mir::Expr::Lib::Internal;
    literal.name = "FnMakeArray";
    literal.unsized = {static_cast<uint8_t>(shape.size() - d),
                       mir::UnsizedLeaf::Int};
    literal.data_only = true;
    literal.args.reserve((size_t)shape[d]);
    for (int64_t k = 0; k < shape[d]; ++k)
      literal.args.push_back(make(d + 1, offset + k * stride[d]));
    return literal;
  };
  return make(0, 0);
}

void ProgramCompiler::literalize_external_ints(mir::Expr* e) {
  if (e->kind == mir::Expr::Var) {
    auto scalar = ints.find(e->name);
    if (scalar != ints.end() && scalar->second.size() == 1) {
      const long value = scalar->second[0];
      *e = mir::Expr{};
      e->kind = mir::Expr::LitInt;
      e->lit_i = value;
      e->unsized = {0, mir::UnsizedLeaf::Int};
      e->data_only = true;
      return;
    }
    auto array = known_int_arrays.find(e->name);
    auto dims = known_int_array_dims.find(e->name);
    if (array != known_int_arrays.end()) {
      *e = int_array_literal(
          array->second,
          dims == known_int_array_dims.end()
              ? std::vector<int64_t>{(int64_t)array->second.size()}
              : dims->second);
      return;
    }
  }
  for (mir::Expr& arg : e->args) literalize_external_ints(&arg);
}

bool ProgramCompiler::refers_to_local_runtime(const mir::Expr& e) const {
  if (e.kind == mir::Expr::Var && reals.count(e.name) &&
      !extern_bound.count(e.name))
    return true;
  for (const auto& arg : e.args)
    if (refers_to_local_runtime(arg)) return true;
  return false;
}

bool ProgramCompiler::external_int_array(const mir::Expr& e,
                                         std::vector<long>* values,
                                         std::vector<int64_t>* dims) {
  if (!extern_ints) return false;
  if (e.kind == mir::Expr::Var) {
    auto known = known_int_arrays.find(e.name);
    if (known != known_int_arrays.end()) {
      auto shape = known_int_array_dims.find(e.name);
      *values = known->second;
      *dims = validated_int_array_dims(
          *values,
          shape == known_int_array_dims.end()
              ? std::vector<int64_t>{(int64_t)values->size()}
              : shape->second,
          "external integer array " + e.name);
      return true;
    }
  }
  mir::Expr literal = e;
  literalize_external_ints(&literal);
  if (refers_to_local_runtime(literal)) return false;
  if (!extern_ints(literal, values, dims)) return false;
  *dims = validated_int_array_dims(*values, std::move(*dims),
                                   "external integer array expression");
  return true;
}

bool ProgramCompiler::external_real(const mir::Expr& e, double* value) {
  if (!extern_real) return false;
  mir::Expr literal = e;
  literalize_external_ints(&literal);
  return !refers_to_local_runtime(literal) && extern_real(literal, value);
}

std::optional<double> ProgramCompiler::constant_real(const mir::Expr& e) {
  if (e.data_only && mir::is_unsized(e, mir::UnsizedLeaf::Real)) {
    double value = 0.0;
    if (external_real(e, &value)) return value;
  }
  switch (e.kind) {
    case mir::Expr::LitInt:
      return static_cast<double>(e.lit_i);
    case mir::Expr::LitReal:
      return e.lit;
    case mir::Expr::Var: {
      auto real = known_reals.find(e.name);
      if (real != known_reals.end()) return real->second;
      auto integer = ints.find(e.name);
      if (integer != ints.end() && integer->second.size() == 1)
        return static_cast<double>(integer->second[0]);
      return std::nullopt;
    }
    case mir::Expr::TernaryIf: {
      long condition;
      if (e.args.size() != 3 || !try_cint(e.args[0], &condition))
        return std::nullopt;
      return constant_real(e.args[condition != 0 ? 1 : 2]);
    }
    case mir::Expr::FunApp:
      if (const auto value = mir::nullary_constant(e)) return value;
      if (e.args.size() == 1 && (e.name == "PMinus__" || e.name == "minus" ||
                                 e.name == "PPlus__" || e.name == "plus")) {
        const auto value = constant_real(e.args[0]);
        if (!value) return std::nullopt;
        return e.name == "PMinus__" || e.name == "minus" ? -*value : *value;
      }
      if (e.args.size() == 2) {
        const auto lhs = constant_real(e.args[0]);
        if (!lhs) return std::nullopt;
        const auto rhs = constant_real(e.args[1]);
        if (!rhs) return std::nullopt;
        if (e.name == "Plus__" || e.name == "add") return *lhs + *rhs;
        if (e.name == "Minus__" || e.name == "subtract") return *lhs - *rhs;
        if (e.name == "Times__" || e.name == "multiply" ||
            e.name == "elt_multiply")
          return *lhs * *rhs;
        if (e.name == "Divide__" || e.name == "divide" ||
            e.name == "elt_divide")
          return *lhs / *rhs;
      }
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

bool ProgramCompiler::try_creal(const mir::Expr& e, double* out) {
  const auto value = constant_real(e);
  if (!value) return false;
  *out = *value;
  return true;
}

long ProgramCompiler::cint(const mir::Expr& e) {
  switch (e.kind) {
    case mir::Expr::LitInt:
      return e.lit_i;
    case mir::Expr::LitReal:
      return (long)e.lit;
    case mir::Expr::Var: {
      auto it = ints.find(e.name);
      if (it != ints.end() && it->second.size() == 1) return it->second[0];
      bail("integer " + e.name + " is not known at compile time");
    }
    case mir::Expr::Indexed: {
      if (e.args.size() < 2) bail("integer index form");
      // The same substituted literal, indexed rather than measured.
      if (e.args.size() == 2 && e.args[0].kind == mir::Expr::FunApp &&
          e.args[0].name == "FnMakeArray" &&
          e.args[1].kind == mir::Expr::IndexSingle) {
        const long ix = cint(e.args[1].args[0]);
        if (ix < 1 || (size_t)ix > e.args[0].args.size())
          bail("integer index range");
        return cint(e.args[0].args[(size_t)ix - 1]);
      }
      // Any other one-dimensional int array a function returns, such as
      // `dims(x)[2]`.
      if (e.args.size() == 2 && e.args[0].kind == mir::Expr::FunApp &&
          e.args[0].unsized.depth == 1 &&
          e.args[0].unsized.leaf == mir::UnsizedLeaf::Int &&
          e.args[1].kind == mir::Expr::IndexSingle) {
        const std::vector<long> values = cints(e.args[0]);
        const long ix = cint(e.args[1].args[0]);
        if (ix < 1 || (size_t)ix > values.size()) bail("integer index range");
        return values[(size_t)ix - 1];
      }
      if (e.args[0].kind != mir::Expr::Var) bail("integer index base");
      for (size_t k = 1; k < e.args.size(); ++k)
        if (e.args[k].kind != mir::Expr::IndexSingle ||
            e.args[k].args.size() != 1)
          bail("integer index form");
      auto known = known_int_arrays.find(e.args[0].name);
      if (known != known_int_arrays.end()) {
        auto shape = known_int_array_dims.find(e.args[0].name);
        const std::vector<int64_t> dims =
            shape == known_int_array_dims.end()
                ? std::vector<int64_t>{(int64_t)known->second.size()}
                : shape->second;
        if (e.args.size() - 1 != dims.size()) bail("integer index form");
        int64_t flat = 0;
        int64_t stride = 1;
        for (size_t d = 0; d < dims.size(); ++d) {
          const long ix = cint(e.args[d + 1].args[0]);
          if (ix < 1 || ix > dims[d]) bail("integer index range");
          if (ix - 1 != 0 && stride > kMaxRegs / (ix - 1))
            bail("integer index offset overflow");
          const int64_t term = (ix - 1) * stride;
          if (flat > kMaxRegs - term) bail("integer index offset overflow");
          flat += term;
          if (d + 1 != dims.size() && dims[d] != 0 &&
              stride > kMaxRegs / dims[d])
            bail("integer index stride overflow");
          stride *= dims[d];
        }
        if (flat < 0 || (size_t)flat >= known->second.size())
          bail("integer index storage range");
        return known->second[(size_t)flat];
      }
      auto it = ints.find(e.args[0].name);
      if (it != ints.end()) {
        if (e.args.size() != 2) bail("integer index form");
        const long ix = cint(e.args[1].args[0]);
        if (ix < 1 || (size_t)ix > it->second.size())
          bail("integer index range");
        return it->second[(size_t)ix - 1];
      }
      // A read from a data array, at any rank: `ms[ri, 8]` where the
      // region unrolled the loop that produced `ri`. Resolving the
      // indices here and asking for a literal read keeps the array's
      // storage order the one place that already knows it.
      if (extern_int && (extern_bound.count(e.args[0].name) ||
                         !reals.count(e.args[0].name))) {
        mir::Expr literal = e;
        for (size_t k = 1; k < literal.args.size(); ++k) {
          mir::Expr& ix = literal.args[k].args[0];
          const long v = cint(ix);
          ix = mir::Expr{};
          ix.kind = mir::Expr::LitInt;
          ix.lit_i = v;
          ix.unsized = {0, mir::UnsizedLeaf::Int};
          ix.data_only = true;
        }
        long v;
        if (extern_int(literal, &v)) return v;
      }
      bail("integer array " + e.args[0].name);
    }
    case mir::Expr::TernaryIf:
      if (e.args.size() != 3) bail("integer conditional form");
      return cint(e.args[cint(e.args[0]) != 0 ? 1 : 2]);
    case mir::Expr::EAnd:
    case mir::Expr::EOr: {
      // Stan short-circuits these, and so does this: the second operand
      // of a decided `&&` is never evaluated, so it does not have to be
      // a compile-time integer -- or an integer at all.
      if (e.args.size() != 2 || !int_operand(e.args[0]))
        bail("integer logical form");
      const bool lhs = cint(e.args[0]) != 0;
      if (e.kind == mir::Expr::EAnd && !lhs) return 0;
      if (e.kind == mir::Expr::EOr && lhs) return 1;
      if (!int_operand(e.args[1])) bail("integer logical form");
      return cint(e.args[1]) != 0;
    }
    case mir::Expr::FunApp:
      if (const FunctionSpec* function = function_spec(e);
          function != nullptr && function->builtin() != nullptr &&
          function->builtin()->shape == BuiltinShapePolicy::Elementwise &&
          function->result() == FunctionArgumentKind::Integer) {
        const BuiltinSpec* spec = function->builtin();
        if (spec->arity == 1 && !builtin_argument_is_integer(*spec, 0)) {
          double value = 0.0;
          if (!try_creal(e.args[0], &value)) bail("integer builtin operand");
          return evaluate_real_to_integer_builtin(*spec, value);
        }
        if (spec->arity == 1)
          return evaluate_integer_unary_builtin(
              *spec, static_cast<int>(cint(e.args[0])));
        if (spec->arity != 2) bail("integer builtin arity");
        return evaluate_integer_binary_builtin(
            *spec, static_cast<int>(cint(e.args[0])),
            static_cast<int>(cint(e.args[1])));
      }
      if (e.args.size() == 2) {
        // Each operator with the named spelling beside it: on ints the
        // alias is the operator, down to `divide`'s truncation.
        const auto arith = [&](IntOp op) -> long {
          return int_arith(op, static_cast<int>(cint(e.args[0])),
                           static_cast<int>(cint(e.args[1])));
        };
        if (e.name == "Plus__" || e.name == "add") return arith(IntOp::Add);
        if (e.name == "Minus__" || e.name == "subtract")
          return arith(IntOp::Sub);
        if (e.name == "Times__" || e.name == "multiply" ||
            e.name == "elt_multiply")
          return arith(IntOp::Mul);
        if (e.name == "IntDivide__" || e.name == "Divide__" ||
            e.name == "divide" || e.name == "elt_divide")
          return arith(IntOp::Div);
        // The comparisons: on integers each is an integer in its own
        // right, and a `while` condition or an integer local written
        // with one -- `int found = (a[i] == k);` -- is as much a
        // compile-time value as its operands are.
        if (const BuiltinSpec* pred =
                shaped_builtin_spec(e.name, 2, BuiltinShapePolicy::Predicate)) {
          if (int_operand(e.args[0]) && int_operand(e.args[1]))
            return evaluate_predicate_builtin(*pred, (double)cint(e.args[0]),
                                              (double)cint(e.args[1]));
          double lhs = 0.0, rhs = 0.0;
          if (try_creal(e.args[0], &lhs) && try_creal(e.args[1], &rhs))
            return evaluate_predicate_builtin(*pred, lhs, rhs);
        }
      }
      if (e.args.size() == 1 && e.name == "PMinus__")
        return int_negate(static_cast<int>(cint(e.args[0])));
      if (e.args.size() == 1) {
        if (const BuiltinSpec* pred =
                shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::Predicate)) {
          if (int_operand(e.args[0]) &&
              pred->predicate == BuiltinPredicate::Negation)
            return evaluate_predicate_builtin(*pred, (double)cint(e.args[0]));
          // The IEEE classifications (and negation of a real) fold over
          // any compile-time value.
          double value = 0.0;
          if (try_creal(e.args[0], &value))
            return evaluate_predicate_builtin(*pred, value);
        }
      }
      // A declared extent, a loop bound or an index written as a shape
      // query: `matrix[rows(m), cols(m)] out;`, `for (i in 1:rows(m))`.
      // Before this, only a shape query in a real-valued context was
      // answered, and these were refused as unknown integer functions.
      if (is_shape_query(e)) {
        Range v;
        if (static_view(e.args[0], &v)) return shape_query(e, v);
      }
      if (e.args.size() == 1 && e.name == "sum" &&
          e.args[0].unsized.leaf == mir::UnsizedLeaf::Int) {
        long total = 0;
        for (long value : cints(e.args[0])) total += value;
        return total;
      }
      bail("integer function " + e.name +
           (e.args.empty()
                ? std::string()
                : " arg-kind=" + std::to_string((int)e.args[0].kind) +
                      " arg-name=" + e.args[0].name +
                      " arg-type=" + mir::type_spelling(e.args[0].unsized)));
    default:
      bail("integer expression kind=" +
           std::to_string(static_cast<int>(e.kind)) + " name=" + e.name +
           " type=" + mir::type_spelling(e.unsized));
  }
}

bool ProgramCompiler::try_cint(const mir::Expr& e, long* out) {
  // Scalar runtime conditions are expected misses. Preserve the throwing
  // evaluator for required geometry and uncommon forms, but do not unwind
  // through the compiler just to learn that a scalar value is unavailable.
  if (e.kind == mir::Expr::Var) {
    const auto it = ints.find(e.name);
    if (it == ints.end() || it->second.size() != 1) return false;
    *out = it->second[0];
    return true;
  }
  if (e.kind == mir::Expr::TernaryIf) {
    long condition;
    return e.args.size() == 3 && try_cint(e.args[0], &condition) &&
           try_cint(e.args[condition != 0 ? 1 : 2], out);
  }
  if (e.kind == mir::Expr::EAnd || e.kind == mir::Expr::EOr) {
    long lhs;
    if (e.args.size() != 2 || !int_operand(e.args[0]) ||
        !try_cint(e.args[0], &lhs))
      return false;
    const bool value = lhs != 0;
    if (e.kind == mir::Expr::EAnd ? !value : value) {
      *out = value;
      return true;
    }
    long rhs;
    if (!int_operand(e.args[1]) || !try_cint(e.args[1], &rhs)) return false;
    *out = rhs != 0;
    return true;
  }
  if (e.kind == mir::Expr::Indexed && e.args.size() >= 2) {
    // Each form cint answers reads every single index as an integer, and
    // a variable base must be a known or external integer array.
    for (size_t k = 1; k < e.args.size(); ++k) {
      long index;
      if (e.args[k].kind == mir::Expr::IndexSingle &&
          e.args[k].args.size() == 1 && !try_cint(e.args[k].args[0], &index))
        return false;
    }
    const mir::Expr& base = e.args[0];
    if (base.kind == mir::Expr::Var && !known_int_arrays.count(base.name) &&
        !ints.count(base.name) &&
        !(extern_int &&
          (extern_bound.count(base.name) || !reals.count(base.name))))
      return false;
  }
  if (e.kind == mir::Expr::FunApp) {
    const FunctionSpec* function = function_spec(e);
    if (function && function->builtin() &&
        function->builtin()->shape == BuiltinShapePolicy::Rng)
      return false;
  }
  if (e.kind == mir::Expr::FunApp &&
      (e.args.size() == 1 || e.args.size() == 2)) {
    if (const BuiltinSpec* pred = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::Predicate)) {
      double args[2] = {};
      const bool integer =
          int_operand(e.args[0]) &&
          (e.args.size() == 2 ? int_operand(e.args[1])
                              : pred->predicate == BuiltinPredicate::Negation);
      for (size_t k = 0; k < e.args.size(); ++k) {
        if (integer) {
          long value;
          if (!try_cint(e.args[k], &value)) return false;
          args[k] = static_cast<double>(value);
        } else if (!try_creal(e.args[k], &args[k]))
          return false;
      }
      *out = evaluate_predicate_builtin(*pred, args[0], args[1]);
      return true;
    }
  }

  try {
    *out = cint(e);
    return true;
  } catch (Bail&) {
    return false;
  } catch (const std::domain_error&) {
    // A fold that raises a math error is left to the run-time op, which
    // raises it only if the expression runs.
    return false;
  }
}

std::vector<long> ProgramCompiler::cints(const mir::Expr& e) {
  if (e.data_only && e.unsized.depth != 0 &&
      e.unsized.leaf == mir::UnsizedLeaf::Int && extern_ints) {
    std::vector<long> values;
    std::vector<int64_t> dims;
    if (external_int_array(e, &values, &dims)) return values;
  }
  // dims of a statically shaped value is a compile-time integer array.
  if (e.kind == mir::Expr::FunApp && e.args.size() == 1) {
    if (const BuiltinSpec* query =
            shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::ShapeQuery);
        query != nullptr && query->shape_query == BuiltinShapeQueryKind::Dims) {
      Range v;
      if (static_view(e.args[0], &v)) {
        if (v.kind == ViewKind::Array && v.dims.empty()) v.dims = {v.len};
        const std::vector<int64_t> extents =
            builtin_shape_query(*query, builtin_argument_shape(e.args[0], v));
        return std::vector<long>(extents.begin(), extents.end());
      }
    }
  }
  if (e.kind == mir::Expr::Var) {
    auto known = known_int_arrays.find(e.name);
    if (known != known_int_arrays.end()) return known->second;
    auto held = ints.find(e.name);
    if (held != ints.end()) return held->second;

    Range view;
    if (static_view(e, &view) && view.kind == ViewKind::Array &&
        view.len >= 0) {
      std::vector<long> values;
      values.reserve((size_t)view.len);
      const std::vector<int64_t> dims =
          view.dims.empty() ? std::vector<int64_t>{view.len} : view.dims;
      std::vector<int64_t> chosen(dims.size());
      std::function<void(size_t)> gather = [&](size_t d) {
        if (d != dims.size()) {
          for (int64_t i = 1; i <= dims[d]; ++i) {
            chosen[d] = i;
            gather(d + 1);
          }
          return;
        }
        mir::Expr indexed;
        indexed.kind = mir::Expr::Indexed;
        indexed.unsized = {0, mir::UnsizedLeaf::Int};
        indexed.data_only = true;
        indexed.args.push_back(e);
        for (int64_t position : chosen) {
          mir::Expr literal;
          literal.kind = mir::Expr::LitInt;
          literal.lit_i = position;
          literal.unsized = {0, mir::UnsizedLeaf::Int};
          literal.data_only = true;
          mir::Expr single;
          single.kind = mir::Expr::IndexSingle;
          single.args.push_back(std::move(literal));
          indexed.args.push_back(std::move(single));
        }
        values.push_back(cint(indexed));
      };
      gather(0);
      return values;
    }
    bail("integer array " + e.name + " is not known at compile time");
  }
  if (e.kind == mir::Expr::FunApp && e.name == "FnMakeArray") {
    std::vector<long> values;
    values.reserve(e.args.size());
    for (const auto& value : e.args) values.push_back(cint(value));
    return values;
  }
  if (e.kind == mir::Expr::FunApp && e.name == "append_array" &&
      e.args.size() == 2) {
    std::vector<long> values = cints(e.args[0]);
    std::vector<long> tail = cints(e.args[1]);
    values.insert(values.end(), tail.begin(), tail.end());
    return values;
  }
  if (e.kind == mir::Expr::Indexed && !e.args.empty() &&
      e.args[0].kind == mir::Expr::Var) {
    Range base;
    if (!static_view(e.args[0], &base) || base.kind != ViewKind::Array)
      bail("integer indexed selector has no static array view");
    const std::vector<int64_t> dims =
        base.dims.empty() ? std::vector<int64_t>{base.len} : base.dims;
    if (e.args.size() - 1 != dims.size())
      bail("integer indexed selector needs every array dimension");
    std::vector<std::vector<int64_t>> positions;
    positions.reserve(dims.size());
    for (size_t d = 0; d < dims.size(); ++d)
      positions.push_back(matrix_positions(
          e.args[d + 1], dims[d], "array selector", SelectorSite::Array));

    std::vector<long> values;
    std::vector<int64_t> chosen(dims.size());
    std::function<void(int)> gather = [&](int d) {
      if (d >= 0) {
        for (int64_t position : positions[(size_t)d]) {
          chosen[(size_t)d] = position + 1;
          gather(d - 1);
        }
        return;
      }
      mir::Expr scalar;
      scalar.kind = mir::Expr::Indexed;
      scalar.unsized = {0, mir::UnsizedLeaf::Int};
      scalar.data_only = true;
      scalar.args.push_back(e.args[0]);
      for (int64_t position : chosen) {
        mir::Expr literal;
        literal.kind = mir::Expr::LitInt;
        literal.lit_i = position;
        literal.unsized = {0, mir::UnsizedLeaf::Int};
        literal.data_only = true;
        mir::Expr single;
        single.kind = mir::Expr::IndexSingle;
        single.args.push_back(std::move(literal));
        scalar.args.push_back(std::move(single));
      }
      values.push_back(cint(scalar));
    };
    gather((int)dims.size() - 1);
    return values;
  }
  std::string detail =
      "integer gather selector is not known at compile time: kind=" +
      std::to_string(static_cast<int>(e.kind)) + " name=" + e.name +
      " type=" + mir::type_spelling(e.unsized) +
      " args=" + std::to_string(e.args.size());
  for (const auto& arg : e.args) detail += " [" + arg.name + "]";
  bail(detail);
}

bool ProgramCompiler::try_cints(const mir::Expr& e, std::vector<long>* out) {
  try {
    *out = cints(e);
    return true;
  } catch (Bail&) {
    return false;
  } catch (const std::domain_error&) {
    return false;
  }
}

std::vector<double> ProgramCompiler::int_array_graph_values(
    const std::vector<long>& values, const std::vector<int64_t>& dims) {
  validated_int_array_dims(values, dims, "integer array graph value");
  if (values.empty()) return {};
  std::vector<std::vector<int64_t>> positions;
  positions.reserve(dims.size());
  for (int64_t extent : dims) {
    positions.emplace_back();
    positions.back().reserve((size_t)extent);
    for (int64_t k = 0; k < extent; ++k) positions.back().push_back(k);
  }
  // Scalar integer arrays have no container leaf, so graph_array_offsets
  // enumerates their cells outer-major.  Each returned coordinate's source
  // address is first-index-fast; reconstruct it while walking that order.
  std::vector<double> out;
  out.reserve(values.size());
  std::vector<int64_t> source_stride(dims.size(), 1);
  for (size_t d = 1; d < dims.size(); ++d)
    source_stride[d] = source_stride[d - 1] * dims[d - 1];
  std::function<void(size_t, int64_t)> gather = [&](size_t d, int64_t source) {
    if (d == dims.size()) {
      out.push_back((double)values.at((size_t)source));
      return;
    }
    for (int64_t position : positions[d])
      gather(d + 1, source + position * source_stride[d]);
  };
  gather(0, 0);
  return out;
}

bool ProgramCompiler::fold_is_certain(const std::string& name) const {
  auto it = int_decl_at.find(name);
  const int depth = it == int_decl_at.end() ? 0 : it->second.first;
  const size_t enclosing = it == int_decl_at.end() ? 0 : it->second.second;
  if (branch_depth != depth) return false;
  for (size_t k = enclosing; k < loops.size(); ++k)
    if (loops[k].structured || !loops[k].breaks.empty() ||
        !loops[k].continues.empty())
      return false;
  return true;
}

}  // namespace stanli
