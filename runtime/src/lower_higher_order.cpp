#include "lower_internal.hpp"
#include <stanli/reduce_sum.hpp>
#include <stanli/detail/input_ranges.hpp>

namespace stanli {
namespace lower_detail {

// Bind callback arguments [begin, end) of a retained call compiled in a
// Program. Data arguments fold into the spec; active ones are copied into
// one register run, which the kernel receives as theta.
Range Lowering::program_callback_theta(ProgramCompiler& c, const mir::Expr& e,
                                       size_t begin, size_t end,
                                       RetainedCallback& spec,
                                       int* parameter_count) {
  std::vector<Range> active = pack_callback_arguments<Range>(
      spec, e.args, begin, end,
      [&](size_t i) {
        Range value = c.expr(e.args[i]);
        return std::make_pair(value, value.len);
      },
      [&](size_t i) {
        DataMap::Entry value =
            program_constant(c, e.args[i], e.name + " data argument");
        if (value.is_int)
          c.bail(e.name + ": real data argument is integer-valued");
        const bool matrix = e.args[i].type_ == "UMatrix";
        const bool nested_matrix =
            e.args[i].unsized.depth != 0 &&
            e.args[i].unsized.leaf == mir::UnsizedLeaf::Matrix;
        return graph_order(value, matrix, nested_matrix);
      },
      [&](size_t i) {
        DataMap::Entry value =
            program_constant(c, e.args[i], e.name + " integer argument");
        if (!value.is_int) c.bail(e.name + ": integer argument is real-valued");
        return value.i;
      },
      [&](const std::string& message) { c.bail(e.name + ": " + message); });
  int total = 0;
  for (const Range& value : active) {
    if (value.len > ProgramCompiler::kMaxRegs - total)
      c.bail(e.name + ": active callback arguments are too large");
    total += value.len;
  }
  *parameter_count = total;
  Range theta{total == 0 ? c.konst(0.0) : c.alloc(total),
              total == 0 ? 1 : total};
  theta.kind = ViewKind::Vector;
  int at = 0;
  for (const Range& value : active)
    for (int k = 0; k < value.len; ++k)
      c.emit(Program::MOV, theta.reg + at++, value.reg + k);
  return theta;
}
// Retained higher-order algorithms use the graph kernel ABI even when
// their call site sits in a runtime-control Program. The Program owns the
// same specification object a graph Op would own, while kernel_call owns
// all register binding, scratch sizing, and reverse-mode wiring.
bool Lowering::lower_program_variadic_algebra(ProgramCompiler& c,
                                              const mir::Expr& e,
                                              Range* out_range) {
  const auto call = mir::algebra_call(e.name);
  if (!call || call->legacy) return false;
  if (e.args.size() < call->callback_args_begin ||
      e.args[0].kind != mir::Expr::Var)
    c.bail(e.name + ": expected a callback and an initial guess");
  if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Vector)
    c.bail(e.name + ": result must be a vector");

