#include <stanli/wa_interp.hpp>

#include "mir_tuples.hpp"

#include <stanli/function_registry.hpp>
#include <stanli/higher_order_eval.hpp>
#include <stanli/mir_interp.hpp>

#include <stan/math.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace stanli {

const ScalarRng* scalar_rng_family(const std::string& name) {
  // The unified FunctionSpec registry owns the name-to-family mapping; the
  // enum-keyed arity and integer-result helpers below stay the single
  // statement of each family's properties, which registration reuses.
  const FunctionSpec* spec = function_spec(name, FunctionFamily::Builtin);
  if (spec == nullptr || spec->builtin() == nullptr ||
      spec->builtin()->shape != BuiltinShapePolicy::Rng)
    return nullptr;
  return &spec->builtin()->rng;
}

size_t scalar_rng_arity(ScalarRng family) {
  switch (family) {
    case ScalarRng::StdNormal:
      return 0;
    case ScalarRng::ChiSquare:
    case ScalarRng::PoissonLog:
    case ScalarRng::Bernoulli:
    case ScalarRng::Exponential:
    case ScalarRng::Poisson:
    case ScalarRng::BernoulliLogit:
    case ScalarRng::YuleSimon:
    case ScalarRng::InvChiSquare:
    case ScalarRng::Rayleigh:
      return 1;
    case ScalarRng::Uniform:
    case ScalarRng::Normal:
    case ScalarRng::Lognormal:
    case ScalarRng::Binomial:
    case ScalarRng::Gumbel:
    case ScalarRng::Gamma:
    case ScalarRng::InvGamma:
    case ScalarRng::Beta:
    case ScalarRng::Cauchy:
    case ScalarRng::DoubleExponential:
    case ScalarRng::Logistic:
    case ScalarRng::Weibull:
    case ScalarRng::NegBinomial2:
    case ScalarRng::NegBinomial2Log:
    case ScalarRng::BetaProportion:
    case ScalarRng::DiscreteRange:
    case ScalarRng::Frechet:
    case ScalarRng::Loglogistic:
    case ScalarRng::NegBinomial:
    case ScalarRng::Pareto:
    case ScalarRng::ScaledInvChiSquare:
    case ScalarRng::VonMises:
      return 2;
    case ScalarRng::BetaBinomial:
    case ScalarRng::StudentT:
    case ScalarRng::BetaNegBinomial:
    case ScalarRng::ExpModNormal:
    case ScalarRng::ParetoType2:
    case ScalarRng::SkewDoubleExponential:
    case ScalarRng::SkewNormal:
      return 3;
  }
  throw std::logic_error("unknown scalar RNG family");
}

bool scalar_rng_is_int(ScalarRng family) {
  switch (family) {
    case ScalarRng::PoissonLog:
    case ScalarRng::Bernoulli:
    case ScalarRng::Poisson:
    case ScalarRng::BernoulliLogit:
    case ScalarRng::Binomial:
    case ScalarRng::BetaBinomial:
    case ScalarRng::NegBinomial2:
    case ScalarRng::NegBinomial2Log:
    case ScalarRng::YuleSimon:
    case ScalarRng::BetaNegBinomial:
    case ScalarRng::DiscreteRange:
    case ScalarRng::NegBinomial:
      return true;
    default:
      return false;
  }
}

unsigned scalar_rng_int_arguments(ScalarRng family) {
  switch (family) {
    case ScalarRng::Binomial:
    case ScalarRng::BetaBinomial:
      return 1;
    case ScalarRng::DiscreteRange:
      return 3;
    default:
      return 0;
  }
}

namespace {
// Only this boundary chooses C++ scalar/container overloads. All validation
// and generation remain in Stan Math, including empty-input behavior and
// validation before the first draw. Numeric graph storage stays unchanged.
template <bool Integer = false, class F>
void with_rng_argument(const RngArgument& arg, F&& f) {
  if (arg.scalar) {
    if constexpr (Integer)
      f(static_cast<int>(arg.data[0]));
    else
      f(arg.data[0]);
  } else if constexpr (Integer) {
    std::vector<int> values(arg.size);
    for (size_t i = 0; i < arg.size; ++i)
      values[i] = static_cast<int>(arg.data[i]);
    f(values);
  } else {
    f(Eigen::Map<const Eigen::VectorXd>(arg.data, arg.size));
  }
}

// Calls `call` with the first N arguments, each in its scalar or container
// form. Bit k of IntArguments makes argument k a Stan int.
template <size_t N, unsigned IntArguments = 0, size_t K = 0, class F,
          class... Bound>
void with_rng_arguments(const RngArgument* args, F&& call,
                        const Bound&... bound) {
  if constexpr (K == N) {
    call(bound...);
  } else {
    with_rng_argument<((IntArguments >> K) & 1u) != 0>(
        args[K], [&](const auto& a) {
          with_rng_arguments<N, IntArguments, K + 1>(args, call, bound..., a);
        });
  }
}

template <class T>
void copy_rng_result(const T& result, double* output, size_t size) {
  if constexpr (std::is_arithmetic_v<T>) {
    if (size != 1) throw std::logic_error("RNG output size mismatch");
    output[0] = static_cast<double>(result);
  } else {
    if (result.size() != size)
      throw std::logic_error("RNG output size mismatch");
    std::copy(result.begin(), result.end(), output);
  }
}
}  // namespace

