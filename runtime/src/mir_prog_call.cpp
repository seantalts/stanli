// ProgramCompiler: calls of builtins, densities, transforms and user functions.
#include <stanli/mir_prog.hpp>

namespace stanli {

bool ProgramCompiler::autodiff(const mir::Expr& e) const {
  if (in_write_array) return false;
  const bool locals = !call_types || call_types->locals;
  return mir::instantiated_autodiff(e, locals, [&](const mir::Expr& v) {
    if (!call_types) return true;
    const auto formal = call_types->formals.find(v.name);
    return formal != call_types->formals.end() ? formal->second : locals;
  });
}

BuiltinArgumentShape ProgramCompiler::builtin_argument_shape(
    const mir::Expr& source, const Range& value) {
  const BuiltinArgumentKind kind = source.unsized.leaf == mir::UnsizedLeaf::Int
                                       ? BuiltinArgumentKind::Integer
                                       : BuiltinArgumentKind::Real;
  return make_view_function_shape(kind, value.kind, value.leaf, value.dims,
                                  value.len, value.rows, value.cols);
}

Range ProgramCompiler::shaped(Range out, const BuiltinArgumentShape& shape) {
  out.kind = function_view_kind(shape.container);
  out.rows = out.cols = 0;
  out.dims.clear();
  out.leaf = ViewKind::Flat;
  if (out.kind == ViewKind::Matrix) {
    out.rows = shape.dimensions[0];
    out.cols = shape.dimensions[1];
  } else if (out.kind == ViewKind::Array) {
    out.dims = shape.dimensions;
    out.leaf = function_view_kind(shape.array_leaf);
  }
  return out;
}

BuiltinLayout ProgramCompiler::resolved_builtin_layout(
    const mir::Expr& e, const BuiltinSpec& spec,
    const std::vector<Range>& values) {
  std::vector<BuiltinArgumentShape> shapes;
  shapes.reserve(values.size());
  try {
    for (size_t k = 0; k < values.size(); ++k)
      shapes.push_back(builtin_argument_shape(e.args[k], values[k]));
    return builtin_layout(spec, shapes, e.name,
                          !in_write_array && mir::any_argument_active(e));
  } catch (const std::invalid_argument& error) {
    throw Bail{builtin_error_message(e.name, error)};
  }
}

InlineArg ProgramCompiler::inline_argument(const mir::Expr& e) {
  InlineArg arg;
  long v;
  if (mir::is_unsized(e, mir::UnsizedLeaf::Int) && try_cint(e, &v)) {
    arg.is_const_int = true;
    arg.ints = {v};
  } else if (e.unsized.depth != 0 && e.unsized.leaf == mir::UnsizedLeaf::Int &&
             try_cints(e, &arg.ints)) {
    arg.is_const_int = true;
    Range view;
    if (!static_view(e, &view) || view.kind != ViewKind::Array)
      bail("integer function argument has no static array view");
    arg.int_dims =
        view.dims.empty() ? std::vector<int64_t>{view.len} : view.dims;
  } else {
    if (mir::is_unsized(e, mir::UnsizedLeaf::Real))
      arg.is_const_real = try_creal(e, &arg.const_real);
    arg.real = expr(e);
    arg.autodiff = autodiff(e);
  }
  return arg;
}

std::vector<InlineArg> ProgramCompiler::inline_arguments(
    const std::vector<mir::Expr>& exprs, size_t begin) {
  std::vector<InlineArg> args;
  args.reserve(exprs.size() - begin);
  for (size_t i = begin; i < exprs.size(); ++i)
    args.push_back(inline_argument(exprs[i]));
  return args;
}

void ProgramCompiler::require_positive(const Range& value,
                                       const std::string& name) {
  if (!is_scalar(value)) bail(name + " is not a scalar");
  const int ok = alloc(1);
  emit(Program::GE, ok, value.reg, konst(1.0));
  const int reject = emit(Program::JZ, 0, ok);
  const int done = emit(Program::JMP, 0);
  p.code[(size_t)reject].dst = (int)p.code.size();
  Program::Message message;
  message.spec.chunks = {name + " must be positive"};
  p.messages.push_back(std::move(message));
  emit(Program::REJECT, 0, (int)p.messages.size() - 1);
  p.code[(size_t)done].dst = (int)p.code.size();
}

Range ProgramCompiler::reduce_sum_call(const mir::Expr& e) {
  if (e.args.size() < 3)
    bail(
        "reduce_sum: expected a partial-sum function, a sliced argument, "
        "and a grainsize");
  if (e.args[0].kind != mir::Expr::Var)
    bail("reduce_sum: the partial-sum argument is not a function name");
  if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Real)
    bail("reduce_sum: result is not a real");

  const Range slice = expr(e.args[1]);
  if (slice.kind != ViewKind::Array || slice.dims.empty())
    bail("reduce_sum: the sliced argument is not an array");
  const Range grainsize = expr(e.args[2]);
  if (e.args[2].unsized.depth != 0 ||
      e.args[2].unsized.leaf != mir::UnsizedLeaf::Int || !is_scalar(grainsize))
    bail("reduce_sum: grainsize is not an integer scalar");

  // Evaluate shared arguments before the empty-slice return, as C++ does.
  std::vector<InlineArg> shared = inline_arguments(e.args, 3);
  require_positive(grainsize, "reduce_sum grainsize");
  const int64_t n = slice.dims.front();
  if (n == 0) return {konst(0.0), 1};
  if (n > std::numeric_limits<int32_t>::max())
    bail("reduce_sum: slice bound exceeds the Stan integer range");

  const bool propto = e.args[0].fn_propto;
  const std::string& base = e.args[0].name;
  const std::vector<mir::UnsizedView> views = mir::reduce_sum_partial_views(e);
  const mir::FunDef* f = mir::find_function(funs, e.args[0]);
  if (f == nullptr) bail("reduce_sum: unknown partial-sum function " + base);
  if (f->arg_names.size() != views.size())
    bail("reduce_sum: partial-sum arity does not match the call");

  std::vector<InlineArg> args;
  args.reserve(views.size());
  InlineArg sliced;
  sliced.real = slice;
  sliced.autodiff = autodiff(e.args[1]);
  args.push_back(std::move(sliced));
  InlineArg start;
  start.is_const_int = true;
  start.ints = {1};
  args.push_back(std::move(start));
  InlineArg end;
  end.is_const_int = true;
  end.ints = {(long)n};
  args.push_back(std::move(end));
  for (InlineArg& arg : shared) args.push_back(std::move(arg));
  // As in CmdStan, an `_lupdf` functor inherits the caller's normalization
  // and an `_lpdf` one forces the normalized density.
  return inline_call(*f, args, false, propto_ctx && propto);
}

Range ProgramCompiler::map_rect_call(const mir::Expr& e) {
  if (e.args.size() != 5)
    bail(
        "map_rect: expected function, shared parameters, job parameters, "
        "real data, and integer data");
  if (e.args[0].kind != mir::Expr::Var)
    bail("map_rect: callback argument is not a function name");
  if (e.unsized.depth != 0 || e.unsized.leaf != mir::UnsizedLeaf::Vector)
    bail("map_rect: result is not a vector");

  const Range shared = expr(e.args[1]);
  const Range jobs = expr(e.args[2]);
  const Range real_data = expr(e.args[3]);
  if (shared.kind != ViewKind::Vector)
    bail("map_rect: shared parameters are not a vector");
  if (jobs.kind != ViewKind::Array || jobs.leaf != ViewKind::Vector ||
      jobs.dims.size() != 2)
    bail("map_rect: job parameters are not an array of vectors");
  if (real_data.kind != ViewKind::Array || real_data.leaf != ViewKind::Flat ||
      real_data.dims.size() != 2)
    bail("map_rect: real data are not a two-dimensional array");

  std::vector<long> ints;
  if (!try_cints(e.args[4], &ints))
    bail("map_rect: integer data are not known at compile time");
  Range int_view;
  if (!static_view(e.args[4], &int_view) || int_view.kind != ViewKind::Array ||
      e.args[4].unsized.depth != 2 ||
      e.args[4].unsized.leaf != mir::UnsizedLeaf::Int ||
      int_view.dims.empty() || int_view.dims.size() > 2)
    bail("map_rect: integer data are not a two-dimensional array");

  const int64_t n = jobs.dims[0];
  if (n != real_data.dims[0] || n != int_view.dims[0])
    bail("map_rect: job parameters and job data sizes do not match");
  const int64_t job_width = jobs.dims[1];
  const int64_t real_width = real_data.dims[1];
  // DataMap omits a trailing singleton dimension from an integer array's
  // stored shape. The MIR type retains its rank, and the value count then
  // recovers that one-element inner row unambiguously.
  const int64_t int_width = int_view.dims.size() == 2
                                ? int_view.dims[1]
                                : (n == 0 ? 0 : (int64_t)ints.size() / n);
  if (n < 0 || job_width < 0 || real_width < 0 || int_width < 0 ||
      n > kMaxRegs || (n && job_width > kMaxRegs / n) ||
      (n && real_width > kMaxRegs / n) || (n && int_width > kMaxRegs / n))
    bail("map_rect: input shape is invalid or too large");
  if ((int64_t)ints.size() != n * int_width)
    bail("map_rect: integer data storage and shape disagree");

  const std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector},
                                            {0, mir::UnsizedLeaf::Vector},
                                            {1, mir::UnsizedLeaf::Real},
                                            {1, mir::UnsizedLeaf::Int}};
  const mir::FunDef* f = mir::find_function(funs, e.args[0]);
  if (f == nullptr)
    bail("map_rect: unknown callback function " + e.args[0].name);
  if (f->arg_names.size() != views.size())
    bail("map_rect: callback arity does not match the call");

  std::vector<Range> results;
  int total = 0;
  results.reserve((size_t)n);
  for (int64_t job = 0; job < n; ++job) {
    std::vector<InlineArg> args(4);
    args[0].real = shared;
    args[1].real = Range{jobs.reg + (int)(job * job_width), (int)job_width};
    args[1].real.kind = ViewKind::Vector;
    args[2].real =
        Range{real_data.reg + (int)(job * real_width), (int)real_width};
    args[2].real.kind = ViewKind::Array;
    args[2].real.dims = {real_width};
    args[2].real.leaf = ViewKind::Flat;
    args[2].autodiff = false;
    args[3].is_const_int = true;
    args[3].int_dims = {int_width};
    args[3].ints.reserve((size_t)int_width);
    // DataMap's flat integer storage has the first array dimension varying
    // fastest. A map_rect job fixes that dimension and ranges over the
    // second, so its row is strided rather than contiguous.
    for (int64_t k = 0; k < int_width; ++k)
      args[3].ints.push_back(ints[(size_t)(job + k * n)]);

    Range result = inline_call(*f, args);
    if (result.kind != ViewKind::Vector)
      bail("map_rect: callback result is not a vector");
    if (result.len > kMaxRegs - total)
      bail("map_rect: result needs too many registers");
    total += result.len;
    results.push_back(result);
  }

  const int out_reg = alloc(total);
  int at = 0;
  for (const Range& result : results)
    for (int k = 0; k < result.len; ++k)
      emit(Program::MOV, out_reg + at++, result.reg + k);
  Range out{out_reg, total};
  out.kind = ViewKind::Vector;
  return out;
}