  std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector}};
  for (size_t i = call->callback_args_begin; i < e.args.size(); ++i)
    views.push_back(e.args[i].unsized);
  const mir::FunDef* system =
      mir::resolve_callback(fun_defs, e.args[0].name, views);
  if (system == nullptr)
    c.bail(e.name + ": unknown algebraic system " + e.args[0].name);

  auto spec = std::make_shared<AlgebraSpec>();
  spec->adopt(fun_defs);
  spec->system_name = system->name;
  spec->select(*call);
  if (call->with_tolerance) {
    spec->relative_tolerance =
        program_scalar_real(c, e.args[2], e.name + " relative tolerance");
    spec->function_tolerance =
        program_scalar_real(c, e.args[3], e.name + " function tolerance");
    spec->max_num_steps =
        program_scalar_int(c, e.args[4], e.name + " maximum steps");
  }

  int parameter_count = 0;
  const Range theta = program_callback_theta(
      c, e, call->callback_args_begin, e.args.size(), *spec, &parameter_count);
  const Range x = c.expr(e.args[1]);
  if (x.kind != ViewKind::Vector)
    c.bail(e.name + ": initial guess must be a vector");
  spec->prog = compile_rhs_args(with_leading_time(*spec->system()),
                                *spec->funs(), x.len, spec->args);
  if (!spec->prog.ok)
    note_interpreter_fallback("the algebraic system " + spec->system_name,
                              spec->prog.why);

  Range result{0, x.len};
  result.kind = ViewKind::Vector;
  *out_range =
      c.kernel_call(OP_ALGEBRA_SOLVER, {x, theta}, result,
                    parameter_count == 0 ? 0u : 0x1u, 0x2u, {}, spec, e.name);
  return true;
}
bool Lowering::lower_program_quadrature(ProgramCompiler& c, const mir::Expr& e,
                                        Range* out_range) {
  const auto call = mir::quadrature_call(e.name);
  if (!call) return false;
  if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Real)
    c.bail(e.name + ": result must be a real");

  size_t callback_end = e.args.size();
  if (call->legacy) {
    if (e.args.size() != 6 && e.args.size() != 7)
      c.bail(e.name + ": expected 6 or 7 arguments");
    callback_end = 6;
  } else if (call->with_tolerance) {
    if (e.args.size() < 6)
      c.bail(e.name + ": expected controls followed by callback arguments");
  } else if (e.args.size() < 3) {
    c.bail(e.name + ": expected callback and integration bounds");
  }
  if (e.args[0].kind != mir::Expr::Var)
    c.bail(e.name + ": integrand is not a function name");

  std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Real},
                                      {0, mir::UnsizedLeaf::Real}};
  for (size_t i = call->callback_args_begin; i < callback_end; ++i)
    views.push_back(e.args[i].unsized);
  const mir::FunDef* integrand =
      mir::resolve_callback(fun_defs, e.args[0].name, views);
  if (!integrand) c.bail(e.name + ": unknown integrand " + e.args[0].name);

  auto spec = std::make_shared<QuadratureSpec>();
  spec->adopt(fun_defs);
  spec->callback_name = integrand->name;
  spec->method = call->method;
  if (call->legacy && e.args.size() == 7) {
    spec->relative_tolerance =
        program_scalar_real(c, e.args[6], "quadrature tolerance");
  } else if (call->with_tolerance) {
    spec->relative_tolerance =
        program_scalar_real(c, e.args[3], "quadrature relative tolerance");
    spec->absolute_tolerance =
        program_scalar_real(c, e.args[4], "quadrature absolute tolerance");
    spec->max_steps = static_cast<int>(
        program_scalar_int(c, e.args[5], "quadrature maximum steps"));
  }

  const Range theta =
      program_callback_theta(c, e, call->callback_args_begin, callback_end,
                             *spec, &spec->parameter_count);
  spec->prog =
      compile_rhs_args(*spec->callback(), *spec->funs(), 1, spec->args);
  if (!spec->prog.ok)
    note_interpreter_fallback("the quadrature integrand " + spec->callback_name,
                              spec->prog.why);

  const Range a = c.expr(e.args[1]);
  const Range b = c.expr(e.args[2]);
  if (!c.is_scalar(a) || !c.is_scalar(b))
    c.bail(e.name + ": integration bounds must be scalar");
  const uint8_t variant = static_cast<uint8_t>(
      (!e.args[1].data_only ? 0x1u : 0u) | (!e.args[2].data_only ? 0x2u : 0u) |
      (spec->parameter_count != 0 ? 0x4u : 0u));
  Range result{0, 1};
  *out_range = c.kernel_call(OP_QUADRATURE, {a, b, theta}, result, variant,
                             variant, {}, spec, e.name);
  return true;
}
bool Lowering::lower_program_ode(ProgramCompiler& c, const mir::Expr& e,
                                 Range* out_range) {
  const auto call = mir::ode_call(e.name);
  if (!call || call->method == mir::OdeMethod::Adjoint) return false;
  if (e.args.size() < call->callback_args_begin ||
      e.args[0].kind != mir::Expr::Var)
    c.bail(e.name + ": expected a right-hand side, state, and times");

  auto spec = std::make_shared<OdeSpec>();
  spec->adopt(fun_defs);
  spec->rhs_name = e.args[0].name;
  if (!spec->rhs())
    c.bail(e.name + ": unknown right-hand side " + spec->rhs_name);
  spec->solver = ode_solver(call->method);
  spec->legacy = call->legacy;
  spec->stiff = spec->solver == OdeSpec::BDF || spec->solver == OdeSpec::ADAMS;
  stamp_ode_defaults(*spec);
  const Range z0 = c.expr(e.args[1]);
  if ((!call->legacy && z0.kind != ViewKind::Vector) ||
      (call->legacy && z0.kind != ViewKind::Array))
    c.bail(e.name + ": initial state has the wrong logical type");
  const int S = z0.len;
  Range t0{0, 1}, ts;
  if (call->legacy) {
    spec->t0 = program_scalar_real(c, e.args[2], "ODE initial time");
    spec->ts = program_vector_real(c, e.args[3], "ODE output times");
  } else {
    t0 = c.expr(e.args[2]);
    ts = c.expr(e.args[3]);
    if (!c.is_scalar(t0)) c.bail("ODE initial time must be scalar");
    if (ts.kind != ViewKind::Array) c.bail("ODE output times must be an array");
    spec->ts.resize((size_t)ts.len);
  }
  const int64_t N = call->legacy ? (int64_t)spec->ts.size() : ts.len;
  if (S < 0 || N < 0 || (N && S > ProgramCompiler::kMaxRegs / N))
    c.bail(e.name + ": result is too large");

  Range theta;
  bool theta_active = false;
  if (call->legacy) {
    if (e.args.size() != 7 && e.args.size() != 10)
      c.bail(e.name + ": expected 7 or 10 arguments");
    theta = c.expr(e.args[4]);
    if (theta.kind != ViewKind::Array)
      c.bail(e.name + ": parameters are not an array");
    theta_active = !e.args[4].data_only;
    spec->x_r = program_vector_real(c, e.args[5], "ODE real data");
    DataMap::Entry xi = program_constant(c, e.args[6], "ODE integer data");
    if (!xi.is_int) c.bail(e.name + ": integer data is real-valued");
    spec->x_i.assign(xi.i.begin(), xi.i.end());
    if (e.args.size() == 10) {
      spec->rtol = program_scalar_real(c, e.args[7], "ODE relative tolerance");
      spec->atol = program_scalar_real(c, e.args[8], "ODE absolute tolerance");
      spec->max_steps = program_scalar_int(c, e.args[9], "ODE maximum steps");
    }
    spec->args.resize(3);
    spec->args[0].is_param = true;
    spec->args[0].len = theta.len;
    spec->args[1].len = (int)spec->x_r.size();
    spec->args[2].is_int = true;
    spec->args[2].ints = spec->x_i;
    spec->prog = compile_rhs(*spec->rhs(), *spec->funs(), S, theta.len,
                             (int)spec->x_r.size(), spec->x_i);
  } else {
    if (call->with_tolerance) {
      spec->rtol = program_scalar_real(c, e.args[4], "ODE relative tolerance");
      spec->atol = program_scalar_real(c, e.args[5], "ODE absolute tolerance");
      spec->max_steps = program_scalar_int(c, e.args[6], "ODE maximum steps");
    }
    int parameter_count = 0;
    theta = program_callback_theta(c, e, call->callback_args_begin,
                                   e.args.size(), *spec, &parameter_count);
    theta_active = parameter_count != 0;
    spec->prog = compile_rhs_args(*spec->rhs(), *spec->funs(), S, spec->args);
    if (!spec->prog.ok)
      note_interpreter_fallback("the ODE right-hand side " + spec->rhs_name,
                                spec->prog.why);
  }

  Range result{0, (int)(N * S)};
  result.kind = ViewKind::Array;
  result.dims = {N, S};
  result.leaf = call->legacy ? ViewKind::Flat : ViewKind::Vector;
  const uint8_t activity = static_cast<uint8_t>(
      (e.args[1].data_only ? 0u : 0x1u) | (theta_active ? 0x2u : 0u) |
      (!call->legacy && !e.args[2].data_only ? 0x4u : 0u) |
      (!call->legacy && !e.args[3].data_only ? 0x8u : 0u));
  if (call->legacy) {
    *out_range = c.kernel_call(OP_ODE, {z0, theta}, result,
                               static_cast<uint8_t>(0x4u | activity), activity,
                               {(int)N, S}, spec, e.name);
  } else {
    *out_range = c.kernel_call(OP_ODE, {z0, theta, t0, ts}, result,
                               static_cast<uint8_t>(0x10u | activity), activity,
                               {(int)N, S}, spec, e.name);
  }
  return true;
}
bool Lowering::lower_program_ode_adjoint(ProgramCompiler& c, const mir::Expr& e,
                                         Range* out_range) {
  const auto call = mir::ode_call(e.name);
  if (!call || call->method != mir::OdeMethod::Adjoint) return false;
  if (e.args.size() < call->callback_args_begin ||
      e.args[0].kind != mir::Expr::Var)
    c.bail(e.name + ": expected a right-hand side and solver controls");

  auto spec = std::make_shared<OdeAdjointSpec>();
  spec->adopt(fun_defs);
  spec->rhs_name = e.args[0].name;
  spec->callback_name = spec->rhs_name;
  if (!spec->rhs())
    c.bail(e.name + ": unknown right-hand side " + spec->rhs_name);
  const auto real = [&](size_t i, const char* role) {
    return program_scalar_real(c, e.args[i],
                               std::string("adjoint ODE ") + role);
  };
  const auto reals = [&](size_t i, const char* role) {
    return program_vector_real(c, e.args[i],
                               std::string("adjoint ODE ") + role);
  };
  const auto integer = [&](size_t i, const char* role) {
    return program_scalar_int(c, e.args[i], std::string("adjoint ODE ") + role);
  };
  spec->relative_tolerance_forward = real(4, "forward relative tolerance");
  spec->absolute_tolerance_forward = reals(5, "forward absolute tolerance");
  spec->relative_tolerance_backward = real(6, "backward relative tolerance");
  spec->absolute_tolerance_backward = reals(7, "backward absolute tolerance");
  spec->relative_tolerance_quadrature =
      real(8, "quadrature relative tolerance");
  spec->absolute_tolerance_quadrature =
      real(9, "quadrature absolute tolerance");
  spec->max_num_steps = integer(10, "maximum steps");
  spec->num_steps_between_checkpoints = integer(11, "checkpoint interval");
  spec->interpolation_polynomial = (int)integer(12, "interpolation polynomial");
  spec->solver_forward = (int)integer(13, "forward solver");
  spec->solver_backward = (int)integer(14, "backward solver");

  const Range y0 = c.expr(e.args[1]);
  const Range t0 = c.expr(e.args[2]);
  const Range ts = c.expr(e.args[3]);
  if (y0.kind != ViewKind::Vector)
    c.bail(e.name + ": initial state must be a vector");
  if (!c.is_scalar(t0)) c.bail(e.name + ": initial time must be scalar");
  if (ts.kind != ViewKind::Array)
    c.bail(e.name + ": output times must be an array");
  const int S = y0.len;
  const int N = ts.len;
  if ((int)spec->absolute_tolerance_forward.size() != S ||
      (int)spec->absolute_tolerance_backward.size() != S)
    c.bail(e.name + ": absolute tolerance vectors must match state size");
  if (S < 0 || N < 0 || (N && S > ProgramCompiler::kMaxRegs / N))
    c.bail(e.name + ": result is too large");

  int parameter_count = 0;
  const Range theta = program_callback_theta(
      c, e, call->callback_args_begin, e.args.size(), *spec, &parameter_count);
  spec->prog = compile_rhs_args(*spec->rhs(), *spec->funs(), S, spec->args);
  if (!spec->prog.ok)
    note_interpreter_fallback(
        "the adjoint ODE right-hand side " + spec->rhs_name, spec->prog.why);

  Range result{0, N * S};
  result.kind = ViewKind::Array;
  result.dims = {N, S};
  result.leaf = ViewKind::Vector;
  const uint8_t activity = static_cast<uint8_t>(
      (!e.args[1].data_only ? 0x1u : 0u) | (!e.args[2].data_only ? 0x2u : 0u) |
      (!e.args[3].data_only ? 0x4u : 0u) | (parameter_count != 0 ? 0x8u : 0u));
  *out_range = c.kernel_call(OP_ODE_ADJOINT, {y0, t0, ts, theta}, result,
                             static_cast<uint8_t>(0x10u | activity), activity,
                             {N, S}, spec, e.name);
  return true;
}
bool Lowering::lower_program_dae(ProgramCompiler& c, const mir::Expr& e,
                                 Range* out_range) {
  const auto call = mir::dae_call(e.name);
  if (!call) return false;
  if (e.args.size() < call->callback_args_begin ||
      e.args[0].kind != mir::Expr::Var)
    c.bail(e.name + ": expected a residual, initial conditions, and times");

  auto spec = std::make_shared<DaeSpec>();
  spec->adopt(fun_defs);
  spec->residual_name = e.args[0].name;
  spec->callback_name = spec->residual_name;
  if (!spec->residual())
    c.bail(e.name + ": unknown residual " + spec->residual_name);
  spec->t0 = program_scalar_real(c, e.args[3], "DAE initial time");
  spec->ts = program_vector_real(c, e.args[4], "DAE output times");
  if (call->with_tolerance) {
    spec->rtol = program_scalar_real(c, e.args[5], "DAE relative tolerance");
    spec->atol = program_scalar_real(c, e.args[6], "DAE absolute tolerance");
    spec->max_steps = program_scalar_int(c, e.args[7], "DAE maximum steps");
  }

  const Range y0 = c.expr(e.args[1]);
  const Range yp0 = c.expr(e.args[2]);
  if (y0.kind != ViewKind::Vector || yp0.kind != ViewKind::Vector)
    c.bail(e.name + ": initial state and derivative must be vectors");
  if (y0.len != yp0.len)
    c.bail(e.name + ": initial state and derivative sizes differ");
  const int S = y0.len;
  const int64_t N = (int64_t)spec->ts.size();
  if (S < 0 || N < 0 || (N && S > ProgramCompiler::kMaxRegs / N))
    c.bail(e.name + ": result is too large");

  int parameter_count = 0;
  const Range theta = program_callback_theta(
      c, e, call->callback_args_begin, e.args.size(), *spec, &parameter_count);
  spec->prog =
      compile_dae_args(*spec->residual(), *spec->funs(), S, spec->args);
  if (!spec->prog.ok)
    note_interpreter_fallback("the DAE residual " + spec->residual_name,
                              spec->prog.why);

  Range result{0, (int)(N * S)};
  result.kind = ViewKind::Array;
  result.dims = {N, S};
  result.leaf = ViewKind::Vector;
  const uint8_t activity = static_cast<uint8_t>(
      (!e.args[1].data_only ? 0x1u : 0u) | (!e.args[2].data_only ? 0x2u : 0u) |
      (parameter_count != 0 ? 0x4u : 0u));
  *out_range = c.kernel_call(OP_DAE, {y0, yp0, theta}, result,
                             static_cast<uint8_t>(0x8u | activity), activity,
                             {(int)N, S}, spec, e.name);
  return true;
}
bool Lowering::lower_program_higher_order(ProgramCompiler& c,
                                          const mir::Expr& e,
                                          Range* out_range) {
  const auto higher_order = mir::higher_order_call(e);
  if (!higher_order) return false;
  if (higher_order->family == mir::HigherOrderFamily::Integrate1D)
    return lower_program_quadrature(c, e, out_range);
  if (higher_order->family == mir::HigherOrderFamily::Ode)
    return lower_program_ode_adjoint(c, e, out_range) ||
           lower_program_ode(c, e, out_range);
  if (higher_order->family == mir::HigherOrderFamily::Dae)
    return lower_program_dae(c, e, out_range);
  if (higher_order->family != mir::HigherOrderFamily::Algebra) return false;
  if (lower_program_variadic_algebra(c, e, out_range)) return true;
  if ((e.name != "algebra_solver" && e.name != "algebra_solver_newton"))
    return false;
  if (e.args.size() != 5 && e.args.size() != 8)
    c.bail("algebra_solver: expected 5 or 8 arguments");
  if (e.args[0].kind != mir::Expr::Var)
    c.bail("algebra_solver: system is not a function name");
  if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Vector)
    c.bail("algebra_solver: result must be a vector");

  const std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector},
                                            {0, mir::UnsizedLeaf::Vector},
                                            {1, mir::UnsizedLeaf::Real},
                                            {1, mir::UnsizedLeaf::Int}};
  const mir::FunDef* system =
      mir::resolve_callback(fun_defs, e.args[0].name, views);
  if (system == nullptr)
    c.bail("algebra_solver: unknown algebraic system " + e.args[0].name);
  if (system->arg_names.size() != views.size())
    c.bail("algebra_solver: system argument metadata is incomplete");

  auto spec = std::make_shared<AlgebraSpec>();
  spec->adopt(fun_defs);
  spec->system_name = system->name;
  spec->select(*mir::algebra_call(e.name));
  spec->x_r = program_vector_real(c, e.args[3], "algebra_solver x_r");
  DataMap::Entry xi = program_constant(c, e.args[4], "algebra_solver x_i");
  if (!xi.is_int || xi.i.size() != xi.r.size())
    c.bail("algebra_solver: malformed integer data argument");
  spec->x_i.assign(xi.i.begin(), xi.i.end());
  if (e.args.size() == 8) {
    spec->relative_tolerance =
        program_scalar_real(c, e.args[5], "algebra_solver relative tolerance");
    spec->function_tolerance =
        program_scalar_real(c, e.args[6], "algebra_solver function tolerance");
    spec->max_num_steps =
        program_scalar_int(c, e.args[7], "algebra_solver maximum steps");
  }

  const Range x = c.expr(e.args[1]);
  const Range y = c.expr(e.args[2]);
  if (x.kind != ViewKind::Vector || y.kind != ViewKind::Vector)
    c.bail("algebra_solver: initial guess and parameters must be vectors");
  if (x.len < 0 || y.len < 0 ||
      spec->x_r.size() > (size_t)std::numeric_limits<int>::max())
    c.bail("algebra_solver: argument is too large");

  std::vector<RhsArg> args(3);
  args[0].is_param = true;
  args[0].len = y.len;
  args[1].len = (int)spec->x_r.size();
  args[2].is_int = true;
  args[2].ints = spec->x_i;
  spec->prog = compile_rhs_args(with_leading_time(*spec->system()),
                                *spec->funs(), x.len, args);
  if (!spec->prog.ok)
    note_interpreter_fallback("the algebraic system " + spec->system_name,
                              spec->prog.why);

  Range result{0, x.len};
  result.kind = ViewKind::Vector;
  const uint8_t active = e.args[2].data_only ? 0u : 0x1u;
  *out_range = c.kernel_call(OP_ALGEBRA_SOLVER, {x, y}, result, active, 0x2u,
                             {}, spec, e.name);
  return true;
}
// map_rect checks that the three job arrays have matching OUTER sizes and
// returns an empty vector before touching the shared parameters or the UDF
// when that size is zero.  This is the one map_rect case which needs no
// runtime callback at all (and is exercised by stanc3's mother model).
// Nonempty calls deliberately keep falling through to the unsupported
// function diagnostic.
std::optional<Lowering::Val> Lowering::lower_empty_map_rect(
    const mir::Expr& e, CallArguments& actuals) {
  if (e.name != "map_rect") return std::nullopt;
  if (actuals.size() != 5)
    fail(
        "map_rect: expected function, shared parameters, job parameters, "
        "real data, and integer data",
        e.raw);

  // A non-variable shared-parameter expression still has to be evaluated
  // before map_rect can take its empty-job return.  Plain zero-length
  // locals have no materialized slot, so their declared view is enough.
  SlotInfo shared_si;
  const mir::Expr& shared_expr = actuals.at(1).expr();
  if (shared_expr.kind == mir::Expr::Var) {
    auto declared = decls.find(shared_expr.name);
    if (declared != decls.end()) shared_si = declared->second.si;
  }
  if (!is_vector(shared_si)) shared_si = actuals.at(1).value().si;
  if (!is_vector(shared_si))
    fail("map_rect: shared parameters are not a vector", e.raw);

  // A default-initialized zero-length local has declaration geometry but
  // no scope value: there are no elements to initialize or materialize.
  // map_rect does not read it on this branch, so consult decls before
  // asking lower_expr for a slot (mother's `tmp2` has exactly this form).
  SlotInfo job_si;
  const mir::Expr& job_expr = actuals.at(2).expr();
  if (job_expr.kind == mir::Expr::Var) {
    auto declared = decls.find(job_expr.name);
    if (declared != decls.end()) job_si = declared->second.si;
  }
  if (!is_array(job_si)) job_si = actuals.at(2).value().si;
  if (!is_array(job_si)) return std::nullopt;
  const ArrayShape& job_shape = array_shape(job_si);
  const size_t job_outer =
      job_shape.dims.size() - (size_t)leaf_rank(job_shape.leaf);
  if (job_shape.leaf != ViewKind::Vector || job_outer != 1 ||
      job_shape.dims.front() != 0)
    return std::nullopt;

  Val real_data = actuals.at(3).value();
  Val int_data = actuals.at(4).value();
  if (!is_array(real_data.si) || !is_array(int_data.si))
    fail("map_rect: job data arguments are not arrays", e.raw);
  const ArrayShape& real_shape = array_shape(real_data.si);
  const ArrayShape& int_shape = array_shape(int_data.si);
  if (real_shape.leaf != ViewKind::Flat || int_shape.leaf != ViewKind::Flat ||
      real_shape.dims.size() != 2 || int_shape.dims.size() != 2)
    fail("map_rect: job data arguments do not have two array dimensions",
         e.raw);
  if (real_shape.dims.front() != 0 || int_shape.dims.front() != 0)
    fail("map_rect: job parameters and job data sizes do not match", e.raw);
  if (e.unsized.leaf != mir::UnsizedLeaf::Vector || e.unsized.depth != 0)
    fail("map_rect: result is not a vector", e.raw);

  SlotInfo si = view_of(e.type_);
  si.param_free = true;
  const int slot = add_slot(0, false);
  out.fills.emplace_back(slot, std::vector<double>{});
  return Val{slot, false, si, owning_layout(si)};
}
mir::Expr Lowering::slice_bound_literal(int64_t value, const std::string& raw) {
  if (value > std::numeric_limits<int32_t>::max())
    fail("reduce_sum: slice bound exceeds the Stan integer range", raw);
  mir::Expr literal;
  literal.kind = mir::Expr::LitInt;
  literal.lit_i = static_cast<long>(value);
  literal.lit = static_cast<double>(value);
  literal.type_ = "UInt";
  literal.unsized = {0, mir::UnsizedLeaf::Int};
  literal.data_only = true;
  literal.raw = raw;
  return literal;
}
// Only publish after every child has lowered and passed the fixed-range proof.
// The ordinary UDF binder has already evaluated arguments and validation once;
// refusal therefore proceeds with that exact same binding and parent graph.
std::optional<Lowering::Val> Lowering::try_lower_parallel_reduce_sum(
    const mir::Expr& call, const std::vector<UdfBinding>& binds, int64_t count,
    int64_t grain, bool fixed_partition) {
  if (compile_options.reduce_sum_threads <= 1) return std::nullopt;
  const auto refuse = [&](const std::string& reason) -> std::optional<Val> {
    const std::string note = call.name + ": " + reason;
    if (std::find(out.reduce_sum_fallbacks.begin(),
                  out.reduce_sum_fallbacks.end(),
                  note) == out.reduce_sum_fallbacks.end())
      out.reduce_sum_fallbacks.push_back(note);
    return std::nullopt;
  };
#if !defined(STAN_THREADS) || defined(__EMSCRIPTEN__)
  return refuse("within-chain workers require a TLS-safe native build");
#endif
  if (region_current || in_write_array)
    return refuse("retained control-flow or write-array context");
  if (count < compile_options.reduce_sum_min_elements)
    return refuse("slice below reduce_sum_min_elements");
  if (count <= 1) return refuse("slice has at most one element");
  if (grain <= 0) return refuse("grainsize is not a known positive integer");
  if (fun_effectful(call.name))
    return refuse("callback has observable effects");
  const int64_t needed = 1 + (count - 1) / grain;
  const int64_t chunks =
      fixed_partition
          ? needed
          : std::min<int64_t>(needed, compile_options.reduce_sum_threads);
  if (chunks <= 1) return refuse("grainsize selects one chunk");
  if (chunks > compile_options.reduce_sum_max_chunks)
    return refuse("partition exceeds reduce_sum_max_chunks");
  std::vector<Val> inputs;
  std::map<int, int> parent_index;
  for (const auto& bind : binds) {
    if (bind.is_int) continue;
    if (!bind.v.runtime_dims.empty() || bind.v.slot < 0)
      return refuse("argument has runtime geometry");
    auto found = parent_index.find(bind.v.slot);
    if (found == parent_index.end()) {
      parent_index[bind.v.slot] = static_cast<int>(inputs.size());
      inputs.push_back(bind.v);
    } else {
      auto& value = inputs[found->second];
      value.autodiff |= bind.v.autodiff;
      value.si.param_free &= bind.v.si.param_free;
    }
  }
  auto spec = std::make_shared<ReduceSumSpec>();
  bool active = false, param_free = true;
  try {
    for (int64_t chunk = 0; chunk < chunks; ++chunk) {
      const int64_t first =
          fixed_partition ? chunk * grain : count * chunk / chunks;
      const int64_t last = fixed_partition ? std::min(count, first + grain)
                                           : count * (chunk + 1) / chunks;
      Lowering trial = fork_region_trial();
      trial.compile_options.reduce_sum_threads = 1;
      trial.structured_policy = StructuredMode::Off;
      std::vector<int> imported;
      for (const auto& input : inputs)
        imported.push_back(
            trial.add_slot(g.slots[input.slot].len, input.autodiff));
      mir::Expr invocation = call;
      for (size_t i = 0; i < binds.size(); ++i) {
        if (i == 1 || i == 2 || binds[i].is_int) {
          invocation.args[i] = slice_bound_literal(i == 1   ? first + 1
                                                   : i == 2 ? last
                                                            : binds[i].iv,
                                                   call.raw);
          continue;
        }
        Val value = binds[i].v;
        value.slot = imported.at(parent_index.at(value.slot));
        const std::string name =
            "(reduce_sum argument " + std::to_string(i) + ")";
        trial.scope[name] = value;
        trial.decls[name] =
            DeclView{trial.g.slots[value.slot].len, value.autodiff, value.si};
        if (binds[i].data) {
          trial.td.env()[name] = *binds[i].data;
          trial.observe(value, *binds[i].data);
        }
        auto arg = call.args[i];
        arg.kind = mir::Expr::Var;
        arg.name = name;
        arg.args.clear();
        if (i == 0) {
          mir::Expr range;
          range.kind = mir::Expr::FunApp;
          range.name = "IndexBetween";
          range.args = {slice_bound_literal(first + 1, call.raw),
                        slice_bound_literal(last, call.raw)};
          mir::Expr sliced = arg;
          sliced.kind = mir::Expr::Indexed;
          sliced.args = {arg, range};
          arg = std::move(sliced);
        }
        invocation.args[i] = std::move(arg);
      }
      const Val result = trial.lower_call_udf(invocation);
      if (!trial.target_terms.empty() || !trial.jac_slots.empty() ||
          trial.g.slots[result.slot].len != 1)
        return refuse(
            "callback changes target/Jacobian state or returns a nonscalar");
      active |= result.autodiff;
      param_free &= result.si.param_free;
      // Roots stay addressable through these ordinary graph passes. Opaque
      // fallback programs and stateful kernels conservatively refuse below.
      // Imported inactive inputs are runtime bindings, not initialized fills.
      // Constant folding would evaluate their readers against zero storage.
      trial.run_passes({result.slot}, PassPlan{false, true, true, true, false});
      trial.g.result_slot = result.slot;
      std::vector<detail::Import> parameter_map;
      std::vector<int64_t> offsets;
      std::string reason;
      if (!detail::compact_imports(trial.g, trial.out.fills, parameter_map,
                                   &offsets, &reason))
        return refuse(reason);
      ReduceSumSpec::Child child;
      for (size_t i = 0; i < imported.size(); ++i)
        if (trial.g.slots[imported[i]].len)
          child.imports.push_back(
              {imported[i], static_cast<int>(i), offsets[imported[i]]});
      child.graph = std::move(trial.g);
      child.fills = std::move(trial.out.fills);
      spec->children.push_back(std::move(child));
    }
  } catch (const CompileError& error) {
    return refuse(std::string("child lowering: ") + error.what());
  }
  // Drop unneeded operand bindings; argument evaluation has already happened.
  std::vector<bool> used(inputs.size(), false);
  for (const auto& child : spec->children)
    for (const auto& in : child.imports) used[in.input] = true;
  std::vector<int> remap(inputs.size(), -1);
  std::vector<Val> live;
  for (size_t i = 0; i < inputs.size(); ++i)
    if (used[i]) {
      remap[i] = static_cast<int>(live.size());
      live.push_back(inputs[i]);
    }
  for (auto& child : spec->children)
    for (auto& in : child.imports) in.input = remap[in.input];
  // No semantic refusal after this point. Allocation failure aborts
  // compilation.
  if (live.size() > 6) {
    const size_t packed_count = live.size() - 5;
    std::vector<int64_t> offsets(packed_count, 0);
    Val packed = live[0];
    int64_t length = g.slots[packed.slot].len;
    for (size_t i = 1; i < packed_count; ++i) {
      offsets[i] = length;
      if (g.slots[live[i].slot].len >
          std::numeric_limits<int64_t>::max() - length)
        throw std::length_error("reduce_sum packed input size overflow");
      length += g.slots[live[i].slot].len;
      packed = emit_value(OP_CONCAT2, {packed, live[i]}, length);
    }
    for (auto& child : spec->children)
      for (auto& in : child.imports) {
        if (size_t(in.input) < packed_count) {
          in.offset += offsets[in.input];
          in.input = 0;
        } else
          in.input -= static_cast<int>(packed_count - 1);
      }
    live.erase(live.begin(), live.begin() + packed_count);
    live.insert(live.begin(), packed);
  }
  std::vector<int> slots;
  for (const auto& in : live) slots.push_back(in.slot);
  Val result = emit_raw(OP_REDUCE_SUM, slots, 1, view_of(call.type_));
  result.autodiff = active;
  result.si.param_free = param_free;
  g.ops.back().udata = spec.get();
  g.udata_pool.push_back(std::move(spec));
  return result;
}
// reduce_sum(f, sliced, grainsize, shared...) sums f over the terms of a
// partition of `sliced`, and its contract is that the partition is
// unobservable: the terms must sum to the same value however the slice is
// cut. Stan Math without STAN_THREADS takes that freedom to its limit and
// makes exactly one call over the whole slice, returning zero for an empty
// one (prim/functor/reduce_sum.hpp). This remains Stanli's default and
// conservative fallback. Opt-in native compilation may retain independently
// lowered, compact children after the generic shape/effect checks above.
//
// Written out, the call is an ordinary user-function call, so this rewrites
// it to f(sliced, 1, size(sliced), shared...) and hands that to the
// inliner, which already owns argument binding, propto threading, and the
// data-only formal rules.
Lowering::Val Lowering::lower_reduce_sum(const mir::Expr& e,
                                         CallArguments& actuals) {
  if (actuals.size() < 3)
    fail(
        "reduce_sum: expected a partial-sum function, a sliced argument, "
        "and a grainsize",
        e.raw);
  const mir::Expr& partial_expr = actuals.at(0).expr();
  if (partial_expr.kind != mir::Expr::Var)
    fail("reduce_sum: the partial-sum argument is not a function name", e.raw);
  if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Real)
    fail("reduce_sum: result is not a real", e.raw);

  Val slice = actuals.at(1).value();
  if (!is_array(slice.si))
    fail("reduce_sum: the sliced argument is not an array", e.raw);
  // Evaluating grainsize and checking positivity are observable even
  // when this call keeps the whole-slice serial lowering. Do not swallow a
  // failed compile-time probe or execute effectful expressions while lowering.
  // Keep pure data integer operations on the interpreter path: operations
  // such as divide(int, int) need not have a runtime graph kernel. Failed
  // folding still lowers (or refuses) the expression; it never drops it.
  const auto folded_grainsize = actuals.at(2).try_fold();
  const Val grainsize =
      folded_grainsize ? *folded_grainsize : actuals.at(2).value();
  const mir::Expr& grainsize_expr = actuals.at(2).expr();
  if (grainsize_expr.unsized.depth != 0 ||
      grainsize_expr.unsized.leaf != mir::UnsizedLeaf::Int ||
      !is_scalar(grainsize))
    fail("reduce_sum: grainsize is not an integer scalar", e.raw);
  const auto check_grainsize = [&] {
    auto spec = std::make_shared<BoundCheckSpec>();
    spec->name = "reduce_sum grainsize";
    spec->bound_is_scalar = true;
    spec->shapes_match = true;
    (void)emit_value(OP_CHECK_LOWER, {grainsize, constant(1.0)}, 1);
    g.ops.back().udata = spec.get();
    g.udata_pool.push_back(std::move(spec));
  };
  const int64_t n = array_shape(slice.si).dims.front();
  if (n == 0) {
    if (compile_options.reduce_sum_threads > 1)
      out.reduce_sum_fallbacks.push_back("reduce_sum: empty slice");
    // C++ evaluates shared arguments before reduce_sum can return zero.
    // Only the partial-sum body is skipped for an empty slice.
    for (size_t i = 3; i < actuals.size(); ++i) (void)actuals.at(i).value();
    check_grainsize();
    return constant(0.0);
  }

  bool propto = false;
  const std::string base =
      mir::reduce_sum_partial_name(partial_expr.name, &propto);
  const std::vector<mir::UnsizedView> views = mir::reduce_sum_partial_views(e);
  const mir::FunDef* f = mir::resolve_callback(fun_defs, base, views);
  if (f == nullptr)
    fail("reduce_sum: unknown partial-sum function " + base, e.raw);
  if (f->arg_views.size() != views.size())
    fail("reduce_sum: " + base + " takes " +
             std::to_string(f->arg_views.size()) +
             " arguments, but reduce_sum calls it with " +
             std::to_string(views.size()),
         e.raw);
  if (f->arg_views[1].depth != 0 ||
      f->arg_views[1].leaf != mir::UnsizedLeaf::Int ||
      f->arg_views[2].depth != 0 ||
      f->arg_views[2].leaf != mir::UnsizedLeaf::Int)
    fail("reduce_sum: " + base + " must take the two slice bounds as integers",
         e.raw);

  // Bind the lowered slice under a name no Stan identifier can collide
  // with, so the rewritten call can name it instead of lowering the slice
  // expression a second time. resolve_overloads borrows Stan's syntax for
  // the same purpose.
  const std::string bound =
      "(reduce_sum slice " + std::to_string(reduce_sum_slices++) + ")";
  scope[bound] = slice;
  decls[bound] =
      DeclView{g.slots[slice.slot].len, slice.autodiff, slice.si, false, false};

  mir::Expr call;
  call.kind = mir::Expr::FunApp;
  call.fn_lib = mir::Expr::Lib::UserDefined;
  call.name = f->name;
  // lower_call_udf reads this exactly as CmdStan reads the generated
  // functor's propto__ argument: an `_lupdf` functor inherits the caller's
  // normalization, an `_lpdf` one forces the normalized density.
  call.fn_propto = propto;
  call.type_ = e.type_;
  call.unsized = e.unsized;
  call.data_only = e.data_only;
  call.raw = e.raw;
  call.args.reserve(views.size());
  mir::Expr sliced;
  sliced.kind = mir::Expr::Var;
  sliced.name = bound;
  const mir::Expr& slice_expr = actuals.at(1).expr();
  sliced.type_ = slice_expr.type_;
  sliced.unsized = slice_expr.unsized;
  sliced.data_only = slice_expr.data_only;
  sliced.raw = slice_expr.raw;
  call.args.push_back(std::move(sliced));
  call.args.push_back(slice_bound_literal(1, e.raw));
  call.args.push_back(slice_bound_literal(n, e.raw));
  for (size_t i = 3; i < actuals.size(); ++i)
    call.args.push_back(actuals.at(i).expr());

  Val result{-1, false, {}};
  try {
    int64_t grain = 0;
    if (compile_options.reduce_sum_threads > 1 && folded_grainsize &&
        !region_current && !in_write_array) {
      const auto* observed = observation(*folded_grainsize);
      if (observed && observed->r.size() == 1 && observed->r[0] >= 1 &&
          observed->r[0] <= std::numeric_limits<int32_t>::max())
        grain = static_cast<int64_t>(observed->r[0]);
    }
    result = lower_call_udf(
        call, check_grainsize,
        [&](const std::vector<UdfBinding>& binds) -> std::optional<Val> {
          return try_lower_parallel_reduce_sum(call, binds, n, grain,
                                               e.name == "reduce_sum_static");
        });
  } catch (...) {
    scope.erase(bound);
    decls.erase(bound);
    throw;
  }
  scope.erase(bound);
  decls.erase(bound);
  return result;
}
// The deprecated algebra_solver interfaces (Powell and Newton):
//
//   algebra_solver(f, x, y, x_r, x_i[, rel_tol, f_tol, max_steps])
//
// x is an initial guess.  It influences which root is selected but legacy
// Stan Math intentionally returns value_type_t<y>, so only y participates
// in autodiff.  Keep x as a graph input for values while stamping the op's
// activity and result scalar type from y alone.
Lowering::Val Lowering::lower_quadrature_fn(const mir::Expr& e,
                                            CallArguments& actuals) {
  const auto call = mir::quadrature_call(e.name);
  if (!call) fail(e.name + ": missing quadrature metadata", e.raw);
  if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Real)
    fail(e.name + ": result must be a real", e.raw);

  size_t callback_end = actuals.size();
  if (call->legacy) {
    if (actuals.size() != 6 && actuals.size() != 7)
      fail(e.name + ": expected 6 or 7 arguments", e.raw);
    callback_end = 6;
  } else if (call->with_tolerance) {
    if (actuals.size() < 6)
      fail(e.name + ": expected controls followed by callback arguments",
           e.raw);
  } else if (actuals.size() < 3) {
    fail(e.name + ": expected callback and integration bounds", e.raw);
  }
  if (e.args[0].kind != mir::Expr::Var)
    fail(e.name + ": integrand is not a function name", e.raw);

  std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Real},
                                      {0, mir::UnsizedLeaf::Real}};
  for (size_t i = call->callback_args_begin; i < callback_end; ++i)
    views.push_back(e.args[i].unsized);
  const mir::FunDef* integrand =
      mir::resolve_callback(fun_defs, e.args[0].name, views);
  if (!integrand) fail(e.name + ": unknown integrand " + e.args[0].name, e.raw);

  auto spec = std::make_shared<QuadratureSpec>();
  spec->adopt(fun_defs);
  spec->callback_name = integrand->name;
  spec->method = call->method;
  if (call->legacy && actuals.size() == 7) {
    spec->relative_tolerance =
        actuals.at(6).require_constant_reals("quadrature tolerance").at(0);
  } else if (call->with_tolerance) {
    spec->relative_tolerance =
        actuals.at(3)
            .require_constant_reals("quadrature relative tolerance")
            .at(0);
    spec->absolute_tolerance =
        actuals.at(4)
            .require_constant_reals("quadrature absolute tolerance")
            .at(0);
    spec->max_steps = static_cast<int>(
        actuals.at(5).require_constant_int("quadrature maximum steps"));
  }

  std::vector<Val> active = pack_callback_arguments<Val>(
      *spec, e.args, call->callback_args_begin, callback_end,
      [&](size_t i) {
        Val value = actuals.at(i).value();
        if (g.slots[value.slot].len > std::numeric_limits<int>::max())
          fail(e.name + ": callback argument is too large", e.raw);
        return std::make_pair(value, static_cast<int>(g.slots[value.slot].len));
      },
      [&](size_t i) {
        const auto& values =
            actuals.at(i).require_constant_reals("quadrature data argument");
        return std::vector<double>(values.begin(), values.end());
      },
      [&](size_t i) {
        const auto& values =
            actuals.at(i).require_constant_ints("quadrature integer argument");
        return std::vector<int>(values.begin(), values.end());
      },
      [&](const std::string& message) {
        fail(e.name + ": " + message, e.raw);
      });

  Val theta = constant(0.0);  // unread placeholder when there are no params
  spec->parameter_count = 0;
  if (!active.empty()) {
    theta = active.front();
    spec->parameter_count = static_cast<int>(g.slots[theta.slot].len);
    for (size_t i = 1; i < active.size(); ++i) {
      const int64_t add = g.slots[active[i].slot].len;
      if (add > std::numeric_limits<int>::max() - spec->parameter_count)
        fail(e.name + ": active callback arguments are too large", e.raw);
      theta = emit_value(OP_CONCAT2, {theta, active[i]},
                         spec->parameter_count + add);
      spec->parameter_count += static_cast<int>(add);
    }
  }
  spec->prog =
      compile_rhs_args(*spec->callback(), *spec->funs(), 1, spec->args);
  if (!spec->prog.ok)
    note_interpreter_fallback("the quadrature integrand " + spec->callback_name,
                              spec->prog.why);

  Val a = actuals.at(1).value();
  Val b = actuals.at(2).value();
  if (!is_scalar(a) || !is_scalar(b))
    fail(e.name + ": integration bounds must be scalar", e.raw);
  const uint8_t variant =
      static_cast<uint8_t>((a.autodiff ? 0x1u : 0u) | (b.autodiff ? 0x2u : 0u) |
                           (spec->parameter_count != 0 ? 0x4u : 0u));
  SlotInfo si = view_of(e.type_);
  si.param_free = variant == 0;
  Val result = emit_raw(OP_QUADRATURE, {a.slot, b.slot, theta.slot}, 1, si, {},
                        -1, variant != 0);
  g.ops.back().variant = variant;
  g.ops.back().udata = spec.get();
  g.udata_pool.push_back(std::move(spec));
  return result;
}
Lowering::Val Lowering::lower_algebra_fn(const mir::Expr& e,
                                         CallArguments& actuals) {
  if (actuals.size() != 5 && actuals.size() != 8)
    fail(e.name + ": expected 5 or 8 arguments", e.raw);
  if (e.unsized.leaf != mir::UnsizedLeaf::Vector || e.unsized.depth != 0)
    fail("algebra_solver: result must be a vector", e.raw);

  // Argument zero is a callback name, not a value acquisition. It must stay
  // source-level so the callback can be retained in AlgebraSpec.
  const mir::Expr& system_expr = actuals.at(0).expr();
  const std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector},
                                            {0, mir::UnsizedLeaf::Vector},
                                            {1, mir::UnsizedLeaf::Real},
                                            {1, mir::UnsizedLeaf::Int}};
  const mir::FunDef* resolved =
      mir::resolve_callback(fun_defs, system_expr.name, views);
  if (resolved == nullptr)
    fail("algebra_solver: unknown algebraic system " + system_expr.name, e.raw);
  const mir::FunDef& f = *resolved;
  if (f.arg_views.size() != 4 || f.arg_names.size() != 4 ||
      f.arg_types.size() != 4 || f.arg_views[0].depth != 0 ||
      f.arg_views[0].leaf != mir::UnsizedLeaf::Vector ||
      f.arg_views[1].depth != 0 ||
      f.arg_views[1].leaf != mir::UnsizedLeaf::Vector ||
      f.arg_views[2].depth != 1 ||
      f.arg_views[2].leaf != mir::UnsizedLeaf::Real ||
      f.arg_views[3].depth != 1 || f.arg_views[3].leaf != mir::UnsizedLeaf::Int)
    fail(
        "algebra_solver: system must take (vector, vector, array[] real, "
        "array[] int)",
        e.raw);

  auto spec = std::make_shared<AlgebraSpec>();
  spec->adopt(fun_defs);
  spec->system_name = f.name;
  spec->select(*mir::algebra_call(e.name));
  spec->x_r = actuals.at(3).require_constant_reals("algebra_solver x_r");
  spec->x_i = actuals.at(4).require_constant_ints("algebra_solver x_i");
  if (actuals.size() == 8) {
    spec->relative_tolerance =
        actuals.at(5)
            .require_constant_reals("algebra_solver relative tolerance")
            .at(0);
    spec->function_tolerance =
        actuals.at(6)
            .require_constant_reals("algebra_solver function tolerance")
            .at(0);
    spec->max_num_steps =
        actuals.at(7).require_constant_int("algebra_solver maximum steps");
  }

  Val x = actuals.at(1).value();
  Val y = actuals.at(2).value();
  if (!is_vector(x.si) || !is_vector(y.si))
    fail("algebra_solver: initial guess and parameters must be vectors", e.raw);
  const int64_t n = g.slots[x.slot].len;
  if (n > std::numeric_limits<int>::max() ||
      g.slots[y.slot].len > std::numeric_limits<int>::max() ||
      spec->x_r.size() > (size_t)std::numeric_limits<int>::max())
    fail(
        "algebra_solver: argument is too large for the callback register "
        "program",
        e.raw);

  std::vector<RhsArg> args(3);
  args[0].is_param = true;
  args[0].len = (int)g.slots[y.slot].len;
  args[1].len = (int)spec->x_r.size();
  args[2].is_int = true;
  args[2].ints = spec->x_i;
  spec->prog = compile_rhs_args(with_leading_time(*spec->system()),
                                *spec->funs(), (int)n, args);
  if (!spec->prog.ok)
    note_interpreter_fallback("the algebraic system " + spec->system_name,
                              spec->prog.why);
  if (!spec->prog.ok && std::getenv("STANLI_DEBUG_ALGEBRA"))
    emit_diagnostic("stanli: algebraic system " + spec->system_name +
                    " falls back to the interpreter: " + spec->prog.why);

  SlotInfo si = view_of(e.type_);
  si.param_free = x.si.param_free && y.si.param_free;
  Val result =
      emit_raw(OP_ALGEBRA_SOLVER, {x.slot, y.slot}, n, si, {}, -1, y.autodiff);
  g.ops.back().variant = y.autodiff ? 0x1u : 0x0u;
  g.ops.back().udata = spec.get();
  g.udata_pool.push_back(std::move(spec));
  return result;
}
// The op tail both ODE families share: report an interpreter fallback,
// emit OP_ODE and hand the spec to the graph.
Lowering::Val Lowering::emit_ode(std::shared_ptr<OdeSpec> spec, const Val& z0,
                                 const Val& theta, int64_t N, int64_t S,
                                 SlotInfo result_si, std::optional<Val> t0,
                                 std::optional<Val> ts) {
  // Falling back to the interpreter is correct but ~30x slower, so make
  // it findable rather than silent.
  if (!spec->prog.ok && std::getenv("STANLI_DEBUG_ODE"))
    emit_diagnostic("stanli: ODE right-hand side " + spec->rhs_name +
                    " falls back to the interpreter: " + spec->prog.why);
  if (spec->prog.ok &&
      (spec->solver == OdeSpec::RK45 || spec->solver == OdeSpec::CKRK)) {
    spec->direct_rk =
        make_rhs_adjoint_program(spec->prog, &spec->direct_rk_why);
    spec->direct_rk_enabled =
        spec->direct_rk && !std::getenv("STANLI_NO_ODE_DIRECT_RK");
  }
  if (spec->prog.ok &&
      (spec->solver == OdeSpec::RK45 || spec->solver == OdeSpec::CKRK) &&
      std::getenv("STANLI_DEBUG_ODE")) {
    if (spec->direct_rk)
      emit_diagnostic("stanli: ODE right-hand side " + spec->rhs_name +
                      " is direct-RK eligible" +
                      (spec->direct_rk_enabled ? "" : " (oracle selected)"));
    else
      emit_diagnostic("stanli: ODE right-hand side " + spec->rhs_name +
                      " keeps the RK oracle: " + spec->direct_rk_why);
  }
  Val v = t0 && ts ? emit_value(OP_ODE, {z0, theta, *t0, *ts}, N * S, result_si,
                                {(int)N, (int)S})
                   : emit_value(OP_ODE, {z0, theta}, N * S, result_si,
                                {(int)N, (int)S});
  // The new four-input form uses bit 4 as its marker and includes initial
  // and output-time scalar types in bits 2 and 3. The old two-input form
  // retains its bit-2 compatibility encoding.
  g.ops.back().variant =
      t0 && ts
          ? (uint8_t)(0x10u | (z0.autodiff ? 0x1u : 0u) |
                      (theta.autodiff ? 0x2u : 0u) |
                      (t0->autodiff ? 0x4u : 0u) | (ts->autodiff ? 0x8u : 0u))
          : (uint8_t)(0x4u | (z0.autodiff ? 0x1u : 0u) |
                      (theta.autodiff ? 0x2u : 0u));
  g.ops.back().udata = spec.get();
  g.udata_pool.push_back(std::move(spec));
  return v;
}
SlotInfo Lowering::ode_result_view(const mir::Expr& e, int64_t N, int64_t S) {
  if (e.unsized.depth == 1 && e.unsized.leaf == mir::UnsizedLeaf::Vector)
    return array_view({N, S}, ViewKind::Vector);
  if (e.unsized.depth == 2 && (e.unsized.leaf == mir::UnsizedLeaf::Real ||
                               e.unsized.leaf == mir::UnsizedLeaf::Int))
    return array_view({N, S}, ViewKind::Flat);
  fail("ODE result has unsupported logical type", e.raw);
}
// The modern variadic family: ode_rk45 / ode_bdf / ode_adams / ode_ckrk
// and their _tol forms.
//
//   ode_SOLVER(f, y0, t0, ts, ...args)
//   ode_SOLVER_tol(f, y0, t0, ts, rtol, atol, max_steps, ...args)
//
// Everything after the fixed prefix is passed straight through to the
// right-hand side, in any number and any type. They reduce to the same
// calling convention integrate_ode_* has always used -- autodiff reals
// packed in order, data reals packed in order, integers as compile-time
// constants -- so the kernel and the register machine are unchanged;
// only the packing at this end is new.
std::optional<Lowering::Val> Lowering::lower_ode_variadic(
    const mir::Expr& e, CallArguments& actuals) {
  const auto call = mir::ode_call(e.name);
  if (!call || call->legacy || call->method == mir::OdeMethod::Adjoint)
    return std::nullopt;
  const size_t fixed = call->callback_args_begin;
  if (actuals.size() < fixed) fail(e.name + ": unexpected arity", e.raw);
  auto spec = std::make_shared<OdeSpec>();
  // Argument zero is the callback name retained in OdeSpec, not a lowered
  // value. The remaining fixed arguments are acquired in the historical
  // order below because constant probing can be observable through errors.
  const mir::Expr& rhs_expr = actuals.at(0).expr();
  if (fun_defs.find(rhs_expr.name) == fun_defs.end())
    fail(e.name + ": unknown right-hand side " + rhs_expr.name, e.raw);
  spec->adopt(fun_defs);
  spec->rhs_name = rhs_expr.name;
  spec->solver = ode_solver(call->method);
  spec->stiff = spec->solver == OdeSpec::BDF || spec->solver == OdeSpec::ADAMS;
  stamp_ode_defaults(*spec);
  const bool runtime_times = !e.args[2].data_only || !e.args[3].data_only;
  Val t0, ts;
  if (runtime_times) {
    t0 = actuals.at(2).value();
    ts = actuals.at(3).value();
    if (!is_scalar(t0) || !is_array(ts.si))
      fail(e.name + ": initial time or output times has the wrong type", e.raw);
  } else {
    spec->t0 = actuals.at(2).require_constant_reals("ODE initial time").at(0);
    spec->ts = actuals.at(3).require_constant_reals("ODE output times");
  }
  if (call->with_tolerance) {
    spec->rtol =
        actuals.at(4).require_constant_reals("ODE relative tolerance").at(0);
    spec->atol =
        actuals.at(5).require_constant_reals("ODE absolute tolerance").at(0);
    spec->max_steps =
        (long)actuals.at(6).require_constant_reals("ODE maximum steps").at(0);
  }

  // Classify and pack. Data arguments fold into the spec here and never
  // reach the graph; autodiff ones are concatenated in argument order
  // into the single theta input the op takes, which is the order
  // compile_rhs_args assigns their register sub-ranges in.
  std::vector<RhsArg> rargs;
  std::vector<Val> param_parts;
  for (size_t k = fixed; k < actuals.size(); ++k) {
    LoweredArgument& actual = actuals.at(k);
    const mir::Expr& a = actual.expr();
    RhsArg ra;
    const bool is_int = a.unsized.leaf == mir::UnsizedLeaf::Int;
    if (is_int && a.data_only) {
      ra.is_int = true;
      ra.ints = actual.require_constant_ints("ODE integer argument");
    } else if (a.data_only) {
      // One evaluation, held in a local. Calling const_values(a) twice
      // and taking begin() from one temporary and end() from the other
      // is an invalid range, and it does not fail loudly: it appended
      // hundreds of garbage doubles to x_r and surfaced much later as
      // "ode parameters and data[927] is nan".
      const std::vector<double>& vals =
          actual.require_constant_reals("ODE data argument");
      ra.len = (int)vals.size();
      spec->x_r.insert(spec->x_r.end(), vals.begin(), vals.end());
    } else {
      if (is_int)
        fail(e.name + ": integer argument " + std::to_string(k - fixed + 1) +
                 " is not data",
             e.raw);
      const Val v = actual.value();
      ra.is_param = true;
      ra.len = (int)g.slots[v.slot].len;
      param_parts.push_back(v);
    }
    rargs.push_back(std::move(ra));
  }

  Val z0 = actuals.at(1).value();
  const int64_t S = g.slots[z0.slot].len;
  const int64_t N =
      runtime_times ? g.slots[ts.slot].len : (int64_t)spec->ts.size();
  if (runtime_times) spec->ts.resize((size_t)N);

  // One contiguous theta. A model with a single parameter argument --
  // which is most of them -- gets its slot used directly and pays for
  // no copy at all; more than one chains through CONCAT2, whose
  // backward already splits the adjoint back out.
  Val theta;
  if (param_parts.empty()) {
    theta = constant(0.0);
    // len 1, unread: n_th is 0
  } else {
    theta = param_parts[0];
    int64_t acc = g.slots[theta.slot].len;
    for (size_t k = 1; k < param_parts.size(); ++k) {
      const int64_t add = g.slots[param_parts[k].slot].len;
      theta = emit_value(OP_CONCAT2, {theta, param_parts[k]}, acc + add);
      acc += add;
    }
  }

  spec->args = rargs;
  spec->prog = compile_rhs_args(*spec->rhs(), *spec->funs(), (int)S, rargs);
  if (!spec->prog.ok)
    note_interpreter_fallback("the ODE right-hand side " + spec->rhs_name,
                              spec->prog.why);
  if (runtime_times)
    return emit_ode(std::move(spec), z0, theta, N, S, ode_result_view(e, N, S),
                    t0, ts);
  return emit_ode(std::move(spec), z0, theta, N, S, ode_result_view(e, N, S));
}
// The integrate_ode_* family.
std::optional<Lowering::Val> Lowering::lower_ode_fn(const mir::Expr& e,
                                                    CallArguments& actuals) {
  if (auto v = lower_ode_variadic(e, actuals)) return v;
  const auto call = mir::ode_call(e.name);
  if (call && call->legacy) {
    const OdeSpec::Solver legacy_solver =
        call->method == mir::OdeMethod::Rk45  ? OdeSpec::RK45
        : call->method == mir::OdeMethod::Bdf ? OdeSpec::BDF
                                              : OdeSpec::ADAMS;
    // integrate_ode_*(f, z_init, t0, ts, theta, x_r, x_i[, rtol, atol,
    // max_steps]). Everything but z_init and theta is data, and is
    // captured in the spec the kernel reads through the op payload.
    if (actuals.size() < 7) fail(e.name + ": unexpected arity", e.raw);
    auto spec = std::make_shared<OdeSpec>();
    // The callback name is retained as source metadata; all value-bearing
    // actuals use the lazy wrapper so each is acquired once.
    const mir::Expr& rhs_expr = actuals.at(0).expr();
    auto fit = fun_defs.find(rhs_expr.name);
    if (fit == fun_defs.end())
      fail(e.name + ": unknown right-hand side " + rhs_expr.name, e.raw);
    spec->adopt(fun_defs);
    spec->rhs_name = rhs_expr.name;
    spec->legacy = true;
    spec->solver = legacy_solver;
    spec->stiff =
        spec->solver == OdeSpec::BDF || spec->solver == OdeSpec::ADAMS;
    stamp_ode_defaults(*spec);
    spec->t0 = actuals.at(2).require_constant_reals("ODE initial time").at(0);
    spec->ts = actuals.at(3).require_constant_reals("ODE output times");
    spec->x_r = actuals.at(5).require_constant_reals("ODE real data");
    spec->x_i = actuals.at(6).require_constant_ints("ODE integer data");
    if (actuals.size() >= 10) {
      spec->rtol =
          actuals.at(7).require_constant_reals("ODE relative tolerance").at(0);
      spec->atol =
          actuals.at(8).require_constant_reals("ODE absolute tolerance").at(0);
      spec->max_steps =
          (long)actuals.at(9).require_constant_reals("ODE maximum steps").at(0);
    }
    Val z0 = actuals.at(1).value();
    Val theta = actuals.at(4).value();
    const int64_t S = g.slots[z0.slot].len;
    const int64_t N = (int64_t)spec->ts.size();
    // Compile the right-hand side now that its argument sizes are known.
    // A failure here is not a compile error: the interpreter still runs it.
    spec->args.resize(3);
    spec->args[0].is_param = true;
    spec->args[0].len = (int)g.slots[theta.slot].len;
    spec->args[1].len = (int)spec->x_r.size();
    spec->args[2].is_int = true;
    spec->args[2].ints = spec->x_i;
    spec->prog = compile_rhs(*spec->rhs(), *spec->funs(), (int)S,
                             (int)g.slots[theta.slot].len,
                             (int)spec->x_r.size(), spec->x_i);
    return emit_ode(std::move(spec), z0, theta, N, S, ode_result_view(e, N, S));
  }
  return std::nullopt;
}
}  // namespace lower_detail
}  // namespace stanli