void container_rng_draw(ScalarRng family, const RngArgument* args, size_t nargs,
                        double* output, size_t output_size, WaRng& rng) {
  if (nargs != scalar_rng_arity(family) || (nargs != 0 && args == nullptr) ||
      (output_size != 0 && output == nullptr))
    throw std::logic_error("malformed container RNG arguments");
  for (size_t k = 0; k < nargs; ++k)
    if ((args[k].scalar && args[k].size != 1) ||
        (args[k].size != 0 && args[k].data == nullptr))
      throw std::logic_error("malformed container RNG argument");
  auto& g = rng.gen();
  const auto draw = [&](const auto& result) {
    copy_rng_result(result, output, output_size);
  };
  namespace m = stan::math;
  switch (family) {
    case ScalarRng::StdNormal:
      return draw(m::std_normal_rng(g));
    case ScalarRng::ChiSquare:
      return with_rng_arguments<1>(
          args, [&](const auto&... a) { draw(m::chi_square_rng(a..., g)); });
    case ScalarRng::PoissonLog:
      return with_rng_arguments<1>(
          args, [&](const auto&... a) { draw(m::poisson_log_rng(a..., g)); });
    case ScalarRng::Bernoulli:
      return with_rng_arguments<1>(
          args, [&](const auto&... a) { draw(m::bernoulli_rng(a..., g)); });
    case ScalarRng::Exponential:
      return with_rng_arguments<1>(
          args, [&](const auto&... a) { draw(m::exponential_rng(a..., g)); });
    case ScalarRng::Poisson:
      return with_rng_arguments<1>(
          args, [&](const auto&... a) { draw(m::poisson_rng(a..., g)); });
    case ScalarRng::BernoulliLogit:
      return with_rng_arguments<1>(args, [&](const auto&... a) {
        draw(m::bernoulli_logit_rng(a..., g));
      });
    case ScalarRng::YuleSimon:
      return with_rng_arguments<1>(
          args, [&](const auto&... a) { draw(m::yule_simon_rng(a..., g)); });
    case ScalarRng::InvChiSquare:
      return with_rng_arguments<1>(args, [&](const auto&... a) {
        draw(m::inv_chi_square_rng(a..., g));
      });
    case ScalarRng::Rayleigh:
      return with_rng_arguments<1>(
          args, [&](const auto&... a) { draw(m::rayleigh_rng(a..., g)); });
    case ScalarRng::Uniform:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::uniform_rng(a..., g)); });
    case ScalarRng::Normal:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::normal_rng(a..., g)); });
    case ScalarRng::Lognormal:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::lognormal_rng(a..., g)); });
    case ScalarRng::Binomial:
      return with_rng_arguments<2, 1>(
          args, [&](const auto&... a) { draw(m::binomial_rng(a..., g)); });
    case ScalarRng::Gumbel:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::gumbel_rng(a..., g)); });
    case ScalarRng::Gamma:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::gamma_rng(a..., g)); });
    case ScalarRng::InvGamma:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::inv_gamma_rng(a..., g)); });
    case ScalarRng::Beta:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::beta_rng(a..., g)); });
    case ScalarRng::Cauchy:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::cauchy_rng(a..., g)); });
    case ScalarRng::DoubleExponential:
      return with_rng_arguments<2>(args, [&](const auto&... a) {
        draw(m::double_exponential_rng(a..., g));
      });
    case ScalarRng::Logistic:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::logistic_rng(a..., g)); });
    case ScalarRng::Weibull:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::weibull_rng(a..., g)); });
    case ScalarRng::NegBinomial2:
      return with_rng_arguments<2>(args, [&](const auto&... a) {
        draw(m::neg_binomial_2_rng(a..., g));
      });
    case ScalarRng::NegBinomial2Log:
      return with_rng_arguments<2>(args, [&](const auto&... a) {
        draw(m::neg_binomial_2_log_rng(a..., g));
      });
    case ScalarRng::BetaProportion:
      return with_rng_arguments<2>(args, [&](const auto&... a) {
        draw(m::beta_proportion_rng(a..., g));
      });
    case ScalarRng::DiscreteRange:
      return with_rng_arguments<2, 3>(args, [&](const auto&... a) {
        draw(m::discrete_range_rng(a..., g));
      });
    case ScalarRng::Frechet:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::frechet_rng(a..., g)); });
    case ScalarRng::Loglogistic:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::loglogistic_rng(a..., g)); });
    case ScalarRng::NegBinomial:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::neg_binomial_rng(a..., g)); });
    case ScalarRng::Pareto:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::pareto_rng(a..., g)); });
    case ScalarRng::ScaledInvChiSquare:
      return with_rng_arguments<2>(args, [&](const auto&... a) {
        draw(m::scaled_inv_chi_square_rng(a..., g));
      });
    case ScalarRng::VonMises:
      return with_rng_arguments<2>(
          args, [&](const auto&... a) { draw(m::von_mises_rng(a..., g)); });
    case ScalarRng::BetaBinomial:
      return with_rng_arguments<3, 1>(
          args, [&](const auto&... a) { draw(m::beta_binomial_rng(a..., g)); });
    case ScalarRng::StudentT:
      return with_rng_arguments<3>(
          args, [&](const auto&... a) { draw(m::student_t_rng(a..., g)); });
    case ScalarRng::BetaNegBinomial:
      return with_rng_arguments<3>(args, [&](const auto&... a) {
        draw(m::beta_neg_binomial_rng(a..., g));
      });
    case ScalarRng::ExpModNormal:
      return with_rng_arguments<3>(args, [&](const auto&... a) {
        draw(m::exp_mod_normal_rng(a..., g));
      });
    case ScalarRng::ParetoType2:
      return with_rng_arguments<3>(
          args, [&](const auto&... a) { draw(m::pareto_type_2_rng(a..., g)); });
    case ScalarRng::SkewDoubleExponential:
      return with_rng_arguments<3>(args, [&](const auto&... a) {
        draw(m::skew_double_exponential_rng(a..., g));
      });
    case ScalarRng::SkewNormal:
      return with_rng_arguments<3>(
          args, [&](const auto&... a) { draw(m::skew_normal_rng(a..., g)); });
  }
  throw std::logic_error("unknown scalar RNG family");
}

