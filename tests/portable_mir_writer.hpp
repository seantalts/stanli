// A test-only writer of portable MIR: it encodes a decoded mir::Program in
// the production OCaml layout, so a test can decode a fixture, change it, and
// hand the result to the runtime's decoder. tests/test_mir_decode.cpp checks
// that fixtures survive a round trip through it.
#ifndef STANLI_TESTS_PORTABLE_MIR_WRITER_HPP
#define STANLI_TESTS_PORTABLE_MIR_WRITER_HPP

#include <stanli/mir.hpp>

#include "../runtime/src/mir_tuples.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <utility>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stanli::portable_writer {

inline void append_u8(std::string& bytes, uint8_t value) {
  bytes.push_back(static_cast<char>(value));
}

inline void append_u32(std::string& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    append_u8(bytes, static_cast<uint8_t>(value >> shift));
}

inline void append_u64(std::string& bytes, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    append_u8(bytes, static_cast<uint8_t>(value >> shift));
}

inline void append_string(std::string& bytes, std::string_view value) {
  append_u32(bytes, static_cast<uint32_t>(value.size()));
  bytes.append(value);
}

inline std::string encode_base64(std::string_view bytes) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((bytes.size() + 2) / 3) * 4);
  for (size_t offset = 0; offset < bytes.size(); offset += 3) {
    const uint32_t a = static_cast<unsigned char>(bytes[offset]);
    const bool have_b = offset + 1 < bytes.size();
    const bool have_c = offset + 2 < bytes.size();
    const uint32_t b =
        have_b ? static_cast<unsigned char>(bytes[offset + 1]) : 0;
    const uint32_t c =
        have_c ? static_cast<unsigned char>(bytes[offset + 2]) : 0;
    result.push_back(alphabet[a >> 2]);
    result.push_back(alphabet[((a & 0x03) << 4) | (b >> 4)]);
    result.push_back(have_b ? alphabet[((b & 0x0f) << 2) | (c >> 6)] : '=');
    result.push_back(have_c ? alphabet[c & 0x3f] : '=');
  }
  return result;
}

inline std::string v2_wire(std::string_view payload) {
  return "STANLI5:" + encode_base64(payload);
}

inline void append_bool(std::string& bytes, bool value) {
  append_u8(bytes, value ? 1 : 0);
}

template <typename T, typename Write>
void append_list(std::string& bytes, const std::vector<T>& values,
                 Write write) {
  append_u32(bytes, static_cast<uint32_t>(values.size()));
  for (const T& value : values) write(bytes, value);
}

inline void append_view(std::string& bytes, const mir::UnsizedView& value) {
  append_u8(bytes, value.depth);
  append_u8(bytes, static_cast<uint8_t>(value.leaf));
}

inline void append_bools(std::string& bytes, const std::vector<bool>& values) {
  append_u32(bytes, static_cast<uint32_t>(values.size()));
  for (bool value : values) append_bool(bytes, value);
}

inline void append_expr(std::string& bytes, const mir::Expr& value);
inline void append_stmt(std::string& bytes, const mir::Stmt& value);

inline void append_expr_list(std::string& bytes,
                             const std::vector<mir::Expr>& values) {
  append_list(bytes, values, [](std::string& out, const mir::Expr& expression) {
    append_expr(out, expression);
  });
}