Range ProgramCompiler::matrix_op_call(const mir::Expr& e,
                                      const BuiltinSpec* matrix,
                                      BuiltinMatrixMap* map_out) {
  std::vector<Range> args;
  args.reserve(matrix->arity);
  for (const mir::Expr& argument : e.args) args.push_back(expr(argument));
  BuiltinMatrixMap map;
  try {
    std::vector<BuiltinArgumentShape> shapes;
    shapes.reserve(args.size());
    for (size_t k = 0; k < args.size(); ++k)
      shapes.push_back(builtin_argument_shape(e.args[k], args[k]));
    map = builtin_matrix_map(*matrix, shapes);
  } catch (const std::invalid_argument& error) {
    bail(e.name + ": " + std::string(error.what()));
  }
  bool active = false;
  for (const mir::Expr& argument : e.args)
    if (!argument.data_only && !in_write_array) active = true;
  Range out{0, (int)map.result.storage_size};
  out = shaped(std::move(out), map.result);
  std::vector<int> idata;
  idata.reserve(map.idata.size());
  for (const int64_t value : map.idata) idata.push_back((int)value);
  if (map_out) *map_out = map;
  return kernel_call(
      matrix->opcode, args, out,
      (uint8_t)(map.variant | (active ? map.active_variant : 0u)),
      matrix->activity_mask, std::move(idata), {}, e.name);
}

Range ProgramCompiler::kernel_call(uint16_t opcode,
                                   const std::vector<Range>& args, Range out,
                                   uint8_t variant, uint8_t input_adjoint_mask,
                                   std::vector<int> idata,
                                   std::shared_ptr<void> udata,
                                   const std::string& name) {
  if (args.size() > 6) bail(name + ": too many kernel arguments");
  out.reg = alloc(out.len);
  Program::Call call;
  call.opcode = opcode;
  call.variant = variant;
  call.input_adjoint_mask = input_adjoint_mask;
  call.n_in = (int8_t)args.size();
  for (size_t k = 0; k < args.size(); ++k) {
    call.in[k] = args[k].reg;
    call.in_len[k] = args[k].len;
  }
  call.out = out.reg;
  call.out_len = out.len;
  call.idata = std::move(idata);
  call.udata_owner = std::move(udata);

  const Kernel* kernel = find_kernel(opcode);
  if (kernel == nullptr) bail(name + ": graph kernel is unavailable");
  const int64_t scratch = kernel_call_scratch(
      kernel->scratch_size, opcode, variant, call.n_in, call.in_len, out.len,
      call.idata.data(), (int64_t)call.idata.size(), call.udata_owner.get());
  if (scratch < 0 || scratch > kMaxRegs)
    bail(name + ": kernel needs excessive scratch storage");
  call.scratch_len = (int32_t)scratch;
  call.scratch = scratch ? alloc((int)scratch) : 0;
  if (!bind_call(call)) bail(name + ": graph kernel is unavailable");
  p.calls.push_back(std::move(call));
  p.code.push_back(Program::Instr{Program::CALL, 0, (int)p.calls.size() - 1});
  return out;
}

bool ProgramCompiler::rng_call_name(const std::string& name) {
  return scalar_rng_family(name) != nullptr || name == "categorical_rng" ||
         name == "categorical_logit_rng" || name == "poisson_binomial_rng" ||
         name == "multi_normal_cholesky_rng" || name == "multi_normal_rng" ||
         name == "dirichlet_rng";
}

Range ProgramCompiler::rng_call(const mir::Expr& e) {
  if (!in_write_array)
    bail(e.name + " is supported only in generated quantities");
  std::vector<Range> args;
  args.reserve(e.args.size());
  for (const mir::Expr& a : e.args) args.push_back(expr(a));

  uint8_t variant = 0;
  int out_len = 1;
  ViewKind out_kind = ViewKind::Flat;
  std::vector<int> idata;
  if (const ScalarRng* family = scalar_rng_family(e.name)) {
    // Stan ints fit exactly in double registers. Draws remain effectful
    // runtime values: cint cannot fold them, shape demands still refuse,
    // and dynamic indexing uses the existing checked register operations.
    const auto leaf = scalar_rng_is_int(*family) ? mir::UnsizedLeaf::Int
                                                 : mir::UnsizedLeaf::Real;
    const bool promoted_int = scalar_rng_is_int(*family) && e.promoted &&
                              e.unsized.leaf == mir::UnsizedLeaf::Real;
    if (e.unsized.depth > 1 || (e.unsized.leaf != leaf && !promoted_int))
      bail(e.name + ": result type does not match scalar RNG family");
    if (args.size() != scalar_rng_arity(*family))
      bail(e.name + ": wrong number of arguments");
    if ((*family == ScalarRng::Binomial ||
         *family == ScalarRng::BetaBinomial) &&
        e.args[0].unsized.leaf != mir::UnsizedLeaf::Int)
      bail(e.name + ": first argument must be int");
    int container_mask = 0;
    for (size_t k = 0; k < args.size(); ++k) {
      const auto& type = e.args[k].unsized;
      const bool scalar =
          type.depth == 0 && (type.leaf == mir::UnsizedLeaf::Real ||
                              type.leaf == mir::UnsizedLeaf::Int);
      const bool array =
          type.depth == 1 && (type.leaf == mir::UnsizedLeaf::Real ||
                              type.leaf == mir::UnsizedLeaf::Int);
      if (scalar) {
        if (!is_scalar(args[k])) bail(e.name + ": expected scalar storage");
      } else {
        if (!array &&
            !(type.depth == 0 && (type.leaf == mir::UnsizedLeaf::Vector ||
                                  type.leaf == mir::UnsizedLeaf::RowVector)))
          bail(e.name + ": expected scalar or one-dimensional arguments");
        if (container_mask != 0 && args[k].len != out_len)
          bail(e.name + ": argument lengths disagree");
        container_mask |= 1 << k;
        out_len = args[k].len;
      }
    }
    if ((e.unsized.depth == 1) != (container_mask != 0))
      bail(e.name + ": result shape does not match RNG arguments");
    if (container_mask != 0) {
      idata.push_back(container_mask);
      out_kind = ViewKind::Array;
    }
    variant = static_cast<uint8_t>(*family);
  } else if (e.name == "categorical_rng" || e.name == "categorical_logit_rng" ||
             e.name == "poisson_binomial_rng") {
    const bool promoted_int =
        e.promoted && e.unsized.leaf == mir::UnsizedLeaf::Real;
    if (e.unsized.depth != 0 ||
        (e.unsized.leaf != mir::UnsizedLeaf::Int && !promoted_int) ||
        args.size() != 1 || args[0].kind != ViewKind::Vector)
      bail(e.name + ": expected a vector argument and scalar integer result");
    variant = e.name == "categorical_logit_rng"  ? kCategoricalLogitRngVariant
              : e.name == "poisson_binomial_rng" ? kPoissonBinomialRngVariant
                                                 : kCategoricalRngVariant;
  } else if (e.name == "dirichlet_rng") {
    if (args.size() != 1 || args[0].kind != ViewKind::Vector ||
        args[0].len <= 0)
      bail("dirichlet_rng: expected one concentration vector");
    variant = kDirichletRngVariant;
    out_len = args[0].len;
    out_kind = ViewKind::Vector;
  } else {
    if (args.size() != 2 || args[0].kind != ViewKind::Vector ||
        args[1].kind != ViewKind::Matrix)
      bail(
          "multi_normal_rng: expected a location vector and a covariance "
          "matrix");
    if (args[1].rows != args[0].len || args[1].cols != args[0].len)
      bail("multi_normal_rng: covariance shape must match the location");
    variant = e.name == "multi_normal_cholesky_rng"
                  ? kMultiNormalCholeskyRngVariant
                  : kMultiNormalRngVariant;
    out_len = args[0].len;
    out_kind = ViewKind::Vector;
    idata.push_back(out_len);
  }

  Range out{0, out_len};
  out.kind = out_kind;
  if (out_kind == ViewKind::Array) out.dims = {out_len};
  return kernel_call(OP_RNG, args, out, variant, 0, std::move(idata), {},
                     e.name);
}

Range ProgramCompiler::builtin_kernel_call(const mir::Expr& e,
                                           const BuiltinSpec& spec) {
  const size_t arity = spec.arity;
  if (e.args.size() != arity) bail(e.name + ": wrong number of arguments");
  std::vector<Range> args;
  args.reserve(arity);
  for (const mir::Expr& arg : e.args) args.push_back(expr(arg));
  const BuiltinLayout layout = resolved_builtin_layout(e, spec, args);
  Range out = args[layout.result_argument];
  if (spec.shape == BuiltinShapePolicy::Ordering) {
    out.kind = ViewKind::Flat;
    out.rows = out.cols = 0;
    out.dims.clear();
    out.leaf = ViewKind::Flat;
    if (spec.ordering == BuiltinOrdering::Rank) {
      out.len = 1;
    } else {
      out.kind = ViewKind::Array;
      out.dims = {out.len};
    }
  }
  out = typed(out, e.unsized);
  std::vector<int> idata;
  if (layout.groups >= 0)
    idata = {static_cast<int>(layout.groups),
             static_cast<int>(layout.group_width), 0};
  if (layout.integer_matrix_rows != 0)
    idata = {(int)layout.integer_matrix_rows, (int)layout.integer_matrix_cols};
  uint8_t activity = spec.activity_mask;
  for (size_t k = 0; k < arity; ++k) {
    // Runtime integers and data-only values still execute normally, but
    // have no derivative. A promotion alone is not proof of inactivity.
    if (e.args[k].data_only || int_operand(e.args[k]))
      activity &= (uint8_t)~(1u << k);
  }
  const uint8_t variant =
      spec.opcode == OP_ORDERING ? static_cast<uint8_t>(spec.ordering) : 0;
  if (spec.opcode == OP_ORDERING) activity = 0;
  return kernel_call(spec.opcode, args, out, variant, activity,
                     std::move(idata), {}, e.name);
}