double scalar_rng_draw(ScalarRng family, const double* args, size_t nargs,
                       WaRng& rng) {
  if (nargs != scalar_rng_arity(family) || (nargs != 0 && args == nullptr))
    throw std::logic_error("malformed scalar RNG arguments");
  RngArgument container_args[3]{};
  for (size_t k = 0; k < nargs; ++k) container_args[k] = {args + k, 1, true};
  double result = 0.0;
  container_rng_draw(family, container_args, nargs, &result, 1, rng);
  return result;
}

int vector_integer_rng_draw(const double* probabilities, size_t size,
                            WaRng& rng, uint8_t variant) {
  if (size != 0 && probabilities == nullptr)
    throw std::logic_error("malformed categorical RNG arguments");
  Eigen::VectorXd theta(static_cast<Eigen::Index>(size));
  for (size_t i = 0; i < size; ++i)
    theta[static_cast<Eigen::Index>(i)] = probabilities[i];
  switch (variant) {
    case kCategoricalRngVariant:
      return stan::math::categorical_rng(theta, rng.gen());
    case kCategoricalLogitRngVariant:
      // Upstream indexes its cumulative sum even for an empty input. Reject
      // that invalid distribution before it consumes the stream or reads OOB.
      stan::math::check_nonzero_size("categorical_logit_rng",
                                     "Log odds parameter", theta);
      return stan::math::categorical_logit_rng(theta, rng.gen());
    case kPoissonBinomialRngVariant:
      return stan::math::poisson_binomial_rng(theta, rng.gen());
    default:
      throw std::logic_error("unknown vector-to-integer RNG family");
  }
}

void multi_normal_rng_draw(const double* location, size_t location_size,
                           const double* covariance, size_t covariance_size,
                           size_t covariance_rows, size_t covariance_cols,
                           double* output, size_t output_size, WaRng& rng,
                           bool cholesky) {
  if ((location_size != 0 && location == nullptr) ||
      (covariance_size != 0 && covariance == nullptr) ||
      (output_size != 0 && output == nullptr) || output_size != location_size ||
      (covariance_rows != 0 &&
       covariance_cols >
           std::numeric_limits<size_t>::max() / covariance_rows) ||
      covariance_rows * covariance_cols != covariance_size)
    throw std::logic_error("malformed multi-normal RNG arguments");
  // Stan Math does not check that the location has one element per row of
  // the covariance. A mismatch makes it write past the end of a buffer.
  if (covariance_rows == covariance_cols)
    stan::math::check_size_match(
        cholesky ? "multi_normal_cholesky_rng" : "multi_normal_rng",
        "Size of location parameter", location_size,
        "rows of covariance parameter", covariance_rows);

  Eigen::VectorXd mu(static_cast<Eigen::Index>(location_size));
  for (size_t i = 0; i < location_size; ++i)
    mu[static_cast<Eigen::Index>(i)] = location[i];
  Eigen::MatrixXd sigma(static_cast<Eigen::Index>(covariance_rows),
                        static_cast<Eigen::Index>(covariance_cols));
  for (size_t j = 0; j < covariance_cols; ++j)
    for (size_t i = 0; i < covariance_rows; ++i)
      sigma(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) =
          covariance[j * covariance_rows + i];

  const Eigen::VectorXd draw =
      cholesky ? stan::math::multi_normal_cholesky_rng(mu, sigma, rng.gen())
               : stan::math::multi_normal_rng(mu, sigma, rng.gen());
  for (size_t i = 0; i < output_size; ++i)
    output[i] = draw[static_cast<Eigen::Index>(i)];
}

