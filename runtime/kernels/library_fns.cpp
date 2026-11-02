#include <stanli/builtin_registry.hpp>
#include <stanli/graph.hpp>
#include <stanli/legacy.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>

#include <stan/math.hpp>

#include <type_traits>
#include <vector>

namespace stanli {
void register_kernel(uint16_t opcode, Kernel k);

namespace {

using stan::math::var;

template <typename S>
using Vec = Eigen::Matrix<S, -1, 1>;
template <typename S>
using Mat = Eigen::Matrix<S, -1, -1>;
template <typename S>
using MapMat = Eigen::Map<Mat<S>>;

template <typename S>
std::vector<Vec<S>> load_inputs(const KernelCtx& ctx) {
  std::vector<Vec<S>> xs(ctx.n_in);
  for (int k = 0; k < ctx.n_in; ++k) {
    xs[k].resize(ctx.in[k].len);
    for (int64_t i = 0; i < ctx.in[k].len; ++i) xs[k](i) = ctx.in[k].data[i];
  }
  return xs;
}

template <typename R>
void store_output(KernelCtx& ctx, const R& result) {
  if constexpr (std::is_arithmetic_v<R> || std::is_same_v<R, var>) {
    ctx.out.data[0] = stan::math::value_of(result);
  } else {
    const Eigen::MatrixXd v = stan::math::value_of(result);
    for (Eigen::Index c = 0, k = 0; c < v.cols(); ++c)
      for (Eigen::Index r = 0; r < v.rows(); ++r, ++k)
        ctx.out.data[k] = v(r, c);
  }
}

template <typename Op>
void library_fwd(KernelCtx& ctx) {
  if ((ctx.variant & kMatrixActiveForward) && !values_only()) {
    stan::math::nested_rev_autodiff nested;
    auto xs = load_inputs<var>(ctx);
    store_output(ctx, Op::template call<var>(ctx, xs));
    return;
  }
  auto xs = load_inputs<double>(ctx);
  store_output(ctx, Op::template call<double>(ctx, xs));
}

template <typename Op>
void library_bwd(KernelCtx& ctx) {
  nary_bwd(ctx, [&ctx](auto& xs) { return Op::template call<var>(ctx, xs); });
}

template <typename Op>
void register_library(uint16_t opcode) {
  register_kernel(opcode, Kernel{library_fwd<Op>, library_bwd<Op>, nullptr});
}

template <typename S>
MapMat<S> matrix_at(std::vector<Vec<S>>& xs, size_t k, int64_t rows,
                    int64_t cols) {
  return MapMat<S>(xs[k].data(), rows, cols);
}

struct DeterminantOp {
  template <typename S>
  static S call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t n = ctx.idata[0];
    return stan::math::determinant(matrix_at(xs, 0, n, n));
  }
};
struct LogDeterminantSpdOp {
  template <typename S>
  static S call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t n = ctx.idata[0];
    return stan::math::log_determinant_spd(matrix_at(xs, 0, n, n));
  }
};
struct Chol2invOp {
  template <typename S>
  static Mat<S> call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t n = ctx.idata[0];
    return stan::math::chol2inv(matrix_at(xs, 0, n, n));
  }
};
struct GeneralizedInverseOp {
  template <typename S>
  static auto call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::generalized_inverse(
        matrix_at(xs, 0, ctx.idata[0], ctx.idata[1]));
  }
};
struct QrQOp {
  template <typename S>
  static Mat<S> call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::qr_Q(matrix_at(xs, 0, ctx.idata[0], ctx.idata[1]));
  }
};
struct QrROp {
  template <typename S>
  static Mat<S> call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::qr_R(matrix_at(xs, 0, ctx.idata[0], ctx.idata[1]));
  }
};
struct QrThinQOp {
  template <typename S>
  static Mat<S> call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::qr_thin_Q(matrix_at(xs, 0, ctx.idata[0], ctx.idata[1]));
  }
};
struct QrThinROp {
  template <typename S>
  static Mat<S> call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::qr_thin_R(matrix_at(xs, 0, ctx.idata[0], ctx.idata[1]));
  }
};
struct MatrixPowerOp {
  template <typename S>
  static auto call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t n = ctx.idata[0];
    const int power = static_cast<int>(stan::math::value_of(xs[1](0)));
    return stan::math::matrix_power(matrix_at(xs, 0, n, n), power);
  }
};
struct TraceQuadFormOp {
  template <typename S>
  static S call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t n = ctx.idata[0];
    return stan::math::trace_quad_form(matrix_at(xs, 0, n, n),
                                       matrix_at(xs, 1, n, ctx.idata[1]));
  }
};
struct TraceGenQuadFormOp {
  template <typename S>
  static S call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::trace_gen_quad_form(
        matrix_at(xs, 0, ctx.idata[0], ctx.idata[1]),
        matrix_at(xs, 1, ctx.idata[2], ctx.idata[3]),
        matrix_at(xs, 2, ctx.idata[4], ctx.idata[5]));
  }
};
struct MatrixExpMultiplyOp {
  template <typename S>
  static auto call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::matrix_exp_multiply(
        matrix_at(xs, 0, ctx.idata[0], ctx.idata[1]),
        matrix_at(xs, 1, ctx.idata[2], ctx.idata[3]));
  }
};
struct ScaleMatrixExpMultiplyOp {
  template <typename S>
  static Mat<S> call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::scale_matrix_exp_multiply(
        xs[0](0), matrix_at(xs, 1, ctx.idata[0], ctx.idata[1]),
        matrix_at(xs, 2, ctx.idata[2], ctx.idata[3]));
  }
};