inline void append_expr(std::string& bytes, const mir::Expr& value) {
  append_u8(bytes, static_cast<uint8_t>(value.kind));
  switch (value.kind) {
    case mir::Expr::Var:
      append_string(bytes, value.name);
      append_bool(bytes, value.fn_type.has_value());
      if (value.fn_type) {
        append_list(bytes, value.fn_type->args,
                    [](std::string& out, const mir::UnsizedView& view) {
                      append_view(out, view);
                    });
        append_u8(bytes, static_cast<uint8_t>(value.fn_type->suffix));
        append_bool(bytes, value.fn_type->propto);
      }
      break;
    case mir::Expr::LitInt:
      append_u32(bytes,
                 static_cast<uint32_t>(static_cast<int32_t>(value.lit_i)));
      break;
    case mir::Expr::LitReal: {
      uint64_t bits = 0;
      std::memcpy(&bits, &value.lit, sizeof(bits));
      append_u64(bytes, bits);
      break;
    }
    case mir::Expr::LitStr:
      append_string(bytes, value.lit_s);
      break;
    case mir::Expr::FunApp:
      append_u8(bytes, static_cast<uint8_t>(value.fn_lib));
      append_string(bytes, value.name);
      append_u8(bytes, static_cast<uint8_t>(value.fn_suffix));
      append_bool(bytes, value.fn_propto);
      append_expr_list(bytes, value.args);
      break;
    case mir::Expr::Promotion:
    case mir::Expr::Indexed:
    case mir::Expr::TernaryIf:
    case mir::Expr::EOr:
    case mir::Expr::EAnd:
    case mir::Expr::IndexAll:
    case mir::Expr::IndexSingle:
    case mir::Expr::IndexUpfrom:
    case mir::Expr::IndexBetween:
    case mir::Expr::IndexMulti:
      append_expr_list(bytes, value.args);
      break;
    case mir::Expr::Unsupported:
      break;
  }
  append_view(bytes, value.unsized);
  append_bool(bytes, value.data_only);
  append_bool(bytes, value.promoted);
  append_string(bytes, value.raw);
}

inline void append_transform(std::string& bytes, const mir::Transform& value) {
  append_u8(bytes, static_cast<uint8_t>(value.kind));
  append_expr_list(bytes, value.args);
  append_string(bytes, value.raw);
}

inline void append_optional_transform(
    std::string& bytes, const std::optional<mir::Transform>& value) {
  append_bool(bytes, value.has_value());
  if (value) append_transform(bytes, *value);
}

inline void append_sized(std::string& bytes, const mir::SizedType& value) {
  append_string(bytes, value.base);
  append_expr_list(bytes, value.dims);
  append_string(bytes, value.elem_base);
  append_string(bytes, value.raw);
}

// The type of an input or output variable, with a tuple's components.
inline void append_interface_type(std::string& bytes,
                                  const mir::detail::TupleType& value) {
  append_sized(bytes, value.type);
  if (mir::detail::is_tuple(value.type))
    append_list(bytes, value.components, append_interface_type);
}

inline void append_print_item(std::string& bytes, const mir::PrintItem& item) {
  append_u8(bytes, item.kind == mir::PrintItem::Kind::Tuple ? 1 : 0);
  if (item.kind == mir::PrintItem::Kind::Tuple) {
    append_u8(bytes, item.depth);
    append_list(bytes, item.components, append_print_item);
  }
}

inline void append_stmt_list(std::string& bytes,
                             const std::vector<mir::Stmt>& values) {
  append_list(bytes, values, [](std::string& out, const mir::Stmt& statement) {
    append_stmt(out, statement);
  });
}