Program::Code ProgramCompiler::integer_instruction(Program::Code code) {
  switch (code) {
    case Program::ADD:
      return Program::IADD;
    case Program::SUB:
      return Program::ISUB;
    case Program::MUL:
      return Program::IMUL;
    case Program::DIV:
      return Program::IDIV;
    case Program::NEG:
      return Program::INEG;
    case Program::FABS:
      return Program::IABS;
    default:
      return code;
  }
}

Program::Code ProgramCompiler::native_math_code(uint16_t opcode) {
  switch (opcode) {
    case OP_ERFC:
      return Program::ERFC;
    case OP_LOG1P:
      return Program::LOG1P;
    case OP_LOG1M_EXP:
      return Program::LOG1M_EXP;
    default:
      return Program::INV_SQUARE;
  }
}

std::optional<Program::Code> ProgramCompiler::native_builtin_code(
    uint16_t opcode) {
  switch (opcode) {
    case OP_ERFC:
    case OP_LOG1P:
    case OP_LOG1M_EXP:
    case OP_INV_SQUARE:
      if (debug_env("STANLI_NO_NATIVE_MATH")) return std::nullopt;
      return native_math_code(opcode);
    case OP_ADD:
      return Program::ADD;
    case OP_SUB:
      return Program::SUB;
    case OP_MUL:
      return Program::MUL;
    case OP_DIV:
      return Program::DIV;
    case OP_POW:
      return Program::POW;
    case OP_FMAX:
      return Program::FMAX;
    case OP_FMIN:
      return Program::FMIN;
    case OP_LSE2:
      return Program::LSE2;
    case OP_LOG_DIFF_EXP:
      return Program::LOG_DIFF_EXP;
    case OP_LMULTIPLY:
      return Program::LMULTIPLY;
    case OP_NEG:
      return Program::NEG;
    case OP_EXPV:
      return Program::EXP;
    case OP_LOGV:
      return Program::LOG;
    case OP_SQRT:
      return Program::SQRT;
    case OP_SQUARE:
      return Program::SQUARE;
    case OP_INV:
      return Program::INV;
    case OP_ABS:
      return Program::FABS;
    case OP_INV_LOGIT:
      return Program::INV_LOGIT;
    case OP_LOG1P_EXP:
      return Program::LOG1P_EXP;
    default:
      return std::nullopt;
  }
}

Range ProgramCompiler::native_builtin_call(const mir::Expr& e,
                                           const BuiltinSpec& spec,
                                           Program::Code native_code) {
  std::vector<Range> args;
  args.reserve(spec.arity);
  for (const mir::Expr& argument : e.args) args.push_back(expr(argument));
  if (args.empty() || args.size() > 2)
    bail(e.name + ": unsupported native builtin arity");
  const BuiltinLayout layout = resolved_builtin_layout(e, spec, args);
  Range out = args[layout.result_argument];
  const int n = (int)layout.lanes;

  Program::Code code = native_code;
  if (spec.result == FunctionArgumentKind::Integer)
    code = integer_instruction(code);
  // fmax/fmin ties and NaN adjoints depend on which operands are data
  // (adjoint.cpp, program_extremum), the same way pow's zero-base law
  // depends on the exponent's.
  const int law =
      code == Program::POW
          ? mir::pow_zero_base_law(e.args[0], e.args[1], !e.args[1].data_only)
      : code == Program::FMAX || code == Program::FMIN
          ? (e.args[0].data_only ? 0 : 0x1) | (e.args[1].data_only ? 0 : 0x2)
          : 0;
  const int result = alloc(n);
  for (int i = 0; i < n; ++i) {
    const int a = args[0].reg + (is_scalar(args[0]) ? 0 : i);
    const int b =
        args.size() == 2 ? args[1].reg + (is_scalar(args[1]) ? 0 : i) : 0;
    emit(code, result + i, a, b, 0, law);
  }
  out.reg = result;
  out.len = n;
  return typed(out, e.unsized);
}

Range ProgramCompiler::density_call(const mir::Expr& e,
                                    const DensitySpec& spec) {
  if (spec.opcode == OP_MULTINOMIAL_LPMF)
    if (const mir::Expr* logits = mir::multinomial_softmax_logits(e)) {
      mir::Expr call = e;
      call.args[1] = *logits;
      return density_call(call, multinomial_softmax_density());
    }
  if ((int)e.args.size() != spec.arity)
    bail(e.name + ": wrong number of arguments");

  const auto integer_values = [&](const mir::Expr& arg) {
    const std::vector<long> source = mir::is_unsized(arg, mir::UnsizedLeaf::Int)
                                         ? std::vector<long>{cint(arg)}
                                         : cints(arg);
    std::vector<int> result;
    result.reserve(source.size());
    for (long value : source) {
      if (value < std::numeric_limits<int>::min() ||
          value > std::numeric_limits<int>::max())
        bail(e.name + ": integer argument is out of range");
      result.push_back((int)value);
    }
    return result;
  };
  std::vector<Range> args;
  args.reserve(e.args.size() - (size_t)spec.integer_args);
  std::vector<DensityCallArgument> plan_arguments;
  plan_arguments.reserve(e.args.size());
  try {
    for (size_t k = 0; k < e.args.size(); ++k) {
      const mir::Expr& source = e.args[k];
      DensityCallArgument argument;
      if (spec.evaluation == DensityEvaluationPolicy::AllInteger ||
          k < static_cast<size_t>(spec.integer_args)) {
        argument = integer_density_argument(integer_values(source),
                                            source.unsized.depth == 0,
                                            source.data_only);
      } else {
        argument.scalar = source.unsized.depth == 0;
        argument.data_only = source.data_only;
        argument.active = autodiff(source);
        args.push_back(expr(source));
        argument.shape = builtin_argument_shape(source, args.back());
      }
      plan_arguments.push_back(std::move(argument));
    }
    const DensityCallPlan plan =
        density_call_plan(spec, plan_arguments, e.fn_propto && propto_ctx);
    if (plan.empty_result) return {konst(0.0), 1};
    if (plan.data_term) args.push_back(Range{konst(*plan.data_term), 1});
    Range out{0, 1};
    return kernel_call(spec.opcode, args, out, plan.variant, plan.activity_mask,
                       plan.idata, {}, e.name);
  } catch (const DensitySizeError& error) {
    // Stan rejects the call when it runs, so the program rejects there too.
    Program::Message message;
    message.spec.chunks = {e.name + ": " + error.what()};
    message.spec.error = MessageError::InvalidArgument;
    p.messages.push_back(std::move(message));
    emit(Program::REJECT, 0, (int)p.messages.size() - 1);
    return {konst(0.0), 1};
  } catch (const std::exception& error) {
    bail(builtin_error_message(e.name, error));
  }

  return {};
}

Range ProgramCompiler::matrix_gram(const Range& m, bool transpose_first) {
  if (m.kind != ViewKind::Matrix)
    bail(std::string(transpose_first ? "crossprod" : "tcrossprod") +
         " requires a matrix");
  const int64_t outer = transpose_first ? m.cols : m.rows;
  const int64_t inner = transpose_first ? m.rows : m.cols;
  if (outer != 0 && outer > kMaxRegs / outer)
    bail("matrix Gram product needs too many registers");
  const int r = alloc((int)(outer * outer));
  const auto at = [&](int64_t row, int64_t col) {
    return m.reg + (int)(col * m.rows + row);
  };
  for (int64_t j = 0; j < outer; ++j)
    for (int64_t i = 0; i < outer; ++i) {
      const int dst = r + (int)(j * outer + i);
      if (inner == 0) {
        const double zero = 0.0;
        emit_const(dst, &zero, 1);
        continue;
      }
      const auto lhs = [&](int64_t k) {
        return transpose_first ? at(k, i) : at(i, k);
      };
      const auto rhs = [&](int64_t k) {
        return transpose_first ? at(k, j) : at(j, k);
      };
      emit(Program::MUL, dst, lhs(0), rhs(0));
      for (int64_t k = 1; k < inner; ++k) {
        const int term = alloc(1);
        emit(Program::MUL, term, lhs(k), rhs(k));
        emit(Program::ADD, dst, dst, term);
      }
    }
  Range out{r, (int)(outer * outer)};
  out.kind = ViewKind::Matrix;
  out.rows = out.cols = outer;
  return out;
}

Range ProgramCompiler::inverse_transform_call(const mir::Expr& e) {
  CallableTransformSpec spec;
  callable_transform(e.name, &spec);
  Program::Transform tr;
  tr.kind = spec.kind;
  tr.direction = spec.direction;
  tr.n_in = 1;
  const Range arg = expr(e.args[0]);
  tr.in[0] = arg.reg;
  tr.in_len[0] = arg.len;
  ViewKind leaf = arg.kind;
  std::vector<int64_t> dims;
  if (leaf == ViewKind::Array) {
    dims = arg.dims;
    leaf = arg.leaf;
  } else if (leaf == ViewKind::Matrix) {
    dims = {arg.rows, arg.cols};
  } else if (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) {
    dims = {arg.len};
  }
  if (leaf != ViewKind::Vector && leaf != ViewKind::Matrix)
    bail(e.name + ": expected a vector or matrix");
  const size_t outer_rank = dims.size() - leaf_rank(leaf);
  int64_t batch = 1;
  for (size_t i = 0; i < outer_rank; ++i) batch *= dims[i];
  tr.in_rows = (int32_t)dims[outer_rank];
  tr.in_cols = leaf == ViewKind::Matrix ? (int32_t)dims.back() : 0;
  int64_t rows = 0, cols = 0;
  bool out_matrix = false;
  if (!free_leaf_shape(spec.kind, tr.in_rows, tr.in_cols, &rows, &cols,
                       &out_matrix) ||
      rows < 0 || cols < 0)
    bail(e.name + ": invalid input container");
  const int64_t inner_free = out_matrix ? rows * cols : rows;
  if (batch > kMaxRegs || (batch && inner_free > kMaxRegs / batch))
    bail(e.name + ": result needs too many registers");
  tr.batch = (int32_t)batch;
  tr.inner_raw = (int32_t)(tr.in_cols ? tr.in_rows * tr.in_cols : tr.in_rows);
  tr.out_rows = (int32_t)rows;
  tr.out_cols = (int32_t)cols;
  tr.out_len = (int32_t)(batch * inner_free);
  Range out = arg;
  out.len = tr.out_len;
  out.kind = outer_rank ? ViewKind::Array
                        : (out_matrix ? ViewKind::Matrix : ViewKind::Vector);
  out.rows = out_matrix && !outer_rank ? rows : 0;
  out.cols = out_matrix && !outer_rank ? cols : 0;
  if (outer_rank) {
    out.dims.assign(dims.begin(), dims.begin() + outer_rank);
    out.dims.push_back(rows);
    if (out_matrix) out.dims.push_back(cols);
    out.leaf = out_matrix ? ViewKind::Matrix : ViewKind::Vector;
  }
  tr.out = alloc(tr.out_len);
  tr.jac = alloc(1);
  out.reg = tr.out;
  p.transforms.push_back(tr);
  p.code.push_back(
      Program::Instr{Program::TRANSFORM, 0, (int)p.transforms.size() - 1});
  return out;
}