struct TraceDotOp {
  template <typename S>
  static S call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    return stan::math::trace_dot(matrix_at(xs, 0, ctx.idata[0], ctx.idata[1]),
                                 matrix_at(xs, 1, ctx.idata[2], ctx.idata[3]));
  }
};
struct Norm1Op {
  template <typename S>
  static S call(const KernelCtx&, std::vector<Vec<S>>& xs) {
    return stan::math::norm1(xs[0]);
  }
};
struct Norm2Op {
  template <typename S>
  static S call(const KernelCtx&, std::vector<Vec<S>>& xs) {
    return stan::math::norm2(xs[0]);
  }
};
struct DistanceOp {
  template <typename S>
  static S call(const KernelCtx&, std::vector<Vec<S>>& xs) {
    return stan::math::distance(xs[0], xs[1]);
  }
};
struct IncBetaOp {
  template <typename S>
  static S call(const KernelCtx&, std::vector<Vec<S>>& xs) {
    return stan::math::inc_beta(xs[0](0), xs[1](0), xs[2](0));
  }
};
struct InvIncBetaOp {
  template <typename S>
  static S call(const KernelCtx&, std::vector<Vec<S>>& xs) {
    return stan::math::inv_inc_beta(xs[0](0), xs[1](0), xs[2](0));
  }
};
struct Hypergeometric2F1Op {
  template <typename S>
  static S call(const KernelCtx&, std::vector<Vec<S>>& xs) {
    return stan::math::hypergeometric_2F1(xs[0](0), xs[1](0), xs[2](0),
                                          xs[3](0));
  }
};
struct Hypergeometric3F2Op {
  template <typename S>
  static S call(const KernelCtx&, std::vector<Vec<S>>& xs) {
    return stan::math::hypergeometric_3F2(xs[0], xs[1], xs[2](0));
  }
};
struct HypergeometricPFqOp {
  template <typename S>
  static S call(const KernelCtx&, std::vector<Vec<S>>& xs) {
    return stan::math::hypergeometric_pFq(xs[0], xs[1], xs[2](0));
  }
};

struct LogMixVecOp {
  template <typename S>
  static S call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t arrays = ctx.idata[0], width = ctx.idata[1];
    if (arrays < 0) return stan::math::log_mix(xs[0], xs[1]);
    std::vector<Vec<S>> lambda(static_cast<size_t>(arrays), Vec<S>(width));
    for (int64_t m = 0; m < arrays; ++m)
      for (int64_t n = 0; n < width; ++n)
        lambda[static_cast<size_t>(m)](n) = xs[1](m * width + n);
    return stan::math::log_mix(xs[0], lambda);
  }
};
struct HmmMarginalOp {
  template <typename S>
  static S call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t states = ctx.idata[0], steps = ctx.idata[1];
    return stan::math::hmm_marginal(matrix_at(xs, 0, states, steps),
                                    matrix_at(xs, 1, states, states), xs[2]);
  }
};
struct HmmHiddenStateProbOp {
  template <typename S>
  static Eigen::MatrixXd call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t states = ctx.idata[0], steps = ctx.idata[1];
    return stan::math::hmm_hidden_state_prob(
        stan::math::value_of(Mat<S>(matrix_at(xs, 0, states, steps))),
        stan::math::value_of(Mat<S>(matrix_at(xs, 1, states, states))),
        stan::math::value_of(xs[2]));
  }
};

struct GaussianDlmObsOp {
  template <typename S>
  static S call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    const int64_t r = ctx.idata[0], steps = ctx.idata[1], n = ctx.idata[2];
    const bool v_matrix = ctx.idata[3] != 0;
    const int64_t v_size = v_matrix ? r * r : r;
    const S* packed = xs[1].data();
    const Mat<S> y = Eigen::Map<const Mat<S>>(xs[0].data(), r, steps);
    const Mat<S> F = Eigen::Map<const Mat<S>>(packed, n, r);
    packed += n * r;
    const Mat<S> G = Eigen::Map<const Mat<S>>(packed, n, n);
    packed += n * n;
    const S* v = packed;
    packed += v_size;
    const Mat<S> W = Eigen::Map<const Mat<S>>(packed, n, n);
    packed += n * n;
    const Vec<S> m0 = Eigen::Map<const Vec<S>>(packed, n);
    packed += n;
    const Mat<S> C0 = Eigen::Map<const Mat<S>>(packed, n, n);
    const auto density = [&](const auto& V) {
      return (ctx.variant & 1u) ? stan::math::gaussian_dlm_obs_lpdf<true>(
                                      y, F, G, V, W, m0, C0)
                                : stan::math::gaussian_dlm_obs_lpdf<false>(
                                      y, F, G, V, W, m0, C0);
    };
    return v_matrix ? density(Mat<S>(Eigen::Map<const Mat<S>>(v, r, r)))
                    : density(Vec<S>(Eigen::Map<const Vec<S>>(v, r)));
  }
};