void dirichlet_rng_draw(const double* alpha, size_t alpha_size, double* output,
                        size_t output_size, WaRng& rng) {
  if ((alpha_size != 0 && alpha == nullptr) ||
      (output_size != 0 && output == nullptr) || output_size != alpha_size)
    throw std::logic_error("malformed dirichlet RNG arguments");
  Eigen::VectorXd a(static_cast<Eigen::Index>(alpha_size));
  for (size_t i = 0; i < alpha_size; ++i)
    a[static_cast<Eigen::Index>(i)] = alpha[i];
  // Stan Math reads the first element of an empty vector.
  stan::math::check_nonzero_size("dirichlet_rng", "prior sample sizes", a);
  const Eigen::VectorXd draw = stan::math::dirichlet_rng(a, rng.gen());
  for (size_t i = 0; i < output_size; ++i)
    output[i] = draw[static_cast<Eigen::Index>(i)];
}

namespace {
constexpr auto kReal = RngArgumentKind::Real;
constexpr auto kInt = RngArgumentKind::Int;
constexpr auto kVector = RngArgumentKind::Vector;
constexpr auto kMatrix = RngArgumentKind::Matrix;

constexpr LibraryRngSpec kLibraryRngs[] = {
    {LibraryRng::Multinomial,
     "multinomial_rng",
     2,
     {kVector, kInt},
     RngResultKind::IntArray},
    {LibraryRng::MultinomialLogit,
     "multinomial_logit_rng",
     2,
     {kVector, kInt},
     RngResultKind::IntArray},
    {LibraryRng::DirichletMultinomial,
     "dirichlet_multinomial_rng",
     2,
     {kVector, kInt},
     RngResultKind::IntArray},
    {LibraryRng::HmmLatent,
     "hmm_latent_rng",
     3,
     {kMatrix, kMatrix, kVector},
     RngResultKind::IntArray},
    {LibraryRng::MultiStudentT,
     "multi_student_t_rng",
     3,
     {kReal, kVector, kMatrix},
     RngResultKind::Vector},
    {LibraryRng::MultiStudentTCholesky,
     "multi_student_t_cholesky_rng",
     3,
     {kReal, kVector, kMatrix},
     RngResultKind::Vector},
    {LibraryRng::OrderedLogistic,
     "ordered_logistic_rng",
     2,
     {kReal, kVector},
     RngResultKind::Int},
    {LibraryRng::OrderedProbit,
     "ordered_probit_rng",
     2,
     {kReal, kVector},
     RngResultKind::Int},
    {LibraryRng::Hypergeometric,
     "hypergeometric_rng",
     3,
     {kInt, kInt, kInt},
     RngResultKind::Int},
    {LibraryRng::LkjCorr,
     "lkj_corr_rng",
     2,
     {kInt, kReal},
     RngResultKind::Matrix},
    {LibraryRng::LkjCorrCholesky,
     "lkj_corr_cholesky_rng",
     2,
     {kInt, kReal},
     RngResultKind::Matrix},
    {LibraryRng::Wishart,
     "wishart_rng",
     2,
     {kReal, kMatrix},
     RngResultKind::Matrix},
    {LibraryRng::WishartCholesky,
     "wishart_cholesky_rng",
     2,
     {kReal, kMatrix},
     RngResultKind::Matrix},
    {LibraryRng::InvWishart,
     "inv_wishart_rng",
     2,
     {kReal, kMatrix},
     RngResultKind::Matrix},
    {LibraryRng::InvWishartCholesky,
     "inv_wishart_cholesky_rng",
     2,
     {kReal, kMatrix},
     RngResultKind::Matrix},
    {LibraryRng::BernoulliLogitGlm,
     "bernoulli_logit_glm_rng",
     3,
     {kMatrix, kVector, kVector},
     RngResultKind::IntArray},
};

Eigen::VectorXd rng_vector(const RngArgument& arg) {
  return Eigen::Map<const Eigen::VectorXd>(arg.data, arg.size);
}
Eigen::MatrixXd rng_matrix(const RngArgument& arg) {
  if (arg.rows * arg.cols != arg.size)
    throw std::logic_error("malformed library RNG matrix argument");
  return Eigen::Map<const Eigen::MatrixXd>(arg.data, arg.rows, arg.cols);
}
int rng_int(const RngArgument& arg) { return static_cast<int>(arg.data[0]); }

RngDraw rng_result(int value) { return {{static_cast<double>(value)}, 1, 1}; }
RngDraw rng_result(const std::vector<int>& values) {
  return {std::vector<double>(values.begin(), values.end()),
          static_cast<int64_t>(values.size()), 1};
}
RngDraw rng_result(const Eigen::MatrixXd& values) {
  return {std::vector<double>(values.data(), values.data() + values.size()),
          values.rows(), values.cols()};
}
}  // namespace