Range ProgramCompiler::transform_call(const mir::Expr& e,
                                      const CallableTransformSpec& spec) {
  if (e.args.size() != spec.arity) bail(e.name + ": wrong number of arguments");
  if (spec.structured && spec.direction == TransformDirection::Unconstrain)
    return inverse_transform_call(e);

  Program::Transform tr;
  tr.kind = spec.kind;
  tr.direction = spec.direction;
  tr.n_in = spec.structured ? 1 : (int8_t)spec.arity;
  std::vector<Range> args;
  args.reserve((size_t)tr.n_in);
  for (int k = 0; k < tr.n_in; ++k) {
    args.push_back(expr(e.args[(size_t)k]));
    tr.in[k] = args.back().reg;
    tr.in_len[k] = args.back().len;
  }
  Range out = args[0];
  if (!spec.structured) {
    for (int k = 1; k < tr.n_in; ++k)
      if (args[k].len != 1 && args[k].len != args[0].len)
        bail(e.name + ": bound is neither scalar nor the input size");
    tr.out_len = args[0].len;
    tr.inner_raw = args[0].len;
    tr.eigen = args[0].kind == ViewKind::Vector      ? 1
               : args[0].kind == ViewKind::RowVector ? 2
               : args[0].kind == ViewKind::Matrix    ? 3
                                                     : 0;
    tr.rows = static_cast<int32_t>(args[0].rows);
  } else {
    ViewKind leaf = args[0].kind;
    std::vector<int64_t> dims;
    if (leaf == ViewKind::Array) {
      dims = args[0].dims;
      leaf = args[0].leaf;
    } else if (leaf == ViewKind::Matrix) {
      dims = {args[0].rows, args[0].cols};
    } else if (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) {
      dims = {args[0].len};
    }
    const size_t rank = leaf_rank(leaf);
    if (rank == 0 || dims.size() < rank)
      bail(e.name + ": invalid input container");
    const size_t outer_rank = dims.size() - rank;
    int64_t batch = 1;
    for (size_t i = 0; i < outer_rank; ++i) batch *= dims[i];
    int64_t raw_rows = leaf == ViewKind::Matrix ? dims[dims.size() - 2] : 0;
    int64_t raw_cols = leaf == ViewKind::Matrix ? dims.back() : 0;
    int64_t rows = 0, cols = 0;
    ViewKind out_leaf = leaf;
    switch (spec.kind) {
      case CallableTransformKind::Ordered:
      case CallableTransformKind::PositiveOrdered:
        if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
        rows = dims.back();
        break;
      case CallableTransformKind::Simplex:
        if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
        rows = dims.back() + 1;
        break;
      case CallableTransformKind::UnitVector:
        if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
        rows = dims.back();
        break;
      case CallableTransformKind::SumToZero:
        if (leaf == ViewKind::Vector) {
          rows = dims.back() + 1;
        } else if (leaf == ViewKind::Matrix) {
          rows = raw_rows + 1;
          cols = raw_cols + 1;
        } else {
          bail(e.name + ": expected vector or matrix");
        }
        break;
      case CallableTransformKind::StochasticColumn:
      case CallableTransformKind::StochasticRow:
        if (leaf != ViewKind::Matrix) bail(e.name + ": expected matrix");
        rows =
            raw_rows + (spec.kind == CallableTransformKind::StochasticColumn);
        cols = raw_cols + (spec.kind == CallableTransformKind::StochasticRow);
        break;
      case CallableTransformKind::CholeskyFactorCorr:
      case CallableTransformKind::CorrMatrix:
      case CallableTransformKind::CovMatrix:
        if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
        out_leaf = ViewKind::Matrix;
        rows = cols = cint(e.args[1]);
        break;
      case CallableTransformKind::CholeskyFactorCov:
        if (leaf != ViewKind::Vector) bail(e.name + ": expected vector");
        out_leaf = ViewKind::Matrix;
        rows = cint(e.args[1]);
        cols = cint(e.args[2]);
        break;
      default:
        bail(e.name + ": invalid structured transform");
    }
    if (batch < 0 || rows < 0 || cols < 0) bail(e.name + ": invalid shape");
    tr.batch = (int32_t)batch;
    tr.inner_raw = leaf == ViewKind::Matrix ? (int32_t)(raw_rows * raw_cols)
                                            : (int32_t)dims.back();
    tr.out_rows = (int32_t)rows;
    tr.out_cols = (int32_t)cols;
    const int64_t inner_con = out_leaf == ViewKind::Matrix ? rows * cols : rows;
    if (inner_con < 0 || batch > kMaxRegs ||
        (batch && inner_con > kMaxRegs / batch))
      bail(e.name + ": result needs too many registers");
    tr.out_len = (int32_t)(batch * inner_con);
    out.len = tr.out_len;
    out.kind = outer_rank ? ViewKind::Array : out_leaf;
    out.rows = out.kind == ViewKind::Matrix ? rows : 0;
    out.cols = out.kind == ViewKind::Matrix ? cols : 0;
    if (outer_rank) {
      out.dims.assign(dims.begin(), dims.begin() + outer_rank);
      out.dims.push_back(rows);
      if (out_leaf == ViewKind::Matrix) out.dims.push_back(cols);
      out.leaf = out_leaf;
    }
  }
  tr.out = alloc(tr.out_len);
  tr.jac = alloc(1);
  out.reg = tr.out;
  p.transforms.push_back(tr);
  p.code.push_back(
      Program::Instr{Program::TRANSFORM, 0, (int)p.transforms.size() - 1});
  if (spec.direction == TransformDirection::Jacobian && !in_write_array) {
    if (target_reg < 0) bail("jacobian transform has no target");
    emit(Program::ADD, target_reg, target_reg, tr.jac);
  }
  return out;
}

