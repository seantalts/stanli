// Runtime checks, reject(), and print(): statements whose whole purpose is a
// side effect, so they are ops rather than anything the graph optimizes.
//
// `reject` has real semantics. It throws std::domain_error, which the
// sampler treats as a rejected proposal exactly as CmdStan's generated
// code does -- the same exception type, from the same place in the
// evaluation. `print` has none; it hands the line to the message sink
// (stdout unless the host installed something else) and returns.
//
// Neither has a backward. `reject` never reaches one (the forward threw)
// and `print` contributes nothing to the target, so the executor's
// reverse sweep skips both.
//
// The message is a template: a list of literal chunks interleaved with
// the runtime values of the op's inputs, assembled at forward time
// because that is the only time the values exist. CmdStan formats a
// vector as `[1,2,3]` and a scalar bare, and so does this.
#include <stanli/graph.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/message.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>
#include <stanli/program.hpp>
#include <stanli/structured_check.hpp>

#include <stan/math/prim/err/check_cholesky_factor.hpp>
#include <stan/math/prim/err/check_cholesky_factor_corr.hpp>
#include <stan/math/prim/err/check_corr_matrix.hpp>
#include <stan/math/prim/err/check_cov_matrix.hpp>
#include <stan/math/prim/err/check_greater_or_equal.hpp>
#include <stan/math/prim/err/check_less_or_equal.hpp>
#include <stan/math/prim/err/check_ordered.hpp>
#include <stan/math/prim/err/check_positive_ordered.hpp>
#include <stan/math/prim/err/check_simplex.hpp>
#include <stan/math/prim/err/check_sum_to_zero.hpp>
#include <stan/math/prim/err/check_unit_vector.hpp>
#include <stan/math/prim/prob/categorical_logit_lpmf.hpp>
#include <stan/math/prim/prob/categorical_lpmf.hpp>
#include <stan/math/rev.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace stanli {

void throw_message(MessageError error, const std::string& message) {
  switch (error) {
    case MessageError::Runtime:
      throw std::runtime_error(message);
    case MessageError::InvalidArgument:
      throw std::invalid_argument(message);
    case MessageError::OutOfRange:
      throw std::out_of_range(message);
    case MessageError::Domain:
      break;
  }
  throw std::domain_error(message);
}

void execute_message(MessageAction action, const std::string& message) {
  if (action != MessageAction::Print)
    throw_message(message_error(action), message);
  emit_message(message);
}

namespace {

int64_t checked_product(const std::vector<int64_t>& dims, size_t first,
                        size_t last) {
  int64_t n = 1;
  for (size_t d = first; d < last; ++d) {
    const int64_t extent = dims[d];
    if (extent < 0 ||
        (extent != 0 && n > std::numeric_limits<int64_t>::max() / extent))
      throw std::logic_error("invalid structured-check geometry");
    n *= extent;
  }
  return n;
}

void check_leaf(const double* values, int64_t rows, int64_t cols,
                const std::string& name, const StructuredCheckSpec& spec) {
  Eigen::Map<const Eigen::VectorXd> vector(values, (Eigen::Index)rows);
  Eigen::Map<const Eigen::MatrixXd> matrix(values, (Eigen::Index)rows,
                                           (Eigen::Index)cols);
  constexpr const char* function = "";
  switch (spec.kind) {
    case mir::Transform::Simplex:
      stan::math::check_simplex(function, name.c_str(), vector);
      return;
    case mir::Transform::Ordered:
      stan::math::check_ordered(function, name.c_str(), vector);
      return;
    case mir::Transform::PositiveOrdered:
      stan::math::check_positive_ordered(function, name.c_str(), vector);
      return;
    case mir::Transform::UnitVector:
      stan::math::check_unit_vector(function, name.c_str(), vector);
      return;
    case mir::Transform::SumToZero:
      if (spec.leaf == StructuredLeaf::Matrix)
        stan::math::check_sum_to_zero(function, name.c_str(), matrix);
      else
        stan::math::check_sum_to_zero(function, name.c_str(), vector);
      return;
    case mir::Transform::CholeskyCorr:
      stan::math::check_cholesky_factor_corr(function, name.c_str(), matrix);
      return;
    case mir::Transform::Correlation:
      stan::math::check_corr_matrix(function, name.c_str(), matrix);
      return;
    case mir::Transform::Covariance:
      stan::math::check_cov_matrix(function, name.c_str(), matrix);
      return;
    case mir::Transform::CholeskyCov:
      stan::math::check_cholesky_factor(function, name.c_str(), matrix);
      return;
    case mir::Transform::StochasticColumn:
      stan::math::check_stochastic_column(function, name.c_str(), matrix);
      return;
    case mir::Transform::StochasticRow:
      stan::math::check_stochastic_row(function, name.c_str(), matrix);
      return;
    default:
      throw std::logic_error("unsupported structured-check kind");
  }
}

}  // namespace