const LibraryRngSpec* library_rng_spec(const std::string& name) {
  for (const LibraryRngSpec& spec : kLibraryRngs)
    if (name == spec.name) return &spec;
  return nullptr;
}

const LibraryRngSpec* library_rng_spec(uint8_t variant) {
  for (const LibraryRngSpec& spec : kLibraryRngs)
    if (variant == static_cast<uint8_t>(spec.family)) return &spec;
  return nullptr;
}

RngDraw library_rng_draw(const LibraryRngSpec& spec, const RngArgument* args,
                         size_t nargs, WaRng& rng) {
  if (nargs != spec.arity || args == nullptr)
    throw std::logic_error("malformed library RNG arguments");
  for (size_t k = 0; k < nargs; ++k)
    if (args[k].size != 0 && args[k].data == nullptr)
      throw std::logic_error("malformed library RNG argument");
  auto& g = rng.gen();
  namespace m = stan::math;
  const RngArgument* a = args;
  switch (spec.family) {
    case LibraryRng::Multinomial:
      return rng_result(m::multinomial_rng(rng_vector(a[0]), rng_int(a[1]), g));
    case LibraryRng::MultinomialLogit:
      return rng_result(
          m::multinomial_logit_rng(rng_vector(a[0]), rng_int(a[1]), g));
    case LibraryRng::DirichletMultinomial:
      return rng_result(
          m::dirichlet_multinomial_rng(rng_vector(a[0]), rng_int(a[1]), g));
    case LibraryRng::HmmLatent: {
      // Stan Math reads the first column of log_omegas, which may be empty.
      // Its own checks come first, so they keep their messages.
      const Eigen::MatrixXd log_omegas = rng_matrix(a[0]);
      const Eigen::MatrixXd Gamma = rng_matrix(a[1]);
      const Eigen::VectorXd rho = rng_vector(a[2]);
      m::hmm_check(log_omegas, Gamma, rho, "hmm_latent_rng");
      m::check_nonzero_size("hmm_latent_rng", "log_omegas", log_omegas);
      return rng_result(m::hmm_latent_rng(log_omegas, Gamma, rho, g));
    }
    case LibraryRng::MultiStudentT:
      return rng_result(m::multi_student_t_rng(a[0].data[0], rng_vector(a[1]),
                                               rng_matrix(a[2]), g));
    case LibraryRng::MultiStudentTCholesky:
      return rng_result(m::multi_student_t_cholesky_rng(
          a[0].data[0], rng_vector(a[1]), rng_matrix(a[2]), g));
    case LibraryRng::OrderedLogistic:
      return rng_result(
          m::ordered_logistic_rng(a[0].data[0], rng_vector(a[1]), g));
    case LibraryRng::OrderedProbit:
      return rng_result(
          m::ordered_probit_rng(a[0].data[0], rng_vector(a[1]), g));
    case LibraryRng::Hypergeometric:
      return rng_result(m::hypergeometric_rng(rng_int(a[0]), rng_int(a[1]),
                                              rng_int(a[2]), g));
    // Stan Math takes the dimension as size_t and loops K - 1 times, so a
    // dimension below 1 never finishes.
    case LibraryRng::LkjCorr:
      m::check_positive(spec.name, "Dimension", rng_int(a[0]));
      return rng_result(m::lkj_corr_rng(rng_int(a[0]), a[1].data[0], g));
    case LibraryRng::LkjCorrCholesky:
      m::check_positive(spec.name, "Dimension", rng_int(a[0]));
      return rng_result(
          m::lkj_corr_cholesky_rng(rng_int(a[0]), a[1].data[0], g));
    case LibraryRng::Wishart:
      return rng_result(m::wishart_rng(a[0].data[0], rng_matrix(a[1]), g));
    case LibraryRng::WishartCholesky:
      return rng_result(
          m::wishart_cholesky_rng(a[0].data[0], rng_matrix(a[1]), g));
    case LibraryRng::InvWishart:
      return rng_result(m::inv_wishart_rng(a[0].data[0], rng_matrix(a[1]), g));
    case LibraryRng::InvWishartCholesky:
      return rng_result(
          m::inv_wishart_cholesky_rng(a[0].data[0], rng_matrix(a[1]), g));
    case LibraryRng::BernoulliLogitGlm:
      return rng_result(m::bernoulli_logit_glm_rng(
          rng_matrix(a[0]), rng_vector(a[1]), rng_vector(a[2]), g));
  }
  throw std::logic_error("unknown library RNG family");
}

double wa_probe_point(int64_t i, int variant) {
  switch (variant) {
    case 1:
      return 0.02 * static_cast<double>((i % 5) - 2);
    case 2:
      return 0.0;
    default:
      return 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
  }
}