inline void append_stmt(std::string& bytes, const mir::Stmt& value) {
  append_u8(bytes, value.kind == mir::Stmt::MultiAssign ? 15
                   : value.jacobian ? 14
                                    : static_cast<uint8_t>(value.kind));
  switch (value.kind) {
    case mir::Stmt::Decl:
      append_string(bytes, value.decl_id);
      if (!value.decl_type.base.empty() ||
          value.decl_type.unsized.leaf == mir::UnsizedLeaf::Unknown) {
        append_u8(bytes, 0);
        append_sized(bytes, value.decl_type);
      } else {
        append_u8(bytes, 1);
        append_view(bytes, value.decl_type.unsized);
        append_string(bytes, value.decl_type.raw);
      }
      append_bool(bytes, value.decl_data_only);
      append_bool(bytes, value.has_init);
      if (value.has_init) append_expr(bytes, value.init);
      append_optional_transform(bytes, value.read_transform);
      append_expr_list(bytes, value.read_dims);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::Assignment:
      append_string(bytes, value.lhs);
      append_expr_list(bytes, value.lhs_idx);
      append_expr(bytes, value.rhs);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::TargetPE:
      append_expr(bytes, value.target);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::Block:
    case mir::Stmt::SList:
      append_stmt_list(bytes, value.body);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::For:
      append_string(bytes, value.loopvar);
      append_expr(bytes, value.lower);
      append_expr(bytes, value.upper);
      append_stmt_list(bytes, value.body);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::IfElse:
    case mir::Stmt::While:
      append_expr(bytes, value.cond);
      append_stmt_list(bytes, value.body);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::NRFunApp:
      append_u8(bytes, static_cast<uint8_t>(value.fn_lib));
      append_string(bytes, value.fn_name);
      append_u8(bytes, static_cast<uint8_t>(value.fn_suffix));
      if (mir::internal_name(value) == "FnCheck" ||
          mir::internal_name(value) == "FnWriteParam") {
        const bool check = mir::internal_name(value) == "FnCheck";
        const std::vector<mir::Expr> args =
            value.fn_args.empty()
                ? std::vector<mir::Expr>{}
                : std::vector<mir::Expr>(value.fn_args.begin() + 1,
                                         value.fn_args.end());
        append_expr_list(bytes, args);
        append_u8(bytes, check ? 1 : 2);
        if (check) {
          append_transform(bytes,
                           value.check_transform.value_or(mir::Transform{}));
          append_string(bytes, value.check_var_name);
        } else {
          append_optional_transform(bytes, value.write_transform);
        }
        append_expr(
            bytes, value.fn_args.empty() ? mir::Expr{} : value.fn_args.front());
      } else {
        append_expr_list(bytes, value.fn_args);
        append_u8(bytes, value.print_layout.empty() ? 0 : 3);
        if (!value.print_layout.empty())
          append_list(bytes, value.print_layout, append_print_item);
      }
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::Return:
      append_expr_list(bytes, value.has_init ? std::vector<mir::Expr>{value.rhs}
                                             : value.fn_args);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::MultiAssign:
      append_list(bytes, value.fn_args,
                  [](std::string& out, const mir::Expr& target) {
                    append_string(out, target.name);
                  });
      append_expr(bytes, value.rhs);
      append_string(bytes, value.raw);
      break;
    case mir::Stmt::Break:
    case mir::Stmt::Continue:
      break;
    case mir::Stmt::Skip:
    case mir::Stmt::Unsupported:
      append_string(bytes, value.raw);
      break;
  }
}

inline void append_strings(std::string& bytes,
                           const std::vector<std::string>& values) {
  append_list(bytes, values, [](std::string& out, const std::string& value) {
    append_string(out, value);
  });
}

inline void append_fun(std::string& bytes, const mir::FunDef& value) {
  append_string(bytes, value.name);
  append_strings(bytes, value.arg_names);
  append_list(bytes, value.arg_views, append_view);
  append_bools(bytes, value.arg_data_only);
  append_list(bytes, value.results, append_view);
  append_stmt_list(bytes, value.body);
}

// What a program's tuple input and output variables add on the wire: their
// full types, by name, and the leaf variables the compiler made for them.
struct WireTuples {
  std::map<std::string, mir::detail::TupleType> types;
  std::vector<mir::detail::WireTupleLeaf> leaves;
};