Range ProgramCompiler::fun(const mir::Expr& e) {
  // A scalar reduction does not need a variable-width register result.
  // Keep the source's fixed capacity and check both bounds when executed.
  // Other dynamic slices retain their existing refusal; no array padding
  // or unused tail values participate in the reduction or its derivative.
  if (e.fn_lib == mir::Expr::Lib::StanLib && e.name == "log_sum_exp" &&
      e.args.size() == 1 && e.args[0].kind == mir::Expr::Indexed) {
    const auto& slice = e.args[0];
    if (slice.args.size() == 2 &&
        slice.args[1].kind == mir::Expr::IndexBetween &&
        slice.args[1].args.size() == 2 &&
        (mir::is_unsized(slice.args[0], mir::UnsizedLeaf::Vector) ||
         mir::is_unsized(slice.args[0], mir::UnsizedLeaf::RowVector))) {
      long lo, hi;
      if (!try_cint(slice.args[1].args[0], &lo) ||
          !try_cint(slice.args[1].args[1], &hi)) {
        const Range base = expr(slice.args[0]);
        const Range lower = expr(slice.args[1].args[0]);
        const Range upper = expr(slice.args[1].args[1]);
        if (!is_scalar(lower) || !is_scalar(upper))
          bail("non-scalar slice bound");
        const int r = alloc(1);
        emit(Program::DYN_LSE_RANGE, r, base.reg, lower.reg, upper.reg,
             base.len);
        return {r, 1};
      }
    }
  }
  if (const auto intrinsic = mir::stateful_intrinsic_kind(e)) {
    switch (*intrinsic) {
      case mir::StatefulIntrinsicKind::Target: {
        if (target_base_reg < 0) {
          Range base;
          if (!bind_target || !bind_target(&base) || !is_scalar(base))
            bail("target() is unavailable in this context");
          target_base_reg = base.reg;
        }
        if (target_reg < 0) return {target_base_reg, 1};
        const int current = alloc(1);
        emit(Program::ADD, current, target_base_reg, target_reg);
        return {current, 1};
      }
    }
  }
  if (const auto value = mir::nullary_constant(e)) return {konst(*value), 1};
  if (const auto higher_order = mir::higher_order_call(e)) {
    switch (higher_order->family) {
      case mir::HigherOrderFamily::ReduceSum:
        return reduce_sum_call(e);
      case mir::HigherOrderFamily::MapRect:
        return map_rect_call(e);
      default:
        Range result;
        if (lower_higher_order && lower_higher_order(e, &result)) return result;
        break;
    }
  }
  // A shape query is a constant whatever surrounds it. Ahead of every
  // other case because `FnLength` is an internal function and the rest
  // are library ones, and they are all answered the same way: from the
  // named value's view where there is one, and otherwise from the view
  // of the value the argument builds.
  if (is_shape_query(e)) {
    Range v;
    if (!static_view(e.args[0], &v)) v = expr(e.args[0]);
    return {konst((double)shape_query(e, v)), 1};
  }
  // dims is the container-answer shape query: every extent, as constants.
  if (const BuiltinSpec* query =
          e.args.size() == 1
              ? shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::ShapeQuery)
              : nullptr) {
    Range v;
    if (!static_view(e.args[0], &v)) v = expr(e.args[0]);
    if (v.kind == ViewKind::Array && v.dims.empty()) v.dims = {v.len};
    const std::vector<int64_t> extents =
        builtin_shape_query(*query, builtin_argument_shape(e.args[0], v));
    const std::vector<double> values(extents.begin(), extents.end());
    const int r = alloc((int)values.size());
    if (!values.empty()) emit_const(r, values.data(), (int)values.size());
    Range out{r, (int)values.size()};
    out.kind = ViewKind::Array;
    out.dims = {(int64_t)values.size()};
    return out;
  }
  if (e.data_only && e.unsized.depth != 0 &&
      e.unsized.leaf == mir::UnsizedLeaf::Int) {
    std::vector<long> values;
    std::vector<int64_t> dims;
    if (external_int_array(e, &values, &dims)) {
      if (values.size() > (size_t)kMaxRegs)
        bail("integer array constant needs too many registers");
      const std::vector<int64_t> logical_dims =
          dims.empty() ? std::vector<int64_t>{(int64_t)values.size()} : dims;
      const std::vector<double> real_values =
          int_array_graph_values(values, logical_dims);
      const int r = alloc((int)values.size());
      emit_const(r, real_values.data(), (int)real_values.size());
      Range out{r, (int)values.size()};
      out.kind = ViewKind::Array;
      out.dims = logical_dims;
      return out;
    }
  }
  if (rng_call_name(e.name)) return rng_call(e);
  CallableTransformSpec transform;
  if (callable_transform(e.name, &transform))
    return transform_call(e, transform);
  const FunctionSpec* registered = function_spec(e);
  const BuiltinSpec* builtin =
      registered != nullptr && registered->builtin() != nullptr
          ? registered->builtin()
          : nullptr;
  // Registered constructors fold to one constant register range through
  // the shared Stan Math evaluator; every argument is a data scalar, so
  // extents, spacing rules, and domain errors are CmdStan's own.
  if (const BuiltinSpec* ctor = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::Constructor)) {
    std::vector<double> ctor_args;
    ctor_args.reserve(e.args.size());
    for (size_t k = 0; k < e.args.size(); ++k)
      ctor_args.push_back(ctor->arguments[k] == BuiltinArgumentKind::Integer
                              ? (double)cint(e.args[k])
                              : creal(e.args[k]));
    ConstructorValue built;
    try {
      built = evaluate_constructor_builtin(*ctor, ctor_args);
    } catch (const std::domain_error&) {
      // Stan Math's own validation, the rejection CmdStan throws.
      throw;
    } catch (const std::invalid_argument& error) {
      bail(e.name + ": " + std::string(error.what()));
    }
    const int len = (int)built.values.size();
    const int r = alloc(len);
    if (len != 0) emit_const(r, built.values.data(), len);
    Range out{r, len};
    switch (ctor->constructor_container) {
      case FunctionContainerKind::Vector:
        out.kind = ViewKind::Vector;
        break;
      case FunctionContainerKind::RowVector:
        out.kind = ViewKind::RowVector;
        break;
      case FunctionContainerKind::Matrix:
        out.kind = ViewKind::Matrix;
        out.rows = built.dimensions[0];
        out.cols = built.dimensions[1];
        break;
      default:
        out.kind = ViewKind::Array;
        out.leaf = ViewKind::Flat;
        out.dims = built.dimensions;
        break;
    }
    return out;
  }

  // Registered slice/view selections: the shared resolver maps result
  // cells to source registers over the register file's outer-major array
  // storage (Eigen leaves are column-major under both conventions), with
  // Stan Math's own index checks (out_of_range and domain_error propagate
  // as CmdStan's rejections). Every selection is a per-lane MOV, so later
  // writes to a source variable cannot alias the result and adjoints
  // accumulate through MOV's reverse pass.
  if (const BuiltinSpec* slice = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::SliceView)) {
    const Range a = expr(e.args[0]);
    Range b{};
    BuiltinSliceMap map;
    try {
      if (builtin_slice_is_append(slice->slice)) {
        // Appends map result cells over both operands' concatenated
        // storage; a source cell at or past the left run reads the right.
        b = expr(e.args[1]);
        map = builtin_append_map(*slice, builtin_argument_shape(e.args[0], a),
                                 builtin_argument_shape(e.args[1], b),
                                 SliceStorageOrder::OuterMajor);
      } else {
        std::vector<int64_t> indexes;
        indexes.reserve(e.args.size() - 1);
        for (size_t k = 1; k < e.args.size(); ++k)
          indexes.push_back(cint(e.args[k]));
        map = builtin_slice_map(*slice, builtin_argument_shape(e.args[0], a),
                                indexes, SliceStorageOrder::OuterMajor);
      }
    } catch (const std::invalid_argument& error) {
      bail(e.name + ": " + std::string(error.what()));
    }
    const auto stamp = [&](Range out) {
      return shaped(std::move(out), map.result);
    };
    // A reshape's identity map relabels the source run outright -- the
    // zero-instruction lowering the named vector transpose always used;
    // persistence still copies at the assignment site.
    if (builtin_slice_is_reshape(slice->slice) &&
        map.kind == BuiltinSliceMap::Kind::Contiguous)
      return stamp(a);
    const int r = alloc((int)map.count);
    const auto source_cell = [&](int64_t k) {
      switch (map.kind) {
        case BuiltinSliceMap::Kind::Contiguous:
          return map.offset + k;
        case BuiltinSliceMap::Kind::Strided:
          return map.offset + k * map.stride;
        case BuiltinSliceMap::Kind::Transpose: {
          const int64_t rows = map.result.dimensions[0];
          const int64_t cols = map.result.dimensions[1];
          return k / rows + cols * (k % rows);
        }
        case BuiltinSliceMap::Kind::Gather:
          break;
      }
      return map.gather[(size_t)k];
    };
    for (int64_t k = 0; k < map.count; ++k) {
      const int64_t cell = source_cell(k);
      const int source =
          cell < a.len ? a.reg + (int)cell : b.reg + (int)(cell - a.len);
      emit(Program::MOV, r + (int)k, source);
    }
    return stamp(Range{r, (int)map.count});
  }

  // Registered paired reductions: the dot kernel over two equal-length
  // ranges (or one, paired with itself); squared_distance subtracts first
  // with the same native SUB lanes the graph's OP_SUB computes.
  if (const BuiltinSpec* paired = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::PairedReduction)) {
    Range a = expr(e.args[0]);
    Range b = paired->arity == 2 ? expr(e.args[1]) : a;
    (void)resolved_builtin_layout(
        e, *paired,
        paired->arity == 2 ? std::vector<Range>{a, b} : std::vector<Range>{a});
    Range out{0, 1};
    if (paired->difference) {
      const int d = alloc(a.len);
      for (int i = 0; i < a.len; ++i)
        emit(Program::SUB, d + i, a.reg + i, b.reg + i);
      const Range difference{d, a.len};
      return kernel_call(OP_DOT, {difference, difference}, out, 0, 0x3, {}, {},
                         e.name);
    }
    return kernel_call(OP_DOT, {a, b}, out, 0, 0x3, {}, {}, e.name);
  }

  // Registered grouped reductions: the shared grouped dot kernel over the
  // operands' column-major ranges, one in-order dot per column or row --
  // the accumulation the AoS reverse-mode overloads perform. Previously
  // unsupported here.
  if (const BuiltinSpec* grouped = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::GroupedReduction)) {
    Range a = expr(e.args[0]);
    Range b = grouped->arity == 2 ? expr(e.args[1]) : a;
    BuiltinGroupedDotMap map;
    try {
      map = builtin_grouped_dot_map(
          *grouped, builtin_argument_shape(e.args[0], a),
          builtin_argument_shape(e.args[grouped->arity == 2 ? 1 : 0], b));
    } catch (const std::invalid_argument& error) {
      bail(e.name + ": " + std::string(error.what()));
    }
    Range out{0, (int)map.groups};
    out.kind = function_view_kind(map.result.container);
    return kernel_call(OP_GROUP_DOT, {a, b}, out, 0, 0x3,
                       {(int)map.groups, (int)map.width, (int)map.group_stride,
                        (int)map.cell_stride},
                       {}, e.name);
  }

  if (e.name == "eval" && e.args.size() == 1) return expr(e.args[0]);

  // Registered matrix operations: the same dedicated kernels the graph
  // emits, called over the operands' column-major ranges. The active
  // variant bit mirrors the graph's autodiff stamp -- any non-data
  // operand -- with the kernels' values_only() guard covering
  // values-only executions, exactly as it does for graph ops.
  if (const BuiltinSpec* matrix = shaped_builtin_spec(
          e.name, e.args.size(), BuiltinShapePolicy::MatrixOp))
    return matrix_op_call(e, matrix);

  // Registered reductions never take the elementwise dispatch below. sum
  // and log_sum_exp keep their native register loops further down; the
  // remaining reductions call their registered kernel with a scalar out.
  if (const BuiltinSpec* reduction =
          reduction_builtin_spec(e.name, e.args.size());
      reduction != nullptr) {
    builtin = nullptr;
    if (reduction->opcode != OP_SUM_VEC &&
        reduction->opcode != OP_LOG_SUM_EXP) {
      const std::vector<Range> args{expr(e.args[0])};
      (void)resolved_builtin_layout(e, *reduction, args);
      // mean groups its sum as Stan Math's Eigen mean does: in order for
      // an autodiff operand, by the source layout for a data vector, in
      // packets for a data std::vector.
      std::pair<SumGrouping, int64_t> grouping{SumGrouping::InOrder, 0};
      if (reduction->opcode == OP_MEAN &&
          (in_write_array || e.args[0].data_only))
        grouping = sum_grouping(mir::eigen_leaf(e.args[0]) &&
                                        e.args[0].unsized.depth == 0
                                    ? mir::source_expression_layout(e.args[0])
                                    : ExpressionLayout::direct());
      std::vector<int> idata;
      if (grouping.first == SumGrouping::Phased)
        idata.push_back(static_cast<int>(grouping.second));
      Range out{0, 1};
      const bool var_array = reduction->opcode == OP_VARIANCE &&
                             e.args[0].unsized.depth == 1 && !in_write_array &&
                             !e.args[0].data_only;
      return kernel_call(reduction->opcode, args, out,
                         var_array ? kVarianceOfVarArray
                                   : static_cast<uint8_t>(grouping.first),
                         reduction->activity_mask, std::move(idata), {},
                         e.name);
    }
  }
  // Matrix right-division is a solve, not the scalar/elementwise DIV in the
  // descriptor. Its specialized shape-aware lowering remains below, as does
  // predicate lowering: a Predicate descriptor has no kernel, and its
  // comparison-opcode spelling lives with the other operators.
  if (builtin != nullptr && builtin->shape != BuiltinShapePolicy::Predicate &&
      builtin->shape != BuiltinShapePolicy::Product &&
      builtin->shape != BuiltinShapePolicy::Solve &&
      !(e.name == "Divide__" && e.args.size() == 2 &&
        mir::is_unsized(e.args[1], mir::UnsizedLeaf::Matrix))) {
    if (const auto native = native_builtin_code(builtin->opcode))
      return native_builtin_call(e, *builtin, *native);
    return builtin_kernel_call(e, *builtin);
  }
  if (const auto wiener = wiener_form(e.name, e.args.size())) {
    const int nargs = wiener->reals;
    std::vector<Range> args;
    for (int k = 0; k < nargs; ++k) args.push_back(expr(e.args[k]));
    int lanes = 1;
    for (const Range& a : args) lanes = std::max(lanes, a.len);
    int active = 0;
    for (int k = 0; k < nargs; ++k) {
      if (args[k].len != lanes) bail(e.name + ": arguments differ in length");
      if (autodiff(e.args[k])) active |= 1 << k;
    }
    const int packed = alloc(nargs * lanes);
    for (int k = 0; k < nargs; ++k)
      for (int i = 0; i < lanes; ++i)
        emit(Program::MOV, packed + k * lanes + i, args[k].reg + i);
    const Range precision = wiener->has_precision
                                ? expr(e.args[nargs])
                                : Range{konst(wiener->default_precision), 1};
    const bool propto_density =
        wiener->kind == WienerKind::Lpdf && e.fn_propto && propto_ctx;
    return kernel_call(
        OP_WIENER_PACKED_LPDF, {Range{packed, nargs * lanes}, precision},
        Range{0, 1}, 0, active ? 0x1 : 0,
        {nargs, lanes, active, propto_density ? 1 : 0, (int)wiener->kind}, {},
        e.name);
  }
  if (registered != nullptr && registered->density() != nullptr)
    return density_call(e, *registered->density());
  if (e.name == "tcrossprod" && e.args.size() == 1)
    return matrix_gram(expr(e.args[0]), false);
  if ((e.name == "LDivide__" || e.name == "mdivide_left") &&
      e.args.size() == 2) {
    const Range divisor = expr(e.args[0]);
    const Range rhs = expr(e.args[1]);
    if (divisor.kind != ViewKind::Matrix || divisor.rows != divisor.cols)
      bail("mdivide_left requires a square matrix divisor");
    if ((rhs.kind != ViewKind::Matrix && rhs.kind != ViewKind::Vector) ||
        (rhs.kind == ViewKind::Matrix && rhs.rows != divisor.rows) ||
        (rhs.kind == ViewKind::Vector && rhs.len != divisor.rows))
      bail("mdivide_left right-hand side size mismatch");
    const int r = alloc(rhs.len);
    const int32_t encoded_rows = rhs.kind == ViewKind::Vector
                                     ? -(int32_t)divisor.rows
                                     : (int32_t)divisor.rows;
    if (rhs.len != 0)
      p.code.push_back(Program::Instr{Program::MDIVIDE_LEFT, r, divisor.reg,
                                      rhs.reg, encoded_rows, rhs.len});
    Range out = rhs;
    out.reg = r;
    return out;
  }
  if (e.name == "mdivide_right_spd" && e.args.size() == 2) {
    const Range lhs = expr(e.args[0]);
    const Range divisor = expr(e.args[1]);
    if (divisor.kind != ViewKind::Matrix || divisor.rows != divisor.cols)
      bail("mdivide_right_spd requires a square matrix divisor");
    if ((lhs.kind != ViewKind::Matrix && lhs.kind != ViewKind::RowVector) ||
        (lhs.kind == ViewKind::Matrix && lhs.cols != divisor.cols) ||
        (lhs.kind == ViewKind::RowVector && lhs.len != divisor.cols))
      bail("mdivide_right_spd left-hand side size mismatch");
    const int r = alloc(lhs.len);
    const int32_t encoded_cols = lhs.kind == ViewKind::RowVector
                                     ? -(int32_t)divisor.cols
                                     : (int32_t)divisor.cols;
    if (lhs.len != 0)
      p.code.push_back(Program::Instr{Program::MDIVIDE_RIGHT_SPD, r,
                                      divisor.reg, lhs.reg, encoded_cols,
                                      lhs.len});
    Range out = lhs;
    out.reg = r;
    return out;
  }
  // The remaining registered solves (and the operator spellings the two
  // native instructions above do not cover) go through the graph solve
  // kernels: the shared resolver validates the square divisor and the
  // dividend's conformity, and the variant carries the same operand-type
  // bits the graph lowering stamps -- result var, vector-vs-one-column
  // dividend, and the divisor/dividend scalar types.
  {
    const BuiltinSpec* solve =
        shaped_builtin_spec(e.name, e.args.size(), BuiltinShapePolicy::Solve);
    if (solve == nullptr && e.name == "Divide__" && e.args.size() == 2 &&
        mir::is_unsized(e.args[1], mir::UnsizedLeaf::Matrix))
      solve =
          shaped_builtin_spec("mdivide_right", 2, BuiltinShapePolicy::Solve);
    if (solve != nullptr) {
      std::vector<Range> args;
      args.reserve(2);
      for (const mir::Expr& argument : e.args) args.push_back(expr(argument));
      BuiltinSolveMap map;
      try {
        map = builtin_solve_map(*solve,
                                builtin_argument_shape(e.args[0], args[0]),
                                builtin_argument_shape(e.args[1], args[1]));
      } catch (const std::invalid_argument& error) {
        bail(e.name + ": " + std::string(error.what()));
      }
      const size_t dividend = solve->solve_left ? 1 : 0;
      const bool divisor_active = !e.args[1 - dividend].data_only;
      const bool dividend_active = !e.args[dividend].data_only;
      const bool dm = args[dividend].kind == ViewKind::Matrix;
      Range out{0, (int)map.result.storage_size};
      out = shaped(std::move(out), map.result);
      const uint8_t variant =
          (uint8_t)(((divisor_active || dividend_active) ? 1u : 0u) |
                    (dm ? 0u : 2u) | (divisor_active ? 4u : 0u) |
                    (dividend_active ? 8u : 0u));
      return kernel_call(solve->opcode, args, out, variant,
                         solve->activity_mask,
                         {(int)map.order, (int)map.columns}, {}, e.name);
    }
  }
  if (e.fn_lib == mir::Expr::Lib::UserDefined) {
    auto it = funs.find(mir::function_key(e));
    if (it == funs.end()) bail("unknown function " + e.name);
    return inline_call(*it->second, inline_arguments(e.args), false,
                       propto_ctx && mir::inherits_propto(e));
  }
  if (e.fn_lib == mir::Expr::Lib::Internal) {
    if (e.name == "FnMakeArray" || e.name == "FnMakeRowVec") {
      std::vector<Range> parts;
      int total = 0;
      for (const auto& a : e.args) {
        parts.push_back(expr(a));
        if (parts.back().len > kMaxRegs - total)
          bail("array literal needs too many registers");
        total += parts.back().len;
      }
      // A matrix literal is rows of row-vectors, while a matrix register
      // run is column-major, so this is the one literal that transposes
      // rather than concatenating.
      if (e.name == "FnMakeRowVec" &&
          mir::is_unsized(e, mir::UnsizedLeaf::Matrix)) {
        if (parts.empty()) bail("matrix literal has no rows");
        for (const Range& q : parts)
          if (q.kind != ViewKind::RowVector || q.len != parts.front().len)
            bail("matrix literal rows have different logical views");
        const int64_t rows = (int64_t)parts.size();
        const int64_t cols = parts.front().len;
        const int r = alloc(total);
        for (int64_t j = 0; j < cols; ++j)
          for (int64_t i = 0; i < rows; ++i)
            emit(Program::MOV, r + (int)(j * rows + i),
                 parts[(size_t)i].reg + (int)j);
        Range out{r, total};
        out.kind = ViewKind::Matrix;
        out.rows = rows;
        out.cols = cols;
        return out;
      }
      ViewKind array_leaf = ViewKind::Flat;
      std::vector<int64_t> array_leaf_dims;
      if (e.name == "FnMakeArray") {
        if (!parts.empty() && !is_scalar(parts.front())) {
          for (const Range& q : parts)
            if (!same_view(q, parts.front()))
              bail("array literal elements have different logical views");
          if (parts.front().kind == ViewKind::Array) {
            // An outer array literal adds one axis to the complete child
            // geometry; the scalar width of a singleton child must not
            // collapse that axis. Storage is already element-contiguous,
            // so only the neutral logical view needs extending.
            array_leaf = parts.front().leaf;
            array_leaf_dims = parts.front().dims;
          } else if (parts.front().kind == ViewKind::Matrix) {
            array_leaf = ViewKind::Matrix;
            array_leaf_dims = {parts.front().rows, parts.front().cols};
          } else {
            array_leaf = parts.front().kind;
            array_leaf_dims = {(int64_t)parts.front().len};
          }
        } else {
          for (const Range& q : parts)
            if (!is_scalar(q))
              bail("array literal mixes scalar and container elements");
        }
      }
      const int r = alloc(total);
      int at = 0;
      for (const Range& q : parts)
        for (int k = 0; k < q.len; ++k) emit(Program::MOV, r + at++, q.reg + k);
      Range out{r, total};
      out.kind =
          e.name == "FnMakeRowVec" ? ViewKind::RowVector : ViewKind::Array;
      if (e.name == "FnMakeArray") {
        out.dims = {(int64_t)e.args.size()};
        out.leaf = array_leaf;
        for (int64_t d : array_leaf_dims) out.dims.push_back(d);
      }
      return out;
    }
    bail("internal function " + e.name);
  }
  const mir::ExtremaCall extrema = mir::extrema_call(e);
  if (extrema.kind != mir::ExtremaKind::Legacy) {
    Range a;
    if (extrema.surface == mir::ExtremaSurface::IntPair) {
      const Range lhs = expr(e.args[0]);
      const Range rhs = expr(e.args[1]);
      if (!is_scalar(lhs) || !is_scalar(rhs))
        bail("min/max integer pair needs scalar arguments");
      a = Range{alloc(2), 2};
      emit(Program::MOV, a.reg, lhs.reg);
      emit(Program::MOV, a.reg + 1, rhs.reg);
    } else {
      a = expr(e.args[0]);
    }
    const int r = alloc(1);
    const bool maximum = extrema.kind == mir::ExtremaKind::Max;
    const bool integer = extrema.surface == mir::ExtremaSurface::IntArray ||
                         extrema.surface == mir::ExtremaSurface::IntPair;
    // Matrix<var>, vector<var>, and std::vector<var> all reduce in ascending
    // scalar order. Otherwise retain the source expression's double
    // evaluator grouping even though `expr` materialized it into a flat
    // register run above. This mirrors Lowering::reduction_grouping.
    const bool active = !in_write_array && !e.args[0].data_only;
    const ExpressionLayout layout =
        integer || active ? ExpressionLayout::scalar()
                          : mir::source_expression_layout(e.args[0]);
    if (!layout.known()) bail("min/max expression grouping is not native");
    int32_t flags = integer ? kProgramExtremaInteger : 0;
    if (layout.kind == ExpressionLayout::Kind::Scalar) {
      flags |= kProgramExtremaScalar;
    } else if (layout.kind == ExpressionLayout::Kind::Direct &&
               layout.element_offset != 0) {
      flags |= kProgramExtremaPhased;
      const int64_t phase = layout.element_offset % extrema_phase_modulus();
      flags |= static_cast<int32_t>(phase << kProgramExtremaPhaseShift);
    }
    p.code.push_back(Program::Instr{Program::EXTREMA_RANGE, r, a.reg,
                                    maximum ? 1 : 0, flags, a.len});
    return typed(Range{r, 1}, e.unsized);
  }
  if (e.name == "fma" && e.args.size() == 3) {
    // Fused, elementwise with scalar broadcast, mirroring OP_FMA.
    const Range a = expr(e.args[0]), b = expr(e.args[1]), c = expr(e.args[2]);
    int n = 1;
    Range shaped{0, 1};
    for (const Range* x : {&a, &b, &c}) {
      if (x->kind == ViewKind::Array)
        bail("array arithmetic is unsupported by the register program");
      if (is_scalar(*x)) continue;
      if (n != 1 && x->len != n) bail("fma on different lengths");
      n = x->len;
      shaped = *x;
    }
    const int r = alloc(n);
    for (int i = 0; i < n; ++i)
      emit(Program::FMA, r + i, a.reg + (is_scalar(a) ? 0 : i),
           b.reg + (is_scalar(b) ? 0 : i), c.reg + (is_scalar(c) ? 0 : i));
    Range out = shaped;
    out.reg = r;
    out.len = n;
    return typed(out, e.unsized);
  }
  if (e.name == "log_mix" && e.args.size() == 3) {
    const Range theta = expr(e.args[0]), a = expr(e.args[1]),
                b = expr(e.args[2]);
    if (!is_scalar(theta) || !is_scalar(a) || !is_scalar(b))
      bail("log_mix on containers");
    const int r = alloc(1);
    emit(Program::LOG_MIX, r, theta.reg, a.reg, b.reg);
    return {r, 1};
  }
  if ((e.name == "diag_pre_multiply" || e.name == "diag_post_multiply") &&
      e.args.size() == 2) {
    // One instruction preserves stan-math's diagonal-product callback as a
    // unit. Scalar MULs would accumulate the repeated vector's adjoint in
    // tape order instead of the callback's rowwise/colwise reduction order.
    const bool pre = e.name == "diag_pre_multiply";
    const Range v = expr(e.args[pre ? 0 : 1]);
    const Range m = expr(e.args[pre ? 1 : 0]);
    if (m.kind != ViewKind::Matrix)
      bail(e.name + " requires a matrix argument");
    if (v.kind != ViewKind::Vector && v.kind != ViewKind::RowVector)
      bail(e.name + " requires a vector argument");
    const int64_t expected = pre ? m.rows : m.cols;
    if (v.len != expected)
      bail(e.name + " vector length does not match the matrix");
    const int r = alloc(m.len);
    // A zero-size multiplication has no values or adjoints. In particular,
    // do not narrow an arbitrarily large empty dimension into Instr.
    if (m.len != 0)
      p.code.push_back(Program::Instr{
          pre ? Program::DIAG_PRE_MULTIPLY : Program::DIAG_POST_MULTIPLY, r,
          v.reg, m.reg, (int32_t)m.rows, (int32_t)m.cols});
    Range out = m;
    out.reg = r;
    return typed(out, e.unsized);
  }
  if (e.args.size() == 2) {
    // An int-typed binary is integer arithmetic, and `divide` truncates
    // where the real DIV below does not: `divide(7, 2)` is 3, not 3.5.
    // The register file holds only reals, so fold the integer answer
    // whenever cint can reach it rather than emitting real arithmetic.
    if (mir::is_unsized(e, mir::UnsizedLeaf::Int)) {
      long v;
      if (try_cint(e, &v)) return {konst((double)v), 1};
    }
    const Range a = expr(e.args[0]), b = expr(e.args[1]);
    const bool a_scalar = is_scalar(a);
    const bool b_scalar = is_scalar(b);
    if (a.kind == ViewKind::Array || b.kind == ViewKind::Array)
      bail("array arithmetic is unsupported by the register program");
    if ((e.name == "Times__" || e.name == "multiply") && !a_scalar &&
        !b_scalar) {
      // The shared product resolver classifies the matvec/GEMM/outer/
      // inner forms and validates the inner dimension. Each form calls the
      // kernel that the graph lowering emits for it (lower_funapp.cpp), so
      // both paths, and Stan, round the products and sums the same way.
      // Eigen fuses each multiply-add on arm64; a MUL/ADD chain does not.
      BuiltinProductMap map;
      try {
        map = builtin_product_map(builtin_argument_shape(e.args[0], a),
                                  builtin_argument_shape(e.args[1], b));
      } catch (const std::invalid_argument& error) {
        bail(builtin_error_message(e.name, error));
      }
      const int64_t rows = map.m, inner = map.k, cols = map.n;
      const ViewKind result_kind =
          map.result.container == FunctionContainerKind::Matrix
              ? ViewKind::Matrix
          : map.result.container == FunctionContainerKind::Vector
              ? ViewKind::Vector
          : map.result.container == FunctionContainerKind::RowVector
              ? ViewKind::RowVector
              : ViewKind::Flat;

      const int64_t width = rows * cols;
      if (width > kMaxRegs) bail("matrix product needs too many registers");
      Range out{0, (int)width};
      if (map.kind == BuiltinProductMap::Kind::Inner)
        out = kernel_call(OP_DOT, {a, b}, out, 0, 0x3, {}, {}, e.name);
      else if (map.kind == BuiltinProductMap::Kind::MatVec &&
               e.args[0].data_only && b.kind == ViewKind::Vector)
        out = kernel_call(OP_MATVEC, {a, b}, out, 0, 0x2,
                          {(int)rows, (int)inner}, {}, e.name);
      else
        out = kernel_call(OP_GEMM, {a, b}, out, 0, 0x3,
                          {(int)rows, (int)inner, (int)cols}, {}, e.name);
      out.kind = result_kind;
      if (result_kind == ViewKind::Matrix) {
        out.rows = rows;
        out.cols = cols;
      }
      return typed(out, e.unsized);
    }
    if (!a_scalar && !b_scalar && !same_view(a, b))
      bail("binary " + e.name + " on different logical views");
    // `multiply` rides with `Times__` here for the same reason it does
    // in the graph lowering: on two containers it is linear algebra,
    // not the elementwise MUL the register file would emit.
    const int n = a_scalar ? b.len : (b_scalar ? a.len : a.len);
    Program::Code c;
    // The named spellings of the operators, on the same opcodes: a
    // region whose control flow depends on a parameter has to compile
    // here or not at all, so a gap is a hard error rather than a slow
    // path.
    if (e.name == "Plus__" || e.name == "add")
      c = Program::ADD;
    else if (e.name == "Minus__" || e.name == "subtract")
      c = Program::SUB;
    else if (e.name == "Times__" || e.name == "EltTimes__" ||
             e.name == "multiply" || e.name == "elt_multiply")
      c = Program::MUL;
    else if (e.name == "IntDivide__" || e.name == "Divide__" ||
             e.name == "EltDivide__" || e.name == "divide" ||
             e.name == "elt_divide")
      c = mir::is_unsized(e, mir::UnsizedLeaf::Int) ? Program::IDIV
                                                    : Program::DIV;
    else if (e.name == "Modulo__")
      c = Program::IMOD;
    else if (e.name == "Pow__" || e.name == "pow")
      c = Program::POW;
    else if (e.name == "fmax")
      c = Program::FMAX;
    else if (e.name == "fmin")
      c = Program::FMIN;
    else if (e.name == "log_sum_exp")
      c = Program::LSE2;
    else if (e.name == "log_diff_exp")
      c = Program::LOG_DIFF_EXP;
    else if (const BuiltinSpec* pred = shaped_builtin_spec(
                 e.name, 2, BuiltinShapePolicy::Predicate)) {
      // Comparisons on the comparison opcodes, for both the operator
      // spellings and the logical_* library names. logical_and and
      // logical_or fold each side's zero-ness first: both sides are
      // always evaluated, Stan Math's own (non-short-circuit) rule.
      switch (pred->predicate) {
        case BuiltinPredicate::Gt:
          c = Program::GT;
          break;
        case BuiltinPredicate::Gte:
          c = Program::GE;
          break;
        case BuiltinPredicate::Lt:
          c = Program::LT;
          break;
        case BuiltinPredicate::Lte:
          c = Program::LE;
          break;
        case BuiltinPredicate::Eq:
          c = Program::EQ;
          break;
        case BuiltinPredicate::Neq:
          c = Program::NE;
          break;
        case BuiltinPredicate::And:
        case BuiltinPredicate::Or: {
          const int zero = konst(0.0);
          const int lhs_set = alloc(n), rhs_set = alloc(n), both = alloc(n);
          for (int i = 0; i < n; ++i) {
            emit(Program::NE, lhs_set + i, a.reg + (a_scalar ? 0 : i), zero);
            emit(Program::NE, rhs_set + i, b.reg + (b_scalar ? 0 : i), zero);
            if (pred->predicate == BuiltinPredicate::And) {
              emit(Program::MUL, both + i, lhs_set + i, rhs_set + i);
            } else {
              emit(Program::ADD, both + i, lhs_set + i, rhs_set + i);
              emit(Program::NE, both + i, both + i, zero);
            }
          }
          return typed(Range{both, n}, e.unsized);
        }
        default:
          bail("function " + e.name);
      }
    } else
      bail("function " + e.name);
    if (mir::is_unsized(e, mir::UnsizedLeaf::Int)) c = integer_instruction(c);
    const int law =
        c == Program::POW
            ? mir::pow_zero_base_law(e.args[0], e.args[1], !e.args[1].data_only)
        : c == Program::FMAX || c == Program::FMIN
            ? (e.args[0].data_only ? 0 : 0x1) | (e.args[1].data_only ? 0 : 0x2)
            : 0;
    const int r = alloc(n);
    for (int i = 0; i < n; ++i)
      emit(c, r + i, a.reg + (a_scalar ? 0 : i), b.reg + (b_scalar ? 0 : i), 0,
           law);
    Range out{r, n};
    if (a_scalar && !b_scalar)
      out = b;
    else if (b_scalar && !a_scalar)
      out = a;
    else if (same_view(a, b))
      out = a;
    out.reg = r;
    out.len = n;
    return typed(out, e.unsized);
  }
  if (e.args.size() == 1) {
    const Range a = expr(e.args[0]);
    if (e.name == "log_sum_exp") {
      if (a.len == 0)
        return {konst(-std::numeric_limits<double>::infinity()), 1};
      const int r = alloc(1);
      p.code.push_back(
          Program::Instr{Program::LSE_RANGE, r, a.reg, 0, 0, a.len});
      return {r, 1};
    }
    if (e.name == "sum") {
      if (a.len == 0) return {konst(0.0), 1};
      const bool integer = mir::is_unsized(e, mir::UnsizedLeaf::Int);
      // A long real sum takes OP_SUM_VEC's compensated sum, as on the graph.
      if (!integer && a.len > kPlainSumMax)
        return kernel_call(OP_SUM_VEC, {a}, Range{0, 1}, 0, 0x1, {}, {},
                           e.name);
      const int r = alloc(1);
      emit(Program::MOV, r, a.reg);
      const auto add = integer ? Program::IADD : Program::ADD;
      for (int i = 1; i < a.len; ++i) emit(add, r, r, a.reg + i);
      return {r, 1};
    }
    // Registered unary predicates, spelled on the comparison opcodes
    // rather than opcodes of their own: both read through value_of, so
    // neither carries an adjoint edge, which is what a 0/1 answer wants.
    // x != x holds for NaN alone.
    if (const BuiltinSpec* pred =
            shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::Predicate)) {
      const bool nan = pred->predicate == BuiltinPredicate::IsNan;
      const int rhs = nan ? -1 : konst(0.0);
      const Program::Code c = nan ? Program::NE
                              : pred->predicate == BuiltinPredicate::IntStep
                                  ? Program::GT
                                  : Program::EQ;
      const int r = alloc(a.len);
      if (pred->predicate == BuiltinPredicate::IsInf) {
        const int pos = konst(std::numeric_limits<double>::infinity());
        const int neg = konst(-std::numeric_limits<double>::infinity());
        const int eq_pos = alloc(a.len), eq_neg = alloc(a.len);
        for (int i = 0; i < a.len; ++i) {
          emit(Program::EQ, eq_pos + i, a.reg + i, pos);
          emit(Program::EQ, eq_neg + i, a.reg + i, neg);
          emit(Program::ADD, r + i, eq_pos + i, eq_neg + i);
        }
      } else {
        for (int i = 0; i < a.len; ++i)
          emit(c, r + i, a.reg + i, rhs < 0 ? a.reg + i : rhs);
      }
      Range out = a;
      out.reg = r;
      return typed(out, e.unsized);
    }
    Program::Code c;
    if (e.name == "PMinus__")
      c = Program::NEG;
    else if (e.name == "PPlus__")
      c = Program::MOV;
    else if (e.name == "exp")
      c = Program::EXP;
    else if (e.name == "log")
      c = Program::LOG;
    else if (e.name == "sqrt")
      c = Program::SQRT;
    else if (e.name == "square")
      c = Program::SQUARE;
    else if (e.name == "inv")
      c = Program::INV;
    else if (e.name == "fabs" || e.name == "abs")
      c = Program::FABS;
    else if (e.name == "inv_logit")
      c = Program::INV_LOGIT;
    else if (e.name == "log1p_exp")
      c = Program::LOG1P_EXP;
    else
      bail("function " + e.name);
    const int r = alloc(a.len);
    for (int i = 0; i < a.len; ++i) emit(c, r + i, a.reg + i);
    Range out = a;
    out.reg = r;
    return typed(out, e.unsized);
  }
  bail("function " + e.name);
}