int64_t probe_wa_columns(WaInterp& interp, const CompiledModel& cm,
                         Executor& ex, std::string& failure) {
  for (int variant = 0; variant < 3; ++variant) {
    for (int64_t i = 0; i < ex.n_params(); ++i)
      ex.params_data()[i] = wa_probe_point(i, variant);
    try {
      ex.run_forward_only();
      WaRng probe(1);
      const auto row = interp.eval(cm.constrained_env(ex), probe);
      failure.clear();
      return (int64_t)row.size();
    } catch (const std::exception& e) {
      failure = e.what();
    }
  }
  return -1;
}

WaInterp::WaInterp(std::shared_ptr<const mir::Program> prog,
                   std::map<std::string, DataMap::Entry> base_env)
    : WaInterp(std::move(prog),
               std::make_shared<const std::map<std::string, DataMap::Entry>>(
                   std::move(base_env)),
               {}) {}

WaInterp::WaInterp(
    std::shared_ptr<const mir::Program> prog,
    std::shared_ptr<const std::map<std::string, DataMap::Entry>> shared_env,
    std::map<std::string, DataMap::Entry> own_env)
    : prog_(std::move(prog)),
      shared_env_(std::move(shared_env)),
      base_env_(std::move(own_env)) {
  for (const auto& f : prog_->fun_defs) funs_[mir::function_key(f)] = &f;
}

std::vector<double> WaInterp::eval(
    const std::map<std::string, DataMap::Entry>& params, WaRng& rng) {
  if (!have_cols_) {
    // A failed discovery may have emitted a prefix. Start the next attempt
    // afresh; otherwise retries duplicate columns before their first success.
    cols_.clear();
    n_tp_start_ = n_gq_start_ = 0;
    saw_tp_ = saw_gq_ = false;
    output_cursor_ =
        mir::OutputCursor(prog_->output_vars,
                          mir::detail::whole_leaf_tuples(prog_->tuples.get()));
  }
  std::vector<double> row;
  MirInterp<double>* cur = nullptr;
  MirHooks h;
  h.stmt = [this, &cur, &params, &row](const mir::Stmt& s) {
    if (s.kind == mir::Stmt::Decl && s.read_transform)
      return read_param(*cur, s, params);
    if (mir::internal_name(s) == "FnWriteParam")
      return write_param(*cur, s, row);
    return false;
  };
  // CmdStan's write_array accumulates the target from lp__ = 0.
  double target = 0.0;
  h.fun = [this, &rng, &target](MirInterp<double>& in, const mir::Expr& e,
                                DataMap::Entry* out) {
    if (mir::stateful_intrinsic_kind(e)) {
      out->r = {target};
      return true;
    }
    return interpreted_rng_call(in, e, out, rng) ||
           evaluate_retained_higher_order(
               funs_, e, [&in](const mir::Expr& arg) { return in.eval(arg); },
               out);
  };
  MirInterp<double> in(funs_, "write_array", std::move(h));
  in.set_native_extrema(true);
  in.set_target_sink(&target);
  cur = &in;
  in.set_parent_env(shared_env_.get());
  in.env() = base_env_;
  in.run(prog_->generate_quantities);
  if (!have_cols_) {
    // A section with no columns starts where the next one does, so the two
    // boundaries cannot come out ordered backwards.
    if (!saw_gq_) n_gq_start_ = cols_.size();
    if (!saw_tp_) n_tp_start_ = n_gq_start_;
    have_cols_ = true;
  }
  return row;
}

bool WaInterp::read_param(MirInterp<double>& in, const mir::Stmt& s,
                          const std::map<std::string, DataMap::Entry>& params) {
  auto it = params.find(s.decl_id);
  if (it == params.end())
    throw CompileError(
        "no constrained value supplied "
        "for parameter " +
        s.decl_id);
  DataMap::Entry e = it->second;
  if (!s.decl_type.dims.empty()) {
    std::vector<int64_t> dims;
    int64_t len = 1;
    for (const auto& d : s.decl_type.dims) {
      dims.push_back(in.as_int(d));
      len *= dims.back();
    }
    if (len != (int64_t)std::max(e.r.size(), e.i.size()))
      throw CompileError("constrained shape mismatch for " + s.decl_id);
    e.dims = std::move(dims);
  }
  in.env()[s.decl_id] = std::move(e);
  return true;
}