inline std::string write_v2(const mir::Program& value,
                            const WireTuples& tuples = {}) {
  const auto append_type = [&](std::string& out, const std::string& name,
                               const mir::SizedType& type) {
    const auto found = tuples.types.find(name);
    if (found != tuples.types.end())
      append_interface_type(out, found->second);
    else
      append_sized(out, type);
  };
  std::string payload;
  append_u32(payload, static_cast<uint32_t>(value.input_vars.size()));
  for (const auto& input : value.input_vars) {
    append_string(payload, input.first);
    append_type(payload, input.first, input.second);
  }
  append_stmt_list(payload, value.prepare_data);
  append_stmt_list(payload, value.log_prob);
  append_stmt_list(payload, value.generate_quantities);
  append_u32(payload,
             static_cast<uint32_t>(value.transformed_parameters_begin));
  append_u32(payload, static_cast<uint32_t>(value.generated_quantities_begin));
  append_list(payload, value.fun_defs,
              [](std::string& out, const mir::FunDef& definition) {
                append_fun(out, definition);
              });
  append_list(payload, value.output_vars,
              [&](std::string& out, const mir::OutputVar& var) {
                append_string(out, var.name);
                append_u8(out, static_cast<uint8_t>(var.block));
                append_type(out, var.name, var.type);
                append_list(out, var.annotations,
                            [](std::string& list, const std::string& name) {
                              append_string(list, name);
                            });
              });
  append_stmt_list(payload, value.transform_inits);
  append_list(payload, tuples.leaves,
              [](std::string& out, const mir::detail::WireTupleLeaf& leaf) {
                append_string(out, leaf.variable);
                append_string(out, leaf.base);
                append_list(
                    out, leaf.path,
                    [](std::string& slots, const mir::detail::TupleSlot& slot) {
                      append_u32(slots, static_cast<uint32_t>(slot.before));
                      append_u32(slots, static_cast<uint32_t>(slot.component));
                    });
              });
  return v2_wire(payload);
}

// Call `visit` on every statement of `body`, nested ones included.
inline void for_each_stmt(std::vector<mir::Stmt>& body,
                          const std::function<void(mir::Stmt&)>& visit) {
  for (mir::Stmt& statement : body) {
    visit(statement);
    for_each_stmt(statement.body, visit);
  }
}

// The same for one statement and the statements nested in it.
inline void for_each_stmt(mir::Stmt& statement,
                          const std::function<void(mir::Stmt&)>& visit) {
  visit(statement);
  for_each_stmt(statement.body, visit);
}

// The same for every section and function body of `program`.
inline void for_each_stmt(mir::Program& program,
                          const std::function<void(mir::Stmt&)>& visit) {
  for (auto* section : {&program.prepare_data, &program.log_prob,
                        &program.generate_quantities, &program.transform_inits})
    for_each_stmt(*section, visit);
  for (mir::FunDef& definition : program.fun_defs)
    for_each_stmt(definition.body, visit);
}

// Call `visit` on every expression of `program`, the arguments of each one
// and the extents of the input and output types included.
inline void visit_exprs(mir::Program& program,
                        const std::function<void(mir::Expr&)>& visit) {
  const std::function<void(mir::Expr&)> deep = [&](mir::Expr& e) {
    visit(e);
    for (mir::Expr& arg : e.args) deep(arg);
  };
  for (auto& input : program.input_vars)
    for (mir::Expr& dim : input.second.dims) deep(dim);
  for (mir::OutputVar& output : program.output_vars)
    for (mir::Expr& dim : output.type.dims) deep(dim);
  for_each_stmt(program, [&](mir::Stmt& statement) {
    mir::for_each_expr(statement, deep);
  });
}

// The first statement, in section order and nested ones included, that
// `match` accepts; null if there is none.
template <class Statements>
mir::Stmt* find_stmt(Statements& statements,
                     const std::function<bool(const mir::Stmt&)>& match) {
  mir::Stmt* found = nullptr;
  for_each_stmt(statements, [&](mir::Stmt& statement) {
    if (!found && match(statement)) found = &statement;
  });
  return found;
}

// The first expression, outermost first, that `match` accepts; null if
// there is none.
template <class Statements>
mir::Expr* find_expr(Statements& statements,
                     const std::function<bool(const mir::Expr&)>& match) {
  mir::Expr* found = nullptr;
  const std::function<void(mir::Expr&)> visit = [&](mir::Expr& e) {
    if (found) return;
    if (match(e)) {
      found = &e;
      return;
    }
    for (mir::Expr& arg : e.args) visit(arg);
  };
  for_each_stmt(statements, [&](mir::Stmt& statement) {
    mir::for_each_expr(statement, visit);
  });
  return found;
}