void check_structured_value(const double* values, int64_t len,
                            const StructuredCheckSpec& spec) {
  const size_t leaf_rank = spec.leaf == StructuredLeaf::Matrix ? 2 : 1;
  if (spec.dims.size() < leaf_rank || len < 0 ||
      (len != 0 && values == nullptr))
    throw std::logic_error("malformed structured-check value");
  if ((mir::is_matrix_only_check(spec.kind) &&
       spec.leaf != StructuredLeaf::Matrix) ||
      (!mir::is_matrix_only_check(spec.kind) &&
       spec.kind != mir::Transform::SumToZero &&
       spec.leaf != StructuredLeaf::Vector))
    throw std::logic_error("structured-check leaf type mismatch");

  const size_t outer_rank = spec.dims.size() - leaf_rank;
  const int64_t batch = checked_product(spec.dims, 0, outer_rank);
  const int64_t leaf_len =
      checked_product(spec.dims, outer_rank, spec.dims.size());
  if ((batch != 0 && leaf_len > std::numeric_limits<int64_t>::max() / batch) ||
      batch * leaf_len != len)
    throw std::logic_error("structured-check width does not match geometry");
  if (batch == 0) return;

  const int64_t rows = spec.dims[outer_rank];
  const int64_t cols =
      spec.leaf == StructuredLeaf::Matrix ? spec.dims[outer_rank + 1] : 1;
  const double zero = 0.0;
  std::vector<double> gathered;
  if (leaf_len != 0 && spec.storage == StructuredStorage::FirstIndexFast)
    gathered.resize((size_t)leaf_len);
  std::vector<int64_t> index(outer_rank);
  for (int64_t leaf = 0; leaf < batch; ++leaf) {
    int64_t rem = leaf;
    for (size_t d = outer_rank; d-- > 0;) {
      index[d] = rem % spec.dims[d];
      rem /= spec.dims[d];
    }

    const double* one = &zero;
    if (leaf_len != 0 && spec.storage == StructuredStorage::ContiguousLeaves) {
      one = values + leaf * leaf_len;
    } else if (leaf_len != 0) {
      int64_t outer_serial = 0;
      int64_t stride = 1;
      for (size_t d = 0; d < outer_rank; ++d) {
        outer_serial += index[d] * stride;
        stride *= spec.dims[d];
      }
      for (int64_t e = 0; e < leaf_len; ++e)
        gathered[(size_t)e] = values[outer_serial + batch * e];
      one = gathered.data();
    }

    std::string name = spec.name;
    // Stan Math's corr/cov std::vector overloads intentionally retain the
    // base name; the other validators append one index at every array level.
    if (spec.kind != mir::Transform::Correlation &&
        spec.kind != mir::Transform::Covariance)
      for (int64_t i : index) name += "[" + std::to_string(i + 1) + "]";
    without_check_prefix([&] { check_leaf(one, rows, cols, name, spec); });
  }
}