bool WaInterp::write_param(MirInterp<double>& in, const mir::Stmt& s,
                           std::vector<double>& row) {
  const mir::Expr& v = s.fn_args.at(0);
  // CmdStan writes a real and an imaginary column; one real column is wrong.
  if (v.unsized.leaf == mir::UnsizedLeaf::Complex)
    throw CompileError("complex numbers are not supported yet (in output " +
                       v.name + ")");
  DataMap::Entry e = in.eval(v);
  const int64_t len = (int64_t)std::max(e.r.size(), e.i.size());
  if (!have_cols_) {
    // Arrays of containers arrive one element at a time (`theta[k]`), and
    // CmdStan names those columns outer-index-first: the index path joins
    // the column name. Same rule as the graph lowering's FnWriteParam.
    std::vector<long> ixs;
    const mir::Expr* base = &v;
    while (base->kind == mir::Expr::Indexed) {
      for (size_t k = base->args.size(); k-- > 1;) {
        if (base->args[k].kind != mir::Expr::IndexSingle)
          throw CompileError(
              "cannot name an output parameter written through a non-scalar "
              "index");
        ixs.push_back(in.as_int(base->args[k].args[0]));
      }
      base = &base->args[0];
    }
    const mir::detail::TupleLeaf* tuple =
        base->kind == mir::Expr::Var
            ? mir::detail::tuple_leaf(*prog_, base->name)
            : nullptr;
    const auto extent = [&in](const mir::Expr& dim) { return in.as_int(dim); };
    const mir::OutputVar* owner = output_cursor_.take(
        tuple                          ? tuple->base
        : base->kind == mir::Expr::Var ? base->name
                                       : std::string(),
        mir::detail::leaf_index(prog_->tuples.get(), tuple), len,
        [&](const mir::OutputVar& var) {
          if (mir::detail::is_tuple(var.type))
            return mir::detail::output_parts(prog_->tuples.get(), var, extent);
          int64_t columns = 1;
          for (const mir::Expr& dim : var.type.dims) columns *= extent(dim);
          return std::vector<int64_t>{columns};
        });
    if (owner == nullptr)
      throw CompileError("an output write matches no output variable");
    tuple = mir::detail::output_leaf(prog_->tuples.get(), *owner, tuple,
                                     output_cursor_.part());
    if (!tuple && mir::detail::is_tuple(owner->type))
      throw CompileError("cannot name a write of part of tuple " + owner->name);
    if (mir::is_complex(owner->type))
      throw CompileError("complex numbers are not supported yet (in output " +
                         owner->name + ")");
    if (owner->block != mir::OutputVar::Block::Parameters && !saw_tp_) {
      n_tp_start_ = cols_.size();
      saw_tp_ = true;
    }
    if (owner->block == mir::OutputVar::Block::GeneratedQuantities &&
        !saw_gq_) {
      n_gq_start_ = cols_.size();
      saw_gq_ = true;
    }
    auto named = mir::detail::output_name(
        owner->name, tuple, std::vector<int64_t>(ixs.rbegin(), ixs.rend()));
    if (!named)
      throw CompileError("cannot name a write of a whole array of tuples");
    using Naming = CompiledModel::ParamView::Naming;
    CompiledModel::ParamView pv{std::move(named->name), (int)row.size(), len};
    pv.variable = std::move(named->variable);
    pv.index = std::move(named->index);
    if (mir::language_scalar(v) ||
        mir::is_unsized(v, mir::UnsizedLeaf::Complex)) {
      pv.naming = Naming::Scalar;
    } else if (mir::is_unsized(v, mir::UnsizedLeaf::Matrix)) {
      pv.naming = Naming::Matrix;
      pv.rows = e.dims.size() == 2 ? e.dims[0] : len;
    } else {
      pv.naming = Naming::Container;
    }
    cols_.push_back(pv);
  }
  for (int64_t k = 0; k < len; ++k)
    row.push_back(k < (int64_t)e.r.size() ? e.r[(size_t)k]
                                          : (double)e.i[(size_t)k]);
  return true;
}

namespace {
// A library RNG call in the interpreter, with the same argument rules as the
// graph lowering: the containers are single vectors and matrices, and a row
// vector design matrix is left unsupported.
bool interpreted_library_rng(const LibraryRngSpec& spec, const mir::Expr& e,
                             const std::vector<DataMap::Entry>& av,
                             DataMap::Entry* out, WaRng& rng) {
  if (av.size() != spec.arity) return false;
  RngArgument args[3]{};
  for (size_t k = 0; k < spec.arity; ++k) {
    const mir::UnsizedView& type = e.args[k].unsized;
    if (type.depth != 0) return false;
    const DataMap::Entry& v = av[k];
    args[k] = {v.r.data(), v.r.size(),
               spec.args[k] == RngArgumentKind::Real ||
                   spec.args[k] == RngArgumentKind::Int};
    if (spec.args[k] == RngArgumentKind::Matrix) {
      if (type.leaf != mir::UnsizedLeaf::Matrix || v.dims.size() != 2)
        return false;
      args[k].rows = static_cast<size_t>(v.dims[0]);
      args[k].cols = static_cast<size_t>(v.dims[1]);
    }
  }
  RngDraw draw = library_rng_draw(spec, args, spec.arity, rng);
  out->r = std::move(draw.values);
  out->is_int = spec.result == RngResultKind::Int ||
                spec.result == RngResultKind::IntArray;
  out->dims.clear();
  if (spec.result == RngResultKind::Matrix)
    out->dims = {draw.rows, draw.cols};
  else if (spec.result != RngResultKind::Int)
    out->dims = {static_cast<int64_t>(out->r.size())};
  out->i.clear();
  if (out->is_int)
    for (double x : out->r) out->i.push_back(static_cast<int>(x));
  return true;
}
}  // namespace