void ProgramCompiler::emit_function_return(const std::vector<Range>& values,
                                           bool several) {
  if (!return_frame) bail("runtime return has no function scope");
  auto& frame = *return_frame;
  std::vector<Range> single;
  if (!several && frame.has_result) single.push_back(frame.result);
  std::vector<Range>& slots = several ? frame.results : single;
  if (!frame.has_result) {
    slots = values;
    for (Range& slot : slots) slot.reg = alloc(slot.len);
    frame.has_result = true;
  } else if (slots.size() != values.size()) {
    bail("function returns give different numbers of values");
  } else {
    for (size_t k = 0; k < values.size(); ++k)
      if (!same_view(slots[k], values[k]))
        bail("function returns have different logical views");
  }
  for (size_t k = 0; k < values.size(); ++k)
    for (int i = 0; i < values[k].len; ++i)
      emit(Program::MOV, slots[k].reg + i, values[k].reg + i);
  if (!several) frame.result = slots[0];
  frame.jumps.push_back(emit(Program::JMP, 0));
  jump_with_facts(nullptr);
}

Range ProgramCompiler::function_body(const std::vector<mir::Stmt>& body,
                                     bool void_call,
                                     std::vector<Range>* results) {
  ReturnFrame frame;
  frame.body = &body;
  frame.parent = return_frame;
  frame.loop_base = loops.size();
  struct Scope {
    ReturnFrame*& current;
    ReturnFrame* saved;
    ~Scope() { current = saved; }
  } scope{return_frame, return_frame};
  return_frame = &frame;
  const size_t start = p.code.size();
  const Path path = compile_body(body);
  if (results) *results = frame.results;
  if (path == Path::Returned) return frame.result;
  if (path == Path::Open && !void_call)
    bail("function can finish without returning a value");
  if (!frame.has_result && !void_call) bail("function returned no value");
  // Fall through at the final return instead of dispatching a no-op jump.
  // Path::Ended propagates only through all-return paths; handlers with a
  // fallthrough edge to code.size() consume it before reaching here. A
  // branch already patched to land just after the jump, as the one for
  // `if (c) return;` is, moves back with it. Only the body's own code can
  // branch there.
  if (!frame.jumps.empty() && frame.jumps.back() == (int)p.code.size() - 1) {
    const int after = (int)p.code.size();
    p.code.pop_back();
    frame.jumps.pop_back();
    for (size_t i = start; i < p.code.size(); ++i) {
      Program::Instr& I = p.code[i];
      if ((I.code == Program::JZ || I.code == Program::JMP) && I.dst == after)
        I.dst = after - 1;
    }
  }
  for (int jump : frame.jumps) p.code[(size_t)jump].dst = (int)p.code.size();
  return frame.result;
}