void check_bound_value(const double* values, int64_t len, const double* bound,
                       int64_t bound_len, const std::string& name,
                       const CheckGeometry& geometry, bool lower) {
  const bool scalar_bound = bound_len == 1;
  bool all_pass = true;
  if (lower)
    for (int64_t i = 0; i < len; ++i)
      all_pass &= values[i] >= bound[scalar_bound ? 0 : i];
  else
    for (int64_t i = 0; i < len; ++i)
      all_pass &= values[i] <= bound[scalar_bound ? 0 : i];
  if (all_pass) return;
  const auto check = [&](const std::string& element_name, const auto& y,
                         const auto& b) {
    without_check_prefix([&] {
      if (lower)
        stan::math::check_greater_or_equal("", element_name.c_str(), y, b);
      else
        stan::math::check_less_or_equal("", element_name.c_str(), y, b);
    });
  };
  if (geometry.dims.empty() || geometry.leaf_rank == 0) {
    for (int64_t i = 0; i < len; ++i)
      check(name, values[i], bound[scalar_bound ? 0 : i]);
    return;
  }

  const std::vector<int64_t>& dims = geometry.dims;
  const size_t outer_rank = dims.size() - geometry.leaf_rank;
  const int64_t batch = checked_product(dims, 0, outer_rank);
  const int64_t leaf_len = checked_product(dims, outer_rank, dims.size());
  if (batch * leaf_len != len || (!scalar_bound && bound_len != len))
    throw std::logic_error("bound-check width does not match geometry");
  if (len == 0) return;

  std::vector<double> y_leaf((size_t)leaf_len), b_leaf((size_t)leaf_len);
  std::vector<int64_t> index(outer_rank);
  const auto gather = [&](const double* source, int64_t leaf,
                          std::vector<double>& out) {
    if (geometry.storage == StructuredStorage::ContiguousLeaves) {
      std::copy(source + leaf * leaf_len, source + (leaf + 1) * leaf_len,
                out.begin());
      return;
    }
    int64_t outer_serial = 0, stride = 1;
    for (size_t d = 0; d < outer_rank; ++d) {
      outer_serial += index[d] * stride;
      stride *= dims[d];
    }
    for (int64_t e = 0; e < leaf_len; ++e)
      out[(size_t)e] = source[outer_serial + batch * e];
  };
  const int64_t rows = dims[outer_rank];
  const int64_t cols = geometry.leaf_rank == 2 ? dims[outer_rank + 1] : 1;
  for (int64_t leaf = 0; leaf < batch; ++leaf) {
    int64_t rem = leaf;
    for (size_t d = outer_rank; d-- > 0;) {
      index[d] = rem % dims[d];
      rem /= dims[d];
    }
    gather(values, leaf, y_leaf);
    if (!scalar_bound) gather(bound, leaf, b_leaf);
    std::string element_name = name;
    for (size_t d = 0; d < outer_rank; ++d)
      element_name += (d == 0 ? "[" : ", ") + std::to_string(index[d] + 1);
    if (outer_rank > 0) element_name += "]";
    using Vec = Eigen::Map<const Eigen::VectorXd>;
    using Mat = Eigen::Map<const Eigen::MatrixXd>;
    const auto with_bound = [&](const auto& y, auto make) {
      if (scalar_bound)
        check(element_name, y, bound[0]);
      else
        check(element_name, y, make(b_leaf.data()));
    };
    if (geometry.leaf_rank == 2)
      with_bound(Mat(y_leaf.data(), rows, cols),
                 [&](const double* p) { return Mat(p, rows, cols); });
    else
      with_bound(Vec(y_leaf.data(), rows),
                 [&](const double* p) { return Vec(p, rows); });
  }
}