// The list that holds the first statement `match` accepts, and that
// statement's position in it; {nullptr, 0} if there is none.
inline std::pair<std::vector<mir::Stmt>*, size_t> find_position(
    std::vector<mir::Stmt>& body,
    const std::function<bool(const mir::Stmt&)>& match) {
  for (size_t i = 0; i < body.size(); ++i) {
    if (match(body[i])) return {&body, i};
    auto nested = find_position(body[i].body, match);
    if (nested.first) return nested;
  }
  return {nullptr, 0};
}

// Builders for the expressions and statements that tests splice into a
// decoded program. Every builder makes data-only values.
inline mir::UnsizedView view(mir::UnsizedLeaf leaf, uint8_t depth = 0) {
  return {depth, leaf};
}

inline mir::Expr lit_int(long value) {
  mir::Expr e;
  e.kind = mir::Expr::LitInt;
  e.lit_i = value;
  e.data_only = true;
  e.unsized = view(mir::UnsizedLeaf::Int);
  return e;
}

inline mir::Expr lit_real(double value) {
  mir::Expr e;
  e.kind = mir::Expr::LitReal;
  e.lit = value;
  e.data_only = true;
  e.unsized = view(mir::UnsizedLeaf::Real);
  return e;
}

inline mir::Expr var(const std::string& name, mir::UnsizedView type) {
  mir::Expr e;
  e.kind = mir::Expr::Var;
  e.name = name;
  e.data_only = true;
  e.unsized = type;
  return e;
}

// A Stan library call, or a compiler-internal one when `lib` says so.
inline mir::Expr call(const std::string& name, mir::UnsizedView type,
                      std::vector<mir::Expr> args,
                      mir::Expr::Lib lib = mir::Expr::Lib::StanLib) {
  mir::Expr e;
  e.kind = mir::Expr::FunApp;
  e.fn_lib = lib;
  e.name = name;
  e.data_only = true;
  for (const mir::Expr& arg : args) e.data_only = e.data_only && arg.data_only;
  e.unsized = type;
  e.args = std::move(args);
  return e;
}

// An index of `kind` (IndexSingle, IndexBetween, IndexMulti, ...).
inline mir::Expr index(mir::Expr::Kind kind, std::vector<mir::Expr> bounds) {
  mir::Expr e;
  e.kind = kind;
  e.data_only = true;
  e.args = std::move(bounds);
  return e;
}

inline mir::Expr indexed(mir::Expr base, std::vector<mir::Expr> indices,
                         mir::UnsizedView type) {
  mir::Expr e;
  e.kind = mir::Expr::Indexed;
  e.data_only = base.data_only;
  e.unsized = type;
  e.args.push_back(std::move(base));
  for (mir::Expr& i : indices) e.args.push_back(std::move(i));
  return e;
}

inline mir::Stmt assign(const std::string& lhs, std::vector<mir::Expr> index,
                        mir::Expr rhs) {
  mir::Stmt s;
  s.kind = mir::Stmt::Assignment;
  s.lhs = lhs;
  s.lhs_idx = std::move(index);
  s.rhs = std::move(rhs);
  return s;
}

// A data-only declaration of the sized type `base` with `dims`.
inline mir::Stmt declare(const std::string& name, const std::string& base,
                         std::vector<mir::Expr> dims = {}) {
  mir::Stmt s;
  s.kind = mir::Stmt::Decl;
  s.decl_id = name;
  s.decl_type.base = base;
  s.decl_type.dims = std::move(dims);
  s.decl_data_only = true;
  return s;
}

inline mir::Stmt block(std::vector<mir::Stmt> body) {
  mir::Stmt s;
  s.kind = mir::Stmt::Block;
  s.body = std::move(body);
  return s;
}

inline mir::Stmt skip() {
  mir::Stmt s;
  s.kind = mir::Stmt::Skip;
  return s;
}

// Whether `e` calls the function `name`.
inline bool calls(const mir::Expr& e, std::string_view name) {
  return e.kind == mir::Expr::FunApp && e.name == name;
}

}  // namespace stanli::portable_writer

#endif