Range ProgramCompiler::inline_call(const mir::FunDef& f,
                                   const std::vector<InlineArg>& args,
                                   bool void_call, bool propto,
                                   std::vector<Range>* results) {
  if (args.size() != f.arg_names.size()) bail("function argument mismatch");
  if ((f.results.size() > 1) != (results != nullptr))
    bail("a call of " + f.name + " with the wrong number of results");
  if (++inline_depth > 32) {
    --inline_depth;
    bail("function inlining too deep");
  }
  // Callee scope: move the caller's bindings aside, install the
  // parameters, and move them back afterwards. Registers are never
  // reused, so nothing aliases.
  auto saved_reals = std::exchange(reals, {});
  auto saved_ints = std::exchange(ints, {});
  auto saved_known_reals = std::exchange(known_reals, {});
  auto saved_known_int_arrays = std::exchange(known_int_arrays, {});
  auto saved_known_int_array_dims = std::exchange(known_int_array_dims, {});
  auto saved_int_array_names = std::exchange(int_array_names, {});
  auto saved_deferred_shapes = std::exchange(deferred_shapes, {});
  auto saved_region_sized = std::exchange(region_sized, {});
  auto saved_unassigned = std::exchange(unassigned, {});
  auto saved_int_decl_at = std::exchange(int_decl_at, {});
  auto saved_int_reset_at = std::exchange(int_reset_at, {});
  auto saved_extern_bound = std::exchange(extern_bound, {});
  auto saved_call_types = call_types;
  const bool saved_propto_ctx = propto_ctx;
  const int saved_branch_depth = branch_depth;
  const int saved_return_while_base = return_while_base;
  const size_t saved_loop_count = loops.size();
  return_while_base = structured_while_depth;
  call_types = CallTypes{};
  call_types->locals = false;
  for (size_t k = 0; k < args.size(); ++k) {
    const bool real_autodiff = !args[k].is_const_int && args[k].autodiff;
    call_types->formals[f.arg_names[k]] = real_autodiff;
    call_types->locals = call_types->locals || real_autodiff;
  }
  propto_ctx = propto;
  branch_depth = 0;
  inline_stack.push_back(f.name);
  std::set<std::string> assigned;
  for (const auto& statement : f.body) assigned_names(statement, &assigned);
  for (size_t k = 0; k < f.arg_names.size(); ++k) {
    if (args[k].is_const_int) {
      if (args[k].int_dims.empty()) {
        if (runtime_for_depth && assigned.count(f.arg_names[k]) &&
            args[k].ints.size() == 1) {
          const int reg = konst(static_cast<double>(args[k].ints[0]));
          reals[f.arg_names[k]] = Range{reg, 1};
        } else {
          ints[f.arg_names[k]] = args[k].ints;
          if (args[k].ints.size() == 1) mark_int_reset(f.arg_names[k]);
        }
      } else {
        known_int_arrays[f.arg_names[k]] = args[k].ints;
        known_int_array_dims[f.arg_names[k]] = args[k].int_dims;
        int_array_names.insert(f.arg_names[k]);
      }
      // The body starts here, inside whatever loop the call sits in.
      int_decl_at[f.arg_names[k]] = {0, loops.size()};
    } else {
      reals[f.arg_names[k]] = args[k].real;
      if (args[k].is_const_real && !assigned.count(f.arg_names[k]))
        known_reals[f.arg_names[k]] = args[k].const_real;
    }
  }
  Range out{0, 0};
  try {
    out = function_body(f.body, void_call, results);
  } catch (...) {
    inline_stack.pop_back();
    reals = std::move(saved_reals);
    ints = std::move(saved_ints);
    known_reals = std::move(saved_known_reals);
    known_int_arrays = std::move(saved_known_int_arrays);
    known_int_array_dims = std::move(saved_known_int_array_dims);
    int_array_names = std::move(saved_int_array_names);
    deferred_shapes = std::move(saved_deferred_shapes);
    region_sized = std::move(saved_region_sized);
    unassigned = std::move(saved_unassigned);
    int_decl_at = std::move(saved_int_decl_at);
    int_reset_at = std::move(saved_int_reset_at);
    extern_bound = std::move(saved_extern_bound);
    call_types = std::move(saved_call_types);
    propto_ctx = saved_propto_ctx;
    branch_depth = saved_branch_depth;
    return_while_base = saved_return_while_base;
    loops.resize(saved_loop_count);
    --inline_depth;
    throw;
  }
  inline_stack.pop_back();
  reals = std::move(saved_reals);
  ints = std::move(saved_ints);
  known_reals = std::move(saved_known_reals);
  known_int_arrays = std::move(saved_known_int_arrays);
  known_int_array_dims = std::move(saved_known_int_array_dims);
  int_array_names = std::move(saved_int_array_names);
  deferred_shapes = std::move(saved_deferred_shapes);
  region_sized = std::move(saved_region_sized);
  unassigned = std::move(saved_unassigned);
  int_decl_at = std::move(saved_int_decl_at);
  int_reset_at = std::move(saved_int_reset_at);
  extern_bound = std::move(saved_extern_bound);
  call_types = std::move(saved_call_types);
  propto_ctx = saved_propto_ctx;
  branch_depth = saved_branch_depth;
  return_while_base = saved_return_while_base;
  loops.resize(saved_loop_count);
  --inline_depth;
  return out;
}

}  // namespace stanli