bool interpreted_rng_call(MirInterp<double>& in, const mir::Expr& e,
                          DataMap::Entry* out, WaRng& rng) {
  stan::rng_t& g = rng.gen();
  const std::string& f = e.name;
  if (e.kind != mir::Expr::FunApp || e.fn_lib != mir::Expr::Lib::StanLib ||
      e.fn_suffix != mir::FnSuffix::Rng || f.size() < 5)
    return false;
  const std::string base = f.substr(0, f.size() - 4);

  std::vector<DataMap::Entry> av;
  for (const auto& a : e.args) av.push_back(in.eval(a));

  // Vector-valued draw from a mean vector and covariance (or Cholesky
  // factor) matrix. Admitted shapes share their owning-Eigen helper with
  // OP_RNG so validation and engine schedules cannot drift between modes.
  if (base == "multi_normal" || base == "multi_normal_cholesky") {
    const auto& mu = av.at(0);
    const auto& S = av.at(1);
    const int64_t K = (int64_t)mu.r.size();
    out->dims = {K};
    out->r.resize(static_cast<size_t>(K));
    if (base == "multi_normal" ||
        (S.dims.size() == 2 && S.dims[0] == K && S.dims[1] == K)) {
      if (S.dims.size() != 2)
        throw std::logic_error("malformed multi-normal covariance shape");
      multi_normal_rng_draw(
          mu.r.data(), mu.r.size(), S.r.data(), S.r.size(),
          static_cast<size_t>(S.dims[0]), static_cast<size_t>(S.dims[1]),
          out->r.data(), out->r.size(), rng, base == "multi_normal_cholesky");
    } else {
      // Preserve the legacy adapter outside the square-vector contract.
      // Rectangular and vectorized shapes need a separate shape migration.
      Eigen::VectorXd m(K);
      for (int64_t i = 0; i < K; ++i) m[i] = mu.r[(size_t)i];
      Eigen::MatrixXd sig(K, K);
      for (int64_t j = 0; j < K; ++j)
        for (int64_t i = 0; i < K; ++i) sig(i, j) = S.r.at((size_t)(j * K + i));
      const Eigen::VectorXd draw =
          stan::math::multi_normal_cholesky_rng(m, sig, g);
      for (int64_t i = 0; i < K; ++i) out->r[static_cast<size_t>(i)] = draw[i];
    }
    return true;
  }

  // Whole-vector concentration argument, one simplex draw.
  if (base == "dirichlet") {
    const auto& alpha = av.at(0);
    const int64_t K = (int64_t)alpha.r.size();
    out->dims = {K};
    out->r.resize(static_cast<size_t>(K));
    dirichlet_rng_draw(alpha.r.data(), alpha.r.size(), out->r.data(),
                       out->r.size(), rng);
    return true;
  }

  // Whole-vector arguments with one integer draw. Use the same owning
  // Eigen instantiation and validation in every execution path.
  if (base == "poisson_binomial" || base == "categorical" ||
      base == "categorical_logit") {
    const uint8_t variant =
        base == "poisson_binomial"    ? kPoissonBinomialRngVariant
        : base == "categorical_logit" ? kCategoricalLogitRngVariant
                                      : kCategoricalRngVariant;
    const int k = vector_integer_rng_draw(av.at(0).r.data(), av.at(0).r.size(),
                                          rng, variant);
    out->is_int = true;
    out->i = {k};
    out->r = {static_cast<double>(k)};
    return true;
  }

  if (const LibraryRngSpec* spec = library_rng_spec(f))
    return interpreted_library_rng(*spec, e, av, out, rng);

  const ScalarRng* family = scalar_rng_family(f);
  if (!family) return false;
  const size_t arity = scalar_rng_arity(*family);
  if (av.size() != arity)
    throw std::logic_error("malformed scalar RNG arguments");

  RngArgument args[3]{};
  bool container = false;
  size_t n = 1;
  for (size_t k = 0; k < arity; ++k) {
    const auto& type = e.args[k].unsized;
    const bool scalar =
        type.depth == 0 && (type.leaf == mir::UnsizedLeaf::Real ||
                            type.leaf == mir::UnsizedLeaf::Int);
    args[k] = {av[k].r.data(), av[k].r.size(), scalar};
    if (!scalar && !container) n = av[k].r.size();
    container = container || !scalar;
  }
  out->r.resize(n);
  out->is_int = scalar_rng_is_int(*family);
  if (container) {
    container_rng_draw(*family, args, arity, out->r.data(), n, rng);
    out->dims = {static_cast<int64_t>(n)};
  } else {
    double values[3]{};
    for (size_t k = 0; k < arity; ++k) values[k] = av[k].r.at(0);
    out->r[0] = scalar_rng_draw(*family, values, arity, rng);
  }
  if (out->is_int) {
    out->i.resize(n);
    for (size_t k = 0; k < n; ++k) out->i[k] = static_cast<int>(out->r[k]);
  }
  return true;
}

}  // namespace stanli