namespace {

// Build the message: chunk 0, then input 0, then chunk 1, then input 1,
// ... Trailing chunks with no matching input are appended as-is, which is
// what a call ending in a string literal produces.
std::string render(const KernelCtx& ctx) {
  const auto* msg = static_cast<const MessageSpec*>(ctx.udata);
  if (msg == nullptr) throw std::logic_error("message op has no template");
  return render_message(
      *msg, static_cast<size_t>(ctx.n_in),
      [&](size_t k) -> const MessageShape& { return msg->shapes.at(k); },
      [&](size_t k, int64_t i) { return ctx.in[k].data[i]; });
}

void reject_fwd(KernelCtx& ctx) {
  // std::domain_error, not a stanli-specific type: this is the exception
  // stan-math's own reject throws, and it is what the executor's callers
  // and the sampler already treat as "this draw is not valid" rather than
  // as a failure of the run.
  throw_message(static_cast<const MessageSpec*>(ctx.udata)->error, render(ctx));
}

void print_fwd(KernelCtx& ctx) {
  execute_message(MessageAction::Print, render(ctx));
}

void check_structured_fwd(KernelCtx& ctx) {
  if (ctx.n_in != 1 || ctx.out.len != 1 || ctx.udata == nullptr)
    throw std::logic_error("malformed structured-check op");
  check_structured_value(ctx.in[0].data, ctx.in[0].len,
                         *static_cast<const StructuredCheckSpec*>(ctx.udata));
  ctx.out.data[0] = 0.0;
}

void check_matching_dims_fwd(KernelCtx& ctx) {
  if (ctx.n_in != 2 || ctx.out.len != 1 || ctx.udata == nullptr)
    throw std::logic_error("malformed dimension-check op");
  const auto* spec = static_cast<const BoundCheckSpec*>(ctx.udata);
  if (!spec->shapes_match)
    throw std::invalid_argument("constraint shapes do not match for " +
                                spec->name);
  ctx.out.data[0] = 0.0;
}

template <bool Lower>
void check_fwd(KernelCtx& ctx) {
  if (ctx.n_in != 2 || ctx.out.len != 1 || ctx.udata == nullptr)
    throw std::logic_error("malformed bound-check op");
  const Desc& value = ctx.in[0];
  const Desc& bound = ctx.in[1];
  const auto* spec = static_cast<const BoundCheckSpec*>(ctx.udata);
  if (!spec->shapes_match)
    throw std::invalid_argument("constraint shapes do not match for " +
                                spec->name);
  if ((spec->bound_is_scalar && bound.len != 1) ||
      (!spec->bound_is_scalar && bound.len != value.len))
    throw std::logic_error("bound-check shape metadata is inconsistent");
  check_bound_value(value.data, value.len, bound.data, bound.len, spec->name,
                    spec->geometry, Lower);
  ctx.out.data[0] = 0.0;
}

int categorical_outcome(double value) {
  if (!std::isfinite(value) || std::trunc(value) != value ||
      value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max())
    throw std::logic_error("malformed integer categorical outcome");
  return static_cast<int>(value);
}

std::vector<int> categorical_outcomes(const KernelCtx& ctx) {
  if ((ctx.variant & kCategoricalScalarOutcome) && ctx.in[0].len != 1)
    throw std::logic_error("categorical scalar outcome has wrong width");
  std::vector<int> outcomes;
  outcomes.reserve((size_t)ctx.in[0].len);
  for (int64_t i = 0; i < ctx.in[0].len; ++i)
    outcomes.push_back(categorical_outcome(ctx.in[0].data[i]));
  return outcomes;
}

template <typename Arg>
auto categorical_eval(uint8_t variant, const std::vector<int>& outcomes,
                      const Arg& arg) {
  const bool propto = (variant & 0x80u) != 0;
  if (variant & kCategoricalScalarOutcome) {
    if (variant & kCategoricalLogit)
      return propto
                 ? stan::math::categorical_logit_lpmf<true>(outcomes[0], arg)
                 : stan::math::categorical_logit_lpmf<false>(outcomes[0], arg);
    return propto ? stan::math::categorical_lpmf<true>(outcomes[0], arg)
                  : stan::math::categorical_lpmf<false>(outcomes[0], arg);
  }
  if (variant & kCategoricalLogit)
    return propto ? stan::math::categorical_logit_lpmf<true>(outcomes, arg)
                  : stan::math::categorical_logit_lpmf<false>(outcomes, arg);
  return propto ? stan::math::categorical_lpmf<true>(outcomes, arg)
                : stan::math::categorical_lpmf<false>(outcomes, arg);
}

Eigen::Matrix<stan::math::var, -1, 1> categorical_vars(const Desc& input) {
  Eigen::Matrix<stan::math::var, -1, 1> arg(input.len);
  for (int64_t k = 0; k < input.len; ++k) arg(k) = input.data[k];
  return arg;
}

// The scalar-outcome probability form is just log(theta[n - 1]), with one
// selected reciprocal in reverse.  Its generic implementation used to build
// and tear down a nested var tape in both sweeps.  Keep Stan Math's double
// overload responsible for the value and every check, then write the exact
// scalar rev rule directly below.  Array outcomes deliberately stay on the
// replay: repeated selections share log nodes, and replacing that tape with
// counts would regroup low bits (pinned in test_lower.cpp).
bool native_scalar_probability(uint8_t variant) {
  return !(variant & kCategoricalLogit) &&
         (variant & kCategoricalScalarOutcome) &&
         (variant & kCategoricalArgAutodiff);
}

bool native_scalar_logit(uint8_t variant) {
  return (variant & kCategoricalLogit) &&
         (variant & kCategoricalScalarOutcome) &&
         (variant & kCategoricalArgAutodiff);
}

int categorical_scalar_outcome(const KernelCtx& ctx) {
  if (ctx.in[0].len != 1)
    throw std::logic_error("categorical scalar outcome has wrong width");
  return categorical_outcome(ctx.in[0].data[0]);
}

void categorical_fwd(KernelCtx& ctx) {
  if (ctx.n_in != 2 || ctx.out.len != 1)
    throw std::logic_error("malformed categorical op");
  // forward_value_only intentionally instantiates the expression on doubles:
  // a propto call whose source type was var therefore returns its dropped
  // zero in that mode.  Preserve that existing contract and use the native
  // active-type path only for a normal forward/gradient evaluation.
  if (native_scalar_probability(ctx.variant) && !values_only()) {
    const int outcome = categorical_scalar_outcome(ctx);
    const Eigen::Map<const Eigen::VectorXd> arg(ctx.in[1].data, ctx.in[1].len);
    // With an active argument, both <true> and <false> retain this summand;
    // the double <false> body is the same value/check order without a tape.
    ctx.out.data[0] = stan::math::categorical_lpmf<false>(outcome, arg);
    return;
  }
  if (native_scalar_logit(ctx.variant) && !values_only()) {
    const int outcome = categorical_scalar_outcome(ctx);
    const Eigen::Map<const Eigen::VectorXd> arg(ctx.in[1].data, ctx.in[1].len);
    ctx.out.data[0] = stan::math::categorical_logit_lpmf<false>(outcome, arg);
    return;
  }
  const std::vector<int> outcomes = categorical_outcomes(ctx);
  if ((ctx.variant & kCategoricalArgAutodiff) && !values_only()) {
    stan::math::nested_rev_autodiff nested;
    const auto arg = categorical_vars(ctx.in[1]);
    ctx.out.data[0] = categorical_eval(ctx.variant, outcomes, arg).val();
  } else {
    Eigen::Map<const Eigen::VectorXd> arg(ctx.in[1].data, ctx.in[1].len);
    ctx.out.data[0] = categorical_eval(ctx.variant, outcomes, arg);
  }
}

void categorical_bwd(KernelCtx& ctx) {
  if (!(ctx.variant & kCategoricalArgAutodiff) ||
      ctx.in_adj[1].data == nullptr ||
      (!(ctx.variant & kCategoricalScalarOutcome) && ctx.in[0].len == 0))
    return;
  if (native_scalar_probability(ctx.variant)) {
    const int outcome = categorical_scalar_outcome(ctx);
    ctx.in_adj[1].data[outcome - 1] +=
        ctx.out_adj / ctx.in[1].data[outcome - 1];
    return;
  }
  if (native_scalar_logit(ctx.variant)) {
    const int outcome = categorical_scalar_outcome(ctx);
    const Eigen::Map<const Eigen::VectorXd> arg(ctx.in[1].data, ctx.in[1].len);
    static constexpr const char* function = "categorical_logit_lpmf";
    stan::math::check_bounded(function, "categorical outcome out of support",
                              outcome, 1, arg.size());
    stan::math::check_finite(function, "log odds parameter", arg);
    const double normalizer = stan::math::log_sum_exp(arg);
    // Mirror categorical_logit_lpmf's subtraction followed by the scalar
    // AoS-var log_sum_exp callback (rev/fun/log_sum_exp.hpp). In particular,
    // update the selected logit first, and form the negative seed with -=
    // from zero. Packet exp changes low bits and must not replace std::exp.
    ctx.in_adj[1].data[outcome - 1] += ctx.out_adj;
    double normalizer_adj = 0.0;
    normalizer_adj -= ctx.out_adj;
    for (int64_t i = 0; i < arg.size(); ++i)
      ctx.in_adj[1].data[i] += normalizer_adj * std::exp(arg(i) - normalizer);
    return;
  }
  stan::math::nested_rev_autodiff nested;
  auto arg = categorical_vars(ctx.in[1]);
  for (int64_t k = 0; k < ctx.in[1].len; ++k)
    arg(k).adj() = ctx.in_adj[1].data[k];
  const auto outcomes = categorical_outcomes(ctx);
  const stan::math::var lp = categorical_eval(ctx.variant, outcomes, arg);
  stan::math::grad((lp * ctx.out_adj).vi_);
  for (int64_t k = 0; k < ctx.in[1].len; ++k)
    ctx.in_adj[1].data[k] = arg(k).adj();
}

void all_integer_density_fwd(KernelCtx& ctx) {
  const auto density =
      static_cast<AllIntegerDensity>(ctx.variant & uint8_t{0x7f});
  ctx.out.data[0] = evaluate_packed_all_integer_density(
      density, ctx.idata, ctx.n_idata, (ctx.variant & 0x80u) != 0);
}

void all_integer_density_bwd(KernelCtx&) {}

}  // namespace

void register_message_kernels() {
  register_kernel(OP_CHECK_STRUCTURED,
                  Kernel{check_structured_fwd, nullptr, nullptr});
  register_kernel(OP_CHECK_MATCHING_DIMS,
                  Kernel{check_matching_dims_fwd, nullptr, nullptr});
  register_kernel(OP_CHECK_LOWER, Kernel{check_fwd<true>, nullptr, nullptr});
  register_kernel(OP_CHECK_UPPER, Kernel{check_fwd<false>, nullptr, nullptr});
  register_kernel(OP_CATEGORICAL,
                  Kernel{categorical_fwd, categorical_bwd, nullptr});
  register_kernel(
      OP_ALL_INTEGER_DENSITY,
      Kernel{all_integer_density_fwd, all_integer_density_bwd, nullptr});
  register_kernel(OP_REJECT, Kernel{reject_fwd, nullptr, nullptr});
  register_kernel(OP_PRINT, Kernel{print_fwd, nullptr, nullptr});
}

}  // namespace stanli
