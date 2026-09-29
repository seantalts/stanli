#include <stanli/higher_order_eval.hpp>

#include <stanli/algebra.hpp>
#include <stanli/container_shape.hpp>
#include <stanli/dae.hpp>
#include <stanli/ode.hpp>
#include <stanli/ode_adjoint.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/optable.hpp>
#include <stanli/quadrature.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace stanli {
namespace {

using Entry = DataMap::Entry;
using Eval = std::function<Entry(const mir::Expr&)>;

double scalar_real(const Entry& value, const std::string& role) {
  if (value.is_int) {
    if (value.i.size() != 1) throw CompileError(role + " must be one real");
    return value.i[0];
  }
  if (value.r.size() != 1) throw CompileError(role + " must be one real");
  return value.r[0];
}

int scalar_int(const Entry& value, const std::string& role) {
  if (!value.is_int || value.i.size() != 1)
    throw CompileError(role + " must be one integer");
  return value.i[0];
}

std::vector<double> promoted_reals(const Entry& value) {
  if (!value.is_int) return value.r;
  return std::vector<double>(value.i.begin(), value.i.end());
}

std::vector<double> storage_order(const mir::Expr& expr, const Entry& value) {
  std::vector<double> values = promoted_reals(value);
  const bool matrix =
      expr.unsized.depth == 0 && expr.unsized.leaf == mir::UnsizedLeaf::Matrix;
  const bool nested_matrix =
      expr.unsized.depth != 0 && expr.unsized.leaf == mir::UnsizedLeaf::Matrix;
  if (matrix || value.dims.size() <= 1) return values;
  const size_t outer_rank =
      value.dims.size() - (nested_matrix ? size_t{2} : size_t{0});
  return graph_container_order(values, value.dims, outer_rank);
}

std::vector<double> real_values(const Entry& value, const std::string&) {
  return promoted_reals(value);
}

std::vector<int> int_values(const Entry& value, const std::string& role) {
  if (!value.is_int) throw CompileError(role + " must be integer-valued");
  return value.i;
}

const mir::FunDef& callback(
    const std::map<std::string, const mir::FunDef*>& funs,
    const mir::Expr& expr, const std::string& family) {
  if (expr.kind != mir::Expr::Var)
    throw CompileError(family + ": callback is not a function name");
  auto it = funs.find(expr.name);
  if (it == funs.end())
    throw CompileError(family + ": unknown callback " + expr.name);
  return *it->second;
}

// Value-only interpretation deliberately binds every real callback argument
// through x_r. No autodiff distinction survives in this backend; RhsArg still
// preserves the positional and logical callback ABI used by the shared kernel.
void pack_data_callback(RetainedCallback& spec,
                        const std::vector<mir::Expr>& args, size_t begin,
                        size_t end, const Eval& eval) {
  for (size_t i = begin; i < end; ++i) {
    Entry value = eval(args[i]);
    RhsArg binding;
    if (args[i].unsized.depth) binding.dims = value.dims;
    if (args[i].unsized.leaf == mir::UnsizedLeaf::Int) {
      binding.is_int = true;
      binding.ints = int_values(value, "integer callback argument");
    } else {
      if (args[i].unsized.depth == 0 &&
          args[i].unsized.leaf == mir::UnsizedLeaf::Matrix) {
        if (value.dims.size() != 2)
          throw CompileError("matrix callback argument " +
                             std::to_string(i - begin + 1) +
                             " has incomplete logical dimensions");
        binding.rows = value.dims[0];
        binding.cols = value.dims[1];
      }
      std::vector<double> values = storage_order(args[i], value);
      if (values.size() > (size_t)std::numeric_limits<int>::max())
        throw CompileError("higher-order callback argument is too large");
      binding.len = (int)values.size();
      spec.x_r.insert(spec.x_r.end(), values.begin(), values.end());
    }
    spec.args.push_back(std::move(binding));
  }
}

Entry invoke(uint16_t opcode, uint8_t variant,
             std::vector<std::vector<double>> inputs, int64_t out_len,
             std::vector<int64_t> dims, std::shared_ptr<void> owner) {
  const Kernel* kernel = find_kernel(opcode);
  if (!kernel || !kernel->forward)
    throw CompileError(std::string(opcode_name(opcode)) +
                       ": runtime kernel is unavailable");
  if (inputs.size() > 6) throw CompileError("too many kernel inputs");

  Op op;
  op.opcode = opcode;
  op.variant = variant;
  op.n_in = (int)inputs.size();
  std::vector<Slot> slots(inputs.size() + 1);
  for (size_t i = 0; i < inputs.size(); ++i) {
    op.in[i] = (int)i;
    slots[i].len = (int64_t)inputs[i].size();
  }
  op.out = (int)inputs.size();
  slots.back().len = out_len;
  const int64_t scratch_len =
      kernel->scratch_size ? kernel->scratch_size(op, slots.data()) : 0;
  std::vector<double> output((size_t)out_len);
  std::vector<double> scratch((size_t)std::max<int64_t>(scratch_len, 1));
  KernelCtx ctx;
  ctx.n_in = op.n_in;
  for (int i = 0; i < op.n_in; ++i)
    ctx.in[i] = Desc{inputs[(size_t)i].data(), slots[(size_t)i].len};
  ctx.out = Desc{output.data(), out_len};
  ctx.variant = variant;
  ctx.scratch = scratch.data();
  ctx.udata = owner.get();
  std::unique_ptr<KernelState> state;
  if (kernel->make_state) {
    state.reset(kernel->make_state(op, slots.data()));
    ctx.state = state.get();
  }
  kernel->forward(ctx);

  Entry result;
  result.r = std::move(output);
  result.dims = std::move(dims);
  return result;
}

Entry solution_invoke(uint16_t opcode, uint8_t variant,
                      std::vector<std::vector<double>> inputs, int64_t times,
                      int64_t states, std::shared_ptr<void> owner) {
  Entry graph = invoke(opcode, variant, std::move(inputs), times * states,
                       {times, states}, std::move(owner));
  // Kernel/graph storage keeps each outer array element contiguous. MIR
  // interpreter storage has the first array dimension varying fastest.
  Entry result;
  result.dims = graph.dims;
  result.r.resize(graph.r.size());
  for (int64_t n = 0; n < times; ++n)
    for (int64_t k = 0; k < states; ++k)
      result.r[(size_t)(n + times * k)] = graph.r[(size_t)(n * states + k)];
  return result;
}

std::shared_ptr<OdeSpec> ode_spec(
    const std::map<std::string, const mir::FunDef*>& funs, const mir::Expr& e,
    const Eval& eval, std::vector<std::vector<double>>& in, int64_t* states,
    int64_t* times) {
  const auto meta = mir::ode_call(e.name);
  if (!meta || meta->method == mir::OdeMethod::Adjoint) return {};
  auto spec = std::make_shared<OdeSpec>();
  spec->adopt(funs);
  spec->rhs_name = callback(funs, e.args.at(0), e.name).name;
  spec->callback_name = spec->rhs_name;
  spec->legacy = meta->legacy;
  switch (meta->method) {
    case mir::OdeMethod::Bdf:
      spec->solver = OdeSpec::BDF;
      break;
    case mir::OdeMethod::Adams:
      spec->solver = OdeSpec::ADAMS;
      break;
    case mir::OdeMethod::Ckrk:
      spec->solver = OdeSpec::CKRK;
      break;
    default:
      spec->solver = OdeSpec::RK45;
      break;
  }
  spec->stiff = spec->solver == OdeSpec::BDF || spec->solver == OdeSpec::ADAMS;
  if (spec->stiff) {
    spec->rtol = 1e-10;
    spec->atol = 1e-10;
    spec->max_steps = 100000000;
  }
  Entry y0 = eval(e.args.at(1));
  Entry t0 = eval(e.args.at(2));
  Entry ts = eval(e.args.at(3));
  in.push_back(real_values(y0, "ODE initial state"));
  *states = (int64_t)in[0].size();
  *times = (int64_t)ts.r.size();
  spec->t0 = scalar_real(t0, "ODE initial time");
  spec->ts = real_values(ts, "ODE output times");

  if (meta->legacy) {
    if (e.args.size() != 7 && e.args.size() != 10)
      throw CompileError(e.name + ": unexpected arity");
    Entry theta = eval(e.args[4]);
    in.push_back(real_values(theta, "ODE parameters"));
    spec->x_r = real_values(eval(e.args[5]), "ODE real data");
    spec->x_i = int_values(eval(e.args[6]), "ODE integer data");
    if (e.args.size() == 10) {
      spec->rtol = scalar_real(eval(e.args[7]), "ODE relative tolerance");
      spec->atol = scalar_real(eval(e.args[8]), "ODE absolute tolerance");
      spec->max_steps = scalar_int(eval(e.args[9]), "ODE maximum steps");
    }
    spec->args.resize(3);
    spec->args[0].is_param = true;
    spec->args[0].len = (int)in[1].size();
    spec->args[1].len = (int)spec->x_r.size();
    spec->args[2].is_int = true;
    spec->args[2].ints = spec->x_i;
    spec->prog =
        compile_rhs(*spec->rhs(), *spec->funs(), (int)*states,
                    (int)in[1].size(), (int)spec->x_r.size(), spec->x_i);
  } else {
    if (meta->with_tolerance) {
      spec->rtol = scalar_real(eval(e.args[4]), "ODE relative tolerance");
      spec->atol = scalar_real(eval(e.args[5]), "ODE absolute tolerance");
      spec->max_steps = scalar_int(eval(e.args[6]), "ODE maximum steps");
    }
    in.push_back({0.0});
    pack_data_callback(*spec, e.args, meta->callback_args_begin, e.args.size(),
                       eval);
    spec->prog =
        compile_rhs_args(*spec->rhs(), *spec->funs(), (int)*states, spec->args);
    in.push_back({spec->t0});
    in.push_back(spec->ts);
  }
  return spec;
}

}  // namespace

