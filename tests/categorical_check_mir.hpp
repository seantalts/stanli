#ifndef STANLI_TEST_CATEGORICAL_CHECK_MIR_HPP
#define STANLI_TEST_CATEGORICAL_CHECK_MIR_HPP

#include "portable_mir_writer.hpp"

#include <stanli/mir_decode.hpp>

#include <stdexcept>
#include <string>
#include <vector>

// Minimal programs built in C++ around one categorical call. Building them
// here makes the behavior tests independent of the compiler's fixture
// output, while they still enter through the portable MIR decoder and the
// lowering path. Each *_program function returns the decoded form for tests
// that change it further; the *_mir functions return the encoded text.
namespace categorical_mir {

namespace mir = stanli::mir;
namespace pw = stanli::portable_writer;

inline mir::UnsizedView int_view(uint8_t depth = 0) {
  return pw::view(mir::UnsizedLeaf::Int, depth);
}
inline mir::UnsizedView real_view() { return pw::view(mir::UnsizedLeaf::Real); }
inline mir::UnsizedView vector_view() {
  return pw::view(mir::UnsizedLeaf::Vector);
}

inline mir::Expr autodiff(mir::Expr e) {
  e.data_only = false;
  return e;
}

// A call of the density `function`, `_lupmf` when `propto`.
inline mir::Expr density(const std::string& function, bool propto,
                         std::vector<mir::Expr> args,
                         mir::Expr::Lib lib = mir::Expr::Lib::StanLib) {
  mir::Expr call = pw::call(function, real_view(), std::move(args), lib);
  call.fn_suffix = mir::FnSuffix::Lpmf;
  call.fn_propto = propto;
  return call;
}

inline mir::Stmt target_pe(mir::Expr value) {
  mir::Stmt s;
  s.kind = mir::Stmt::TargetPE;
  s.target = std::move(value);
  return s;
}

inline mir::Stmt return_value(mir::Expr value) {
  mir::Stmt s;
  s.kind = mir::Stmt::Return;
  s.has_init = true;
  s.rhs = std::move(value);
  return s;
}

inline mir::FunDef function(const std::string& name,
                            std::vector<std::string> arg_names,
                            std::vector<mir::UnsizedView> arg_views,
                            std::vector<bool> arg_data_only,
                            mir::UnsizedView result,
                            std::vector<mir::Stmt> body) {
  mir::FunDef f;
  f.name = name;
  f.arg_names = std::move(arg_names);
  f.arg_views = std::move(arg_views);
  f.arg_data_only = std::move(arg_data_only);
  f.results = {result};
  f.body = {pw::block(std::move(body))};
  return f;
}

// int noisy(int x) { print("categorical effect"); return x; }
inline mir::FunDef noisy() {
  mir::Stmt print;
  print.kind = mir::Stmt::NRFunApp;
  print.fn_lib = mir::Expr::Lib::Internal;
  print.fn_name = "FnPrint";
  mir::Expr text;
  text.kind = mir::Expr::LitStr;
  text.lit_s = "categorical effect";
  text.data_only = true;
  text.unsized = real_view();
  print.fn_args = {text};
  return function("noisy", {"x"}, {int_view()}, {true}, int_view(),
                  {print, return_value(pw::var("x", int_view()))});
}

// The first expression of `statements` that `match` accepts, or a
// logic_error naming `what`.
template <class Statements>
mir::Expr& expect_expr(Statements& statements,
                       const std::function<bool(const mir::Expr&)>& match,
                       const char* what) {
  mir::Expr* found = pw::find_expr(statements, match);
  if (found == nullptr) throw std::logic_error(what);
  return *found;
}

inline std::function<bool(const mir::Expr&)> is_var(const std::string& name) {
  return [name](const mir::Expr& e) {
    return e.kind == mir::Expr::Var && e.name == name;
  };
}

inline std::function<bool(const mir::Expr&)> calls(const std::string& name) {
  return [name](const mir::Expr& e) { return pw::calls(e, name); };
}

// data { <outcome> outcome; vector[arg_size] arg; }
// parameters { real anchor; }
// model { target += anchor; target += function(outcome | arg); }
inline mir::Program check_program(const std::string& function,
                                  bool scalar_outcome, int outcome_size,
                                  int arg_size, bool propto = true,
                                  bool autodiff_local_arg = false,
                                  bool nested_outcome_binding = false,
                                  bool wrap_density_udf = false) {
  mir::Program p;
  mir::SizedType outcome;
  if (scalar_outcome) {
    outcome.base = "SInt";
  } else {
    outcome.base = "SArray";
    outcome.elem_base = "SInt";
    outcome.dims =
        nested_outcome_binding
            ? std::vector<mir::Expr>{pw::lit_int(1), pw::lit_int(outcome_size)}
            : std::vector<mir::Expr>{pw::lit_int(outcome_size)};
  }
  mir::SizedType arg;
  arg.base = "SVector";
  arg.dims = {pw::lit_int(arg_size)};
  p.input_vars = {{"outcome", outcome}, {"arg", arg}};

  mir::Stmt anchor = pw::declare("anchor", "SReal");
  anchor.decl_data_only = false;
  anchor.has_init = true;
  anchor.init = autodiff(
      pw::call("FnReadParam", real_view(), {}, mir::Expr::Lib::Internal));
  anchor.read_transform = mir::Transform{};
  p.log_prob.push_back(anchor);
  p.log_prob.push_back(target_pe(autodiff(pw::var("anchor", real_view()))));

  mir::Expr vector_arg = pw::var("arg", vector_view());
  if (autodiff_local_arg) {
    mir::Stmt local =
        pw::declare("local_arg", "SVector", {pw::lit_int(arg_size)});
    local.decl_data_only = false;
    p.log_prob.push_back(local);
    p.log_prob.push_back(
        pw::assign("local_arg", {}, autodiff(pw::var("arg", vector_view()))));
    vector_arg = autodiff(pw::var("local_arg", vector_view()));
  }
  mir::Expr call =
      density(function, propto,
              {pw::var("outcome", scalar_outcome ? int_view() : int_view(1)),
               vector_arg});
  call.data_only = !autodiff_local_arg;
  if (wrap_density_udf) {
    mir::Expr wrapped = pw::call("identity_real", real_view(), {call},
                                 mir::Expr::Lib::UserDefined);
    wrapped.data_only = true;
    call = wrapped;
  }
  p.log_prob.push_back(target_pe(call));
  p.has_transform_inits = true;
  return p;
}

inline std::string categorical_check_mir(const std::string& function,
                                         bool scalar_outcome, int outcome_size,
                                         int arg_size, bool propto = true,
                                         bool autodiff_local_arg = false,
                                         bool nested_outcome_binding = false,
                                         bool wrap_density_udf = false) {
  return pw::write_v2(check_program(function, scalar_outcome, outcome_size,
                                    arg_size, propto, autodiff_local_arg,
                                    nested_outcome_binding, wrap_density_udf));
}

// The scalar outcome goes through int noisy(int), which prints.
inline std::string categorical_effect_check_mir() {
  mir::Program p = check_program("categorical_lpmf", true, 1, 3);
  p.fun_defs = {noisy()};
  mir::Expr& outcome =
      expect_expr(p.log_prob, is_var("outcome"), "no categorical outcome");
  outcome =
      pw::call("noisy", int_view(), {outcome}, mir::Expr::Lib::UserDefined);
  return pw::write_v2(p);
}

// target += identity_real(function(outcome | arg)), with
// real identity_real(real x) { return x; }.
inline std::string categorical_udf_actual_mir(const std::string& function) {
  mir::Program p =
      check_program(function, true, 1, 3, false, false, false, true);
  p.fun_defs = {categorical_mir::function(
      "identity_real", {"x"}, {real_view()}, {true}, real_view(),
      {return_value(pw::var("x", real_view()))})};
  return pw::write_v2(p);
}

// The name of the user density that wraps `function`.
inline std::string udf_name(const std::string& function) {
  return function == "categorical_lpmf" ? "cat_lpmf" : "cat_logit_lpmf";
}

// The density call goes through a user density
// real cat_lpmf(int y, vector p) { return categorical_lupmf(y | p); },
// whose vector formal is data-only when `data_formal`. With
// `udf_body_local`, the body first copies p to a local q and uses that.
inline mir::Program udf_program(const std::string& function, bool propto,
                                bool autodiff_local_arg,
                                bool udf_body_local = false,
                                bool data_formal = false) {
  mir::Program p =
      check_program(function, true, 1, 3, propto, autodiff_local_arg);
  std::vector<mir::Stmt> body;
  mir::Expr vector_formal = autodiff(pw::var("p", vector_view()));
  if (udf_body_local) {
    mir::Stmt q = pw::declare("q", "SVector", {pw::lit_int(3)});
    q.decl_data_only = false;
    q.has_init = true;
    q.init = vector_formal;
    body.push_back(q);
    vector_formal = autodiff(pw::var("q", vector_view()));
  }
  body.push_back(return_value(autodiff(
      density(function, true, {pw::var("y", int_view()), vector_formal}))));
  p.fun_defs = {categorical_mir::function(
      udf_name(function), {"y", "p"}, {int_view(), vector_view()},
      {true, data_formal}, real_view(), body)};
  mir::Expr& call =
      expect_expr(p.log_prob, calls(function), "no categorical density call");
  call.fn_lib = mir::Expr::Lib::UserDefined;
  call.name = udf_name(function);
  return p;
}

inline std::string categorical_udf_mir(const std::string& function, bool propto,
                                       bool autodiff_local_arg,
                                       bool udf_body_local = false,
                                       bool data_formal = false) {
  return pw::write_v2(udf_program(function, propto, autodiff_local_arg,
                                  udf_body_local, data_formal));
}

// The vector argument is choose_vector(local_arg, arg), with
// vector choose_vector(vector ignored, data vector value) { return value; }.
inline std::string categorical_udf_return_mir(const std::string& function) {
  mir::Program p = check_program(function, true, 1, 3, true, true);
  p.fun_defs = {categorical_mir::function(
      "choose_vector", {"ignored", "value"}, {vector_view(), vector_view()},
      {false, true}, vector_view(),
      {return_value(pw::var("value", vector_view()))})};
  mir::Expr& density_call =
      expect_expr(p.log_prob, calls(function), "no categorical density call");
  mir::Expr& arg = density_call.args.at(1);
  arg = autodiff(pw::call("choose_vector", vector_view(),
                          {arg, pw::var("arg", vector_view())},
                          mir::Expr::Lib::UserDefined));
  return pw::write_v2(p);
}

// The user density takes an extra unused real formal, the caller passes
// anchor for it, and the body's vector argument is the ternary 1 ? p : p.
inline mir::Program udf_ternary_type_program(const std::string& function) {
  mir::Program p = udf_program(function, true, false);
  mir::FunDef& udf = p.fun_defs.at(0);
  udf.arg_names.push_back("ignored");
  udf.arg_views.push_back(real_view());
  udf.arg_data_only.push_back(false);
  mir::Expr& body_call =
      expect_expr(udf.body, calls(function), "no categorical body call");
  mir::Expr ternary;
  ternary.kind = mir::Expr::TernaryIf;
  ternary.unsized = vector_view();
  ternary.args = {pw::lit_int(1), body_call.args.at(1), body_call.args.at(1)};
  body_call.args.at(1) = ternary;
  mir::Expr& caller = expect_expr(p.log_prob, calls(udf_name(function)),
                                  "no categorical caller");
  caller.args.push_back(autodiff(pw::var("anchor", real_view())));
  return p;
}

inline std::string categorical_udf_ternary_type_mir(
    const std::string& function) {
  return pw::write_v2(udf_ternary_type_program(function));
}

// The same, with the int outcome promoted to real as the extra actual.
inline std::string categorical_udf_promoted_actual_mir(
    const std::string& function) {
  mir::Program p = udf_ternary_type_program(function);
  mir::Expr& caller = expect_expr(p.log_prob, calls(udf_name(function)),
                                  "no categorical caller");
  mir::Expr promoted = pw::var("outcome", int_view());
  promoted.unsized = real_view();
  promoted.promoted = true;
  caller.args.back() = promoted;
  return pw::write_v2(p);
}

// data { int flag; ... } and the vector argument flag ? arg : local_arg.
inline std::string categorical_promoted_ternary_mir(const std::string& function,
                                                    bool through_udf) {
  mir::Program p = through_udf
                       ? udf_program(function, true, true)
                       : check_program(function, true, 1, 3, true, true);
  mir::SizedType flag;
  flag.base = "SInt";
  p.input_vars.insert(p.input_vars.begin(), {"flag", flag});
  mir::Expr& arg = expect_expr(p.log_prob, is_var("local_arg"),
                               "no categorical vector argument");
  mir::Expr ternary;
  ternary.kind = mir::Expr::TernaryIf;
  ternary.unsized = vector_view();
  ternary.args = {pw::var("flag", int_view()), pw::var("arg", vector_view()),
                  arg};
  arg = ternary;
  return pw::write_v2(p);
}

// `mir` with the scalar outcome of its log density's `function` call passed
// through int noisy(int), which prints.
inline std::string categorical_parameter_effect_mir(
    const std::string& mir, const std::string& function) {
  mir::Program p = stanli::decode_program(mir);
  p.fun_defs.push_back(noisy());
  mir::Expr& call =
      expect_expr(p.log_prob, calls(function), "no categorical density");
  mir::Expr& outcome = call.args.at(0);
  outcome =
      pw::call("noisy", int_view(), {outcome}, mir::Expr::Lib::UserDefined);
  return pw::write_v2(p);
}

// `mir` with a last generated quantity
//   real categorical_value = function(y | theta);
// With `data_udf` the call is real data_categorical(int y, data vector p),
// which returns 0. With `island_arg` the vector is
// theta[1] > 0 ? theta : theta. With `force_interp` the outcome is
// binomial_rng(1, 1.0), or an array of `forced_outcome_count` of them.
inline mir::Program write_array_program(const std::string& mir,
                                        const std::string& function,
                                        bool propto, bool island_arg = false,
                                        bool data_udf = false,
                                        bool force_interp = false,
                                        int forced_outcome_count = 1) {
  mir::Program p = stanli::decode_program(mir);
  if (data_udf)
    p.fun_defs.push_back(categorical_mir::function(
        "data_categorical", {"y", "p"}, {int_view(), vector_view()},
        {true, true}, real_view(), {return_value(pw::lit_real(0.0))}));
  mir::Expr vector_arg = pw::var("theta", vector_view());
  if (island_arg) {
    mir::Expr first = autodiff(pw::indexed(
        autodiff(pw::var("theta", vector_view())),
        {pw::index(mir::Expr::IndexSingle, {pw::lit_int(1)})}, real_view()));
    mir::Expr ternary;
    ternary.kind = mir::Expr::TernaryIf;
    ternary.data_only = true;
    ternary.unsized = vector_view();
    ternary.args = {
        autodiff(pw::call("Greater__", int_view(), {first, pw::lit_int(0)})),
        autodiff(pw::var("theta", vector_view())),
        autodiff(pw::var("theta", vector_view()))};
    vector_arg = ternary;
  }
  mir::Expr outcome = pw::var("y", int_view());
  if (force_interp) {
    mir::Expr draw = pw::call("binomial_rng", int_view(),
                              {pw::lit_int(1), pw::lit_real(1.0)});
    draw.fn_suffix = mir::FnSuffix::Rng;
    outcome = draw;
    if (forced_outcome_count != 1)
      outcome = pw::call("FnMakeArray", int_view(1),
                         std::vector<mir::Expr>(
                             static_cast<size_t>(forced_outcome_count), draw),
                         mir::Expr::Lib::Internal);
  }
  mir::Stmt value = pw::declare("categorical_value", "SReal");
  value.has_init = true;
  value.init =
      data_udf ? pw::call("data_categorical", real_view(),
                          {outcome, vector_arg}, mir::Expr::Lib::UserDefined)
               : density(function, propto, {outcome, vector_arg});
  value.init.data_only = true;
  p.generate_quantities.push_back(value);
  mir::Stmt write;
  write.kind = mir::Stmt::NRFunApp;
  write.fn_lib = mir::Expr::Lib::Internal;
  write.fn_name = "FnWriteParam";
  write.fn_args = {pw::var("categorical_value", real_view())};
  p.generate_quantities.push_back(write);
  mir::OutputVar output;
  output.name = "categorical_value";
  output.block = mir::OutputVar::Block::GeneratedQuantities;
  output.type.base = "SReal";
  p.output_vars.push_back(output);
  return p;
}

inline std::string categorical_write_array_mir(
    const std::string& mir, const std::string& function, bool propto,
    bool island_arg = false, bool data_udf = false, bool force_interp = false,
    int forced_outcome_count = 1) {
  return pw::write_v2(write_array_program(mir, function, propto, island_arg,
                                          data_udf, force_interp,
                                          forced_outcome_count));
}

// data { real source; int outcome; vector[3] arg; }
// transformed data { real td_outcome = source; }
// and the categorical call takes the real td_outcome as its int outcome, in
// the log density or, with `in_gq`, in an added generated-quantities
// target increment.
inline mir::Program transformed_data_mismatch_program(bool in_gq) {
  mir::Program p = check_program("categorical_lpmf", true, 1, 3);
  mir::SizedType source;
  source.base = "SReal";
  p.input_vars.insert(p.input_vars.begin(), {"source", source});
  mir::Stmt td = pw::declare("td_outcome", "SReal");
  td.has_init = true;
  td.init = pw::var("source", real_view());
  p.prepare_data = {td};
  mir::Expr td_outcome = pw::var("td_outcome", int_view());
  if (!in_gq) {
    mir::Expr& call = expect_expr(p.log_prob, calls("categorical_lpmf"),
                                  "no categorical outcome call");
    call.args.at(0) = td_outcome;
  } else {
    p.generate_quantities.push_back(
        target_pe(density("categorical_lpmf", true,
                          {td_outcome, pw::var("arg", vector_view())})));
  }
  return p;
}

inline std::string categorical_transformed_data_mismatch_mir(bool in_gq) {
  return pw::write_v2(transformed_data_mismatch_program(in_gq));
}

}  // namespace categorical_mir

using categorical_mir::categorical_check_mir;
using categorical_mir::categorical_effect_check_mir;
using categorical_mir::categorical_parameter_effect_mir;
using categorical_mir::categorical_promoted_ternary_mir;
using categorical_mir::categorical_transformed_data_mismatch_mir;
using categorical_mir::categorical_udf_actual_mir;
using categorical_mir::categorical_udf_mir;
using categorical_mir::categorical_udf_promoted_actual_mir;
using categorical_mir::categorical_udf_return_mir;
using categorical_mir::categorical_udf_ternary_type_mir;
using categorical_mir::categorical_write_array_mir;

#endif