std::vector<int> csr_ints(const KernelCtx& ctx, int64_t* at) {
  const int64_t count = ctx.idata[*at];
  std::vector<int> values(ctx.idata + *at + 1, ctx.idata + *at + 1 + count);
  *at += 1 + count;
  return values;
}
struct CsrMatrixTimesVectorOp {
  template <typename S>
  static auto call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    int64_t at = 2;
    const std::vector<int> v = csr_ints(ctx, &at);
    const std::vector<int> u = csr_ints(ctx, &at);
    return stan::math::csr_matrix_times_vector(static_cast<int>(ctx.idata[0]),
                                               static_cast<int>(ctx.idata[1]),
                                               xs[0], v, u, xs[1]);
  }
};
struct CsrToDenseMatrixOp {
  template <typename S>
  static Mat<S> call(const KernelCtx& ctx, std::vector<Vec<S>>& xs) {
    int64_t at = 2;
    const std::vector<int> v = csr_ints(ctx, &at);
    const std::vector<int> u = csr_ints(ctx, &at);
    return stan::math::csr_to_dense_matrix(static_cast<int>(ctx.idata[0]),
                                           static_cast<int>(ctx.idata[1]),
                                           xs[0], v, u);
  }
};

void quantile_fwd(KernelCtx& ctx) {
  const std::vector<double> samples(ctx.in[0].data,
                                    ctx.in[0].data + ctx.in[0].len);
  if (ctx.idata[0] == 0) {
    ctx.out.data[0] = stan::math::quantile(samples, ctx.in[1].data[0]);
    return;
  }
  const std::vector<double> probabilities(ctx.in[1].data,
                                          ctx.in[1].data + ctx.in[1].len);
  const std::vector<double> result =
      stan::math::quantile(samples, probabilities);
  std::copy(result.begin(), result.end(), ctx.out.data);
}
void quantile_bwd(KernelCtx&) {}

}  // namespace

void register_library_kernels() {
  register_kernel(OP_QUANTILE, Kernel{quantile_fwd, quantile_bwd, nullptr});
  register_library<CsrMatrixTimesVectorOp>(OP_CSR_MATRIX_TIMES_VECTOR);
  register_library<CsrToDenseMatrixOp>(OP_CSR_TO_DENSE_MATRIX);
  register_library<GaussianDlmObsOp>(OP_GAUSSIAN_DLM_OBS_LPDF);
  register_library<LogMixVecOp>(OP_LOG_MIX_VEC);
  register_library<HmmMarginalOp>(OP_HMM_MARGINAL);
  register_library<HmmHiddenStateProbOp>(OP_HMM_HIDDEN_STATE_PROB);
  register_library<TraceDotOp>(OP_TRACE_DOT);
  register_library<Norm1Op>(OP_NORM1);
  register_library<Norm2Op>(OP_NORM2);
  register_library<DistanceOp>(OP_DISTANCE);
  register_library<IncBetaOp>(OP_INC_BETA);
  register_library<InvIncBetaOp>(OP_INV_INC_BETA);
  register_library<Hypergeometric2F1Op>(OP_HYPERGEOMETRIC_2F1);
  register_library<Hypergeometric3F2Op>(OP_HYPERGEOMETRIC_3F2);
  register_library<HypergeometricPFqOp>(OP_HYPERGEOMETRIC_PFQ);
  register_library<DeterminantOp>(OP_DETERMINANT);
  register_library<LogDeterminantSpdOp>(OP_LOG_DETERMINANT_SPD);
  register_library<Chol2invOp>(OP_CHOL2INV);
  register_library<GeneralizedInverseOp>(OP_GENERALIZED_INVERSE);
  register_library<QrQOp>(OP_QR_Q);
  register_library<QrROp>(OP_QR_R);
  register_library<QrThinQOp>(OP_QR_THIN_Q);
  register_library<QrThinROp>(OP_QR_THIN_R);
  register_library<MatrixPowerOp>(OP_MATRIX_POWER);
  register_library<TraceQuadFormOp>(OP_TRACE_QUAD_FORM);
  register_library<TraceGenQuadFormOp>(OP_TRACE_GEN_QUAD_FORM);
  register_library<MatrixExpMultiplyOp>(OP_MATRIX_EXP_MULTIPLY);
  register_library<ScaleMatrixExpMultiplyOp>(OP_SCALE_MATRIX_EXP_MULTIPLY);
}

}  // namespace stanli