bool evaluate_retained_higher_order(
    const std::map<std::string, const mir::FunDef*>& funs, const mir::Expr& e,
    const Eval& eval, Entry* out) {
  const auto family = mir::higher_order_call(e);
  if (!family) return false;

  if (family->family == mir::HigherOrderFamily::Ode) {
    const auto meta = mir::ode_call(e.name);
    if (meta->method == mir::OdeMethod::Adjoint) {
      auto spec = std::make_shared<OdeAdjointSpec>();
      spec->adopt(funs);
      spec->rhs_name = callback(funs, e.args.at(0), e.name).name;
      spec->callback_name = spec->rhs_name;
      std::vector<std::vector<double>> in;
      in.push_back(real_values(eval(e.args[1]), "adjoint ODE initial state"));
      in.push_back({scalar_real(eval(e.args[2]), "adjoint ODE initial time")});
      in.push_back(real_values(eval(e.args[3]), "adjoint ODE output times"));
      spec->relative_tolerance_forward =
          scalar_real(eval(e.args[4]), "forward tolerance");
      spec->absolute_tolerance_forward =
          real_values(eval(e.args[5]), "forward absolute tolerance");
      spec->relative_tolerance_backward =
          scalar_real(eval(e.args[6]), "backward tolerance");
      spec->absolute_tolerance_backward =
          real_values(eval(e.args[7]), "backward absolute tolerance");
      spec->relative_tolerance_quadrature =
          scalar_real(eval(e.args[8]), "quadrature tolerance");
      spec->absolute_tolerance_quadrature =
          scalar_real(eval(e.args[9]), "quadrature absolute tolerance");
      spec->max_num_steps = scalar_int(eval(e.args[10]), "maximum steps");
      spec->num_steps_between_checkpoints =
          scalar_int(eval(e.args[11]), "checkpoint interval");
      spec->interpolation_polynomial =
          scalar_int(eval(e.args[12]), "interpolation polynomial");
      spec->solver_forward = scalar_int(eval(e.args[13]), "forward solver");
      spec->solver_backward = scalar_int(eval(e.args[14]), "backward solver");
      in.push_back({0.0});
      pack_data_callback(*spec, e.args, meta->callback_args_begin,
                         e.args.size(), eval);
      spec->prog = compile_rhs_args(*spec->rhs(), *spec->funs(),
                                    (int)in[0].size(), spec->args);
      const int64_t states = (int64_t)in[0].size();
      const int64_t times = (int64_t)in[2].size();
      *out = solution_invoke(OP_ODE_ADJOINT, 0x10u, std::move(in), times,
                             states, spec);
      return true;
    }
    std::vector<std::vector<double>> in;
    int64_t states = 0, times = 0;
    auto spec = ode_spec(funs, e, eval, in, &states, &times);
    *out = solution_invoke(OP_ODE, meta->legacy ? 0x4u : 0x10u, std::move(in),
                           times, states, spec);
    return true;
  }
  if (family->family == mir::HigherOrderFamily::Integrate1D) {
    const auto meta = mir::quadrature_call(e.name);
    auto spec = std::make_shared<QuadratureSpec>();
    spec->adopt(funs);
    spec->callback_name = callback(funs, e.args.at(0), e.name).name;
    spec->method = meta->method;
    size_t callback_end = e.args.size();
    if (meta->legacy) {
      callback_end = 6;
      if (e.args.size() == 7)
        spec->relative_tolerance =
            scalar_real(eval(e.args[6]), "quadrature tolerance");
    } else if (meta->with_tolerance) {
      spec->relative_tolerance =
          scalar_real(eval(e.args[3]), "quadrature relative tolerance");
      spec->absolute_tolerance =
          scalar_real(eval(e.args[4]), "quadrature absolute tolerance");
      spec->max_steps = scalar_int(eval(e.args[5]), "quadrature maximum steps");
    }
    pack_data_callback(*spec, e.args, meta->callback_args_begin, callback_end,
                       eval);
    spec->parameter_count = 0;
    spec->prog =
        compile_rhs_args(*spec->callback(), *spec->funs(), 1, spec->args);
    std::vector<std::vector<double>> in{
        {scalar_real(eval(e.args[1]), "quadrature lower bound")},
        {scalar_real(eval(e.args[2]), "quadrature upper bound")},
        {0.0}};
    *out = invoke(OP_QUADRATURE, 0, std::move(in), 1, {}, spec);
    return true;
  }
  if (family->family == mir::HigherOrderFamily::Dae) {
    const auto meta = mir::dae_call(e.name);
    auto spec = std::make_shared<DaeSpec>();
    spec->adopt(funs);
    spec->residual_name = callback(funs, e.args.at(0), e.name).name;
    spec->callback_name = spec->residual_name;
    std::vector<std::vector<double>> in;
    in.push_back(real_values(eval(e.args[1]), "DAE initial state"));
    in.push_back(real_values(eval(e.args[2]), "DAE initial derivative"));
    spec->t0 = scalar_real(eval(e.args[3]), "DAE initial time");
    spec->ts = real_values(eval(e.args[4]), "DAE output times");
    if (meta->with_tolerance) {
      spec->rtol = scalar_real(eval(e.args[5]), "DAE relative tolerance");
      spec->atol = scalar_real(eval(e.args[6]), "DAE absolute tolerance");
      spec->max_steps = scalar_int(eval(e.args[7]), "DAE maximum steps");
    }
    in.push_back({0.0});
    pack_data_callback(*spec, e.args, meta->callback_args_begin, e.args.size(),
                       eval);
    spec->prog = compile_dae_args(*spec->residual(), *spec->funs(),
                                  (int)in[0].size(), spec->args);
    const int64_t states = (int64_t)in[0].size();
    const int64_t times = (int64_t)spec->ts.size();
    *out = solution_invoke(OP_DAE, 0x8u, std::move(in), times, states, spec);
    return true;
  }
  if (family->family == mir::HigherOrderFamily::Algebra) {
    const auto meta = mir::algebra_call(e.name);
    auto spec = std::make_shared<AlgebraSpec>();
    spec->adopt(funs);
    spec->system_name = callback(funs, e.args.at(0), e.name).name;
    spec->callback_name = spec->system_name;
    spec->select(*meta);
    std::vector<std::vector<double>> in;
    in.push_back(real_values(eval(e.args[1]), "algebra initial guess"));
    if (meta->legacy) {
      in.push_back(real_values(eval(e.args[2]), "algebra parameters"));
      spec->x_r = real_values(eval(e.args[3]), "algebra real data");
      spec->x_i = int_values(eval(e.args[4]), "algebra integer data");
      if (e.args.size() == 8) {
        spec->relative_tolerance =
            scalar_real(eval(e.args[5]), "algebra relative tolerance");
        spec->function_tolerance =
            scalar_real(eval(e.args[6]), "algebra function tolerance");
        spec->max_num_steps =
            scalar_int(eval(e.args[7]), "algebra maximum steps");
      }
      std::vector<RhsArg> args(3);
      args[0].is_param = true;
      args[0].len = (int)in[1].size();
      args[1].len = (int)spec->x_r.size();
      args[2].is_int = true;
      args[2].ints = spec->x_i;
      spec->prog = compile_rhs_args(with_leading_time(*spec->system()),
                                    *spec->funs(), (int)in[0].size(), args);
    } else {
      if (meta->with_tolerance) {
        spec->relative_tolerance =
            scalar_real(eval(e.args[2]), "algebra relative tolerance");
        spec->function_tolerance =
            scalar_real(eval(e.args[3]), "algebra function tolerance");
        spec->max_num_steps =
            scalar_int(eval(e.args[4]), "algebra maximum steps");
      }
      in.push_back({0.0});
      pack_data_callback(*spec, e.args, meta->callback_args_begin,
                         e.args.size(), eval);
      spec->prog =
          compile_rhs_args(with_leading_time(*spec->system()), *spec->funs(),
                           (int)in[0].size(), spec->args);
    }
    const int64_t size = (int64_t)in[0].size();
    *out = invoke(OP_ALGEBRA_SOLVER, 0, std::move(in), size, {size}, spec);
    return true;
  }
  return false;
}

}  // namespace stanli
