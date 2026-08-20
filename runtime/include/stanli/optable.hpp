// Opcode registry. Every op, native or legacy, presents the same interface.
#ifndef STANLI_OPTABLE_HPP
#define STANLI_OPTABLE_HPP

#include <stanli/graph.hpp>

#include <limits>

namespace stanli {

// One list, two uses: the enum and the name table are generated from it,
// so a new op cannot be added to one and forgotten in the other.
#define STANLI_OPCODE_LIST(X)       \
  X(OP_EXP)                         \
  X(OP_ADD_N)                       \
  X(OP_BCAST_FMA)                   \
  X(OP_MATVEC)                      \
  X(OP_POISSON_LOG_LPMF)            \
  X(OP_BERNOULLI_LOGIT_LPMF)        \
  X(OP_BERNOULLI_LPMF)              \
  X(OP_POISSON_LPMF)                \
  X(OP_NEG_BINOMIAL_2_LPMF)         \
  X(OP_BINOMIAL_LPMF)               \
  X(OP_BINOMIAL_LOGIT_LPMF)         \
  X(OP_BERNOULLI_LOGIT_GLM_LPMF)    \
  X(OP_POISSON_LOG_GLM_LPMF)        \
  X(OP_NEG_BINOMIAL_2_LOG_GLM_LPMF) \
  X(OP_BETA_BINOMIAL_LPMF)          \
  X(OP_LOGIT)                       \
  X(OP_MEAN)                        \
  X(OP_REP_VEC)                     \
  X(OP_INDEX)                       \
  X(OP_SET_INDEX)                   \
  X(OP_SET_INDEX_INPLACE)           \
  X(OP_SLICE)                       \
  X(OP_SET_SLICE)                   \
  X(OP_SET_SLICE_STRIDED)           \
  X(OP_SLICE_STRIDED)               \
  X(OP_GATHER)                      \
  X(OP_CONCAT2)                     \
  X(OP_REP_MAT)                     \
  X(OP_GP_EXP_QUAD_COV)             \
  X(OP_DIAG_MATRIX)                 \
  X(OP_CHOLESKY)                    \
  X(OP_MULTI_NORMAL_CHOL_LPDF)      \
  X(OP_MULTI_NORMAL_LPDF)           \
  X(OP_MULTI_NORMAL_PREC_LPDF)      \
  X(OP_GEMM)                        \
  X(OP_LOG_SOFTMAX)                 \
  X(OP_CONSTRAIN_CHOL_CORR)         \
  X(OP_LKJ_CORR_CHOL_LPDF)          \
  X(OP_LKJ_CORR_LPDF)               \
  X(OP_WISHART_LPDF)                \
  X(OP_INV_WISHART_LPDF)            \
  X(OP_WISHART_CHOL_LPDF)           \
  X(OP_INV_WISHART_CHOL_LPDF)       \
  X(OP_MULTI_GP_LPDF)               \
  X(OP_MULTI_GP_CHOL_LPDF)          \
  X(OP_MULTI_STUDENT_T_LPDF)        \
  X(OP_MULTI_STUDENT_T_CHOL_LPDF)   \
  X(OP_MULTINOMIAL_LPMF)            \
  X(OP_MULTINOMIAL_LOGIT_LPMF)      \
  X(OP_DIRICHLET_MULTINOMIAL_LPMF)  \
  X(OP_ORDERED_PROBIT_LPMF)         \
  X(OP_WIENER_LPDF)                 \
  X(OP_LKJ_COV_LPDF)                \
  X(OP_BINOMIAL_LOGIT_GLM_LPMF)     \
  X(OP_CATEGORICAL_LOGIT_GLM_LPMF)  \
  X(OP_ORDERED_LOGISTIC_GLM_LPMF)   \
  X(OP_NORMAL_ID_GLM_LPDF)          \
  X(OP_TRANSPOSE)                   \
  X(OP_ODE)                         \
  X(OP_ISLAND)                      \
  X(OP_EIGENVALUES_SYM)             \
  X(OP_EIGENVECTORS_SYM)            \
  X(OP_LOG_SUM_EXP)                 \
  X(OP_LSE2)                        \
  X(OP_LOG_DIFF_EXP)                \
  X(OP_LOG_MIX)                     \
  X(OP_SOFTMAX)                     \
  X(OP_SUM_VEC)                     \
  X(OP_ADD)                         \
  X(OP_SUB)                         \
  X(OP_MUL)                         \
  X(OP_DIV)                         \
  X(OP_POW)                         \
  X(OP_DOT)                         \
  X(OP_NEG)                         \
  X(OP_EXPV)                        \
  X(OP_LOGV)                        \
  X(OP_INV_LOGIT)                   \
  X(OP_SQRT)                        \
  X(OP_SQUARE)                      \
  X(OP_LOG1M)                       \
  X(OP_TANHV)                       \
  X(OP_TRIGAMMA)                    \
  X(OP_CUMSUM)                      \
  X(OP_FMA)                         \
  X(OP_ATAN2)                       \
  X(OP_BETA_FN)                     \
  X(OP_FDIM)                        \
  X(OP_FMAX)                        \
  X(OP_FMIN)                        \
  X(OP_FMOD)                        \
  X(OP_GAMMA_P)                     \
  X(OP_GAMMA_Q)                     \
  X(OP_HYPOT)                       \
  X(OP_LBETA)                       \
  X(OP_LCHOOSE)                     \
  X(OP_LMULTIPLY)                   \
  X(OP_LOG_FALLING_FACTORIAL)       \
  X(OP_LOG_INV_LOGIT_DIFF)          \
  X(OP_LOG_MODIFIED_BESSEL_1)       \
  X(OP_LOG_RISING_FACTORIAL)        \
  X(OP_OWENS_T)                     \
  X(OP_BESSEL_1)                    \
  X(OP_BESSEL_2)                    \
  X(OP_MODIFIED_BESSEL_1)           \
  X(OP_MODIFIED_BESSEL_2)           \
  X(OP_BINARY_LOG_LOSS)             \
  X(OP_LMGAMMA)                     \
  X(OP_FALLING_FACTORIAL)           \
  X(OP_RISING_FACTORIAL)            \
  X(OP_LDEXP)                       \
  X(OP_CONSTRAIN_LOWER)             \
  X(OP_CONSTRAIN_UPPER)             \
  X(OP_CONSTRAIN_LU)                \
  X(OP_CONSTRAIN_SIMPLEX)           \
  X(OP_CONSTRAIN_ORDERED)           \
  X(OP_CONSTRAIN_POS_ORDERED)       \
  X(OP_CONSTRAIN_OFFSET_MULT)       \
  X(OP_CONSTRAIN_UNIT_VECTOR)       \
  X(OP_CONSTRAIN_SUM_TO_ZERO)       \
  X(OP_CONSTRAIN_SUM_TO_ZERO_MAT)   \
  X(OP_CONSTRAIN_CORR_MATRIX)       \
  X(OP_CONSTRAIN_COV_MATRIX)        \
  X(OP_CONSTRAIN_CHOL_COV)          \
  X(OP_CHECK_STRUCTURED)            \
  X(OP_CHECK_MATCHING_DIMS)         \
  X(OP_CHECK_LOWER)                 \
  X(OP_CHECK_UPPER)                 \
  X(OP_CATEGORICAL)                 \
  X(OP_REJECT)                      \
  X(OP_PRINT)                       \
  X(OP_DIRICHLET_LPDF)

// Scalar densities, one line each: this list generates the opcode, the
// name, the kernel, its registration, and the lowering table entry
// (densities.cpp, lower.cpp). They are all the same shape --
// density_fwd_v<N> over stan-math's propto-true/false pair, with
// density_bwd<N> contracting the stashed partials -- so the only
// things that vary are the stan-math name and the argument count.
// Discrete densities are not here: an integer outcome needs idata
// plumbing that differs per distribution.
//
// The last field is the instantiation tier, two independent bits over
// what CmdStan gets for free by instantiating only the one combination
// your model uses. A density instantiates stan-math's template once per
// activity mask, twice for propto, and again for the elementwise form --
// 4 * 2^N, about 630 KB of object for a 3-argument density.
//
//   bit 1 (2): instantiate the propto family. Term-dropping is keyed on
//              the argument TYPES, so propto only means anything with the
//              per-mask dispatch; the two always come together. Without
//              this bit, `y ~ foo(...)` evaluates the full density: same
//              gradient, lp off by a constant.
//   bit 0 (1): instantiate the mask dispatch for the full form. Without
//              it one all-active binding covers every mask -- the value
//              does not depend on the mask with propto off -- but data
//              arguments bind as recorder scalars, so their partials are
//              computed and thrown away. On radon_pooled (a vectorized
//              normal over 919 data points) that cost 30-40%.
//
// So: 3 is the whole ladder, for the densities models lean on; 2 keeps
// exact propto lp and pays the collapse only on an explicit
// `target += foo_lpdf(...)`; 1 is fast but constant-shifted lp; 0 is two
// instantiations, about 40 KB, and is how the long tail gets to exist at
// all. STANLI_LITE_LP clears bit 1 across the board -- see density_tier.
#define STANLI_DENSITY_FULL_MASKS 1
#define STANLI_DENSITY_PROPTO 2
// The sub-lists are the shard boundaries. Each one is a translation unit
// (densities_*.cpp), because the instantiations are what make this
// expensive to compile: the tier-3 distributions below expand to 4 * 2^N
// copies of a stan-math template each, and one file holding all of them
// peaked at 7.6 GB of compiler memory. Splitting is the only lever, since
// the instantiations themselves are the product.
//
// Balance them by COST, not by count. A tier-3 four-argument density is
// worth about eight tier-2 two-argument ones, which is why COMMON_A holds
// six entries and REST_A holds seven.
#define STANLI_SCALAR_DENSITY_LIST_COMMON_A(X) \
  X(OP_NORMAL_LPDF, normal_lpdf, 3, 3)         \
  X(OP_CAUCHY_LPDF, cauchy_lpdf, 3, 3)         \
  X(OP_GAMMA_LPDF, gamma_lpdf, 3, 3)           \
  X(OP_BETA_LPDF, beta_lpdf, 3, 3)             \
  X(OP_LOGNORMAL_LPDF, lognormal_lpdf, 3, 3)

#define STANLI_SCALAR_DENSITY_LIST_COMMON_B(X)         \
  X(OP_STUDENT_T_LPDF, student_t_lpdf, 4, 3)           \
  X(OP_UNIFORM_LPDF, uniform_lpdf, 3, 3)               \
  X(OP_DOUBLE_EXP_LPDF, double_exponential_lpdf, 3, 3) \
  X(OP_EXPONENTIAL_LPDF, exponential_lpdf, 2, 3)       \
  X(OP_INV_GAMMA_LPDF, inv_gamma_lpdf, 3, 3)           \
  X(OP_STD_NORMAL_LPDF, std_normal_lpdf, 1, 3)         \
  X(OP_WEIBULL_LPDF, weibull_lpdf, 3, 3)               \
  X(OP_LOGISTIC_LPDF, logistic_lpdf, 3, 3)

#define STANLI_SCALAR_DENSITY_LIST_REST_A(X)                         \
  X(OP_CHI_SQUARE_LPDF, chi_square_lpdf, 2, 2)                       \
  X(OP_INV_CHI_SQUARE_LPDF, inv_chi_square_lpdf, 2, 2)               \
  X(OP_SCALED_INV_CHI_SQUARE_LPDF, scaled_inv_chi_square_lpdf, 3, 2) \
  X(OP_FRECHET_LPDF, frechet_lpdf, 3, 2)                             \
  X(OP_GUMBEL_LPDF, gumbel_lpdf, 3, 2)                               \
  X(OP_LOGLOGISTIC_LPDF, loglogistic_lpdf, 3, 2)                     \
  X(OP_PARETO_LPDF, pareto_lpdf, 3, 2)                               \
  X(OP_SKEW_NORMAL_LPDF, skew_normal_lpdf, 4, 2)                     \
  X(OP_EXP_MOD_NORMAL_LPDF, exp_mod_normal_lpdf, 4, 2)

#define STANLI_SCALAR_DENSITY_LIST_REST_B(X)             \
  X(OP_PARETO_TYPE_2_LPDF, pareto_type_2_lpdf, 4, 2)     \
  X(OP_RAYLEIGH_LPDF, rayleigh_lpdf, 2, 2)               \
  X(OP_VON_MISES_LPDF, von_mises_lpdf, 3, 2)             \
  X(OP_BETA_PROPORTION_LPDF, beta_proportion_lpdf, 3, 2) \
  X(OP_SKEW_DOUBLE_EXPONENTIAL_LPDF, skew_double_exponential_lpdf, 4, 2)

#define STANLI_SCALAR_DENSITY_LIST(X)    \
  STANLI_SCALAR_DENSITY_LIST_COMMON_A(X) \
  STANLI_SCALAR_DENSITY_LIST_COMMON_B(X) \
  STANLI_SCALAR_DENSITY_LIST_REST_A(X)   \
  STANLI_SCALAR_DENSITY_LIST_REST_B(X)

// Discrete densities: an integer outcome that rides in idata instead of
// on a propagator edge, plus NReal real arguments that behave exactly like
// an lpdf's. The outcome is argument 0 in every Stan signature, so the
// lowering entry is NReal + 1 arguments with one int group, and the same
// tier field applies -- see the note above.
//
// The older lpmfs (poisson, bernoulli, binomial, the GLM) predate this
// list and stay hand-written: binomial carries two int groups and the GLM
// carries a data matrix, so they are not the same shape. What is here is
// everything whose outcome is a single int per lane.
#define STANLI_INT_DENSITY_LIST(X)                             \
  X(OP_NEG_BINOMIAL_2_LOG_LPMF, neg_binomial_2_log_lpmf, 2, 3) \
  X(OP_NEG_BINOMIAL_LPMF, neg_binomial_lpmf, 2, 2)             \
  X(OP_BETA_NEG_BINOMIAL_LPMF, beta_neg_binomial_lpmf, 3, 2)   \
  X(OP_YULE_SIMON_LPMF, yule_simon_lpmf, 1, 2)

// Distribution functions: cdf, lcdf, lccdf. Same shape as an lpdf --
// real arguments, partials through ops_partials -- with one difference:
// there is no propto form to instantiate, because a cumulative
// probability has no terms to drop. So the tier field here only ever
// carries bit 0.
//
// Every one of them is at 0, and that is measured rather than assumed.
// These are not cheap functions the way an lpdf is: they pull in
// inc_beta, grad_reg_inc_gamma and friends, so an instantiation runs
// about 30 KB. Giving the thirteen common distributions the mask
// dispatch (2^N instantiations each) cost 4.8 MB of the library on its
// own -- more than the whole rest of this list. At 0 the 72 functions
// together cost 2.2 MB. The price is that data arguments bind as
// recorder scalars and their partials are computed and dropped, which on
// a function called once per truncation bound is not where the time
// goes. Raise one to 1 if a profile ever says otherwise.
//
// This is what truncation runs on. `y ~ normal(mu, sigma) T[a, b]` is
// lowered by stanc3 into the density minus log_diff_exp of the two
// bounds' lcdfs, so a model with a T[] on it needs the lcdf and
// log_diff_exp and nothing else. Censoring uses the same pieces.
//
// The 27 discrete cdfs (poisson_lcdf and friends) are not here: their
// outcome is an int, which is the idata plumbing STANLI_INT_DENSITY_LIST
// handles, and no truncated discrete model has asked for them yet.
//
// von_mises is not here either, and for a reason worth keeping: its cdf
// does `res *= 0.0` on the scalar type at a degenerate endpoint. The
// recorder computes in doubles and carries no tape, so an assignment
// like that would change the value and leave the partials describing the
// old one. rvar has no arithmetic operators precisely so this fails to
// compile instead.
#define STANLI_SCALAR_CDF_LIST_A(X)                              \
  X(OP_BETA_CDF, beta_cdf, 3, 0)                                 \
  X(OP_BETA_LCCDF, beta_lccdf, 3, 0)                             \
  X(OP_BETA_LCDF, beta_lcdf, 3, 0)                               \
  X(OP_BETA_PROPORTION_LCCDF, beta_proportion_lccdf, 3, 0)       \
  X(OP_BETA_PROPORTION_LCDF, beta_proportion_lcdf, 3, 0)         \
  X(OP_CAUCHY_CDF, cauchy_cdf, 3, 0)                             \
  X(OP_CAUCHY_LCCDF, cauchy_lccdf, 3, 0)                         \
  X(OP_CAUCHY_LCDF, cauchy_lcdf, 3, 0)                           \
  X(OP_CHI_SQUARE_CDF, chi_square_cdf, 2, 0)                     \
  X(OP_CHI_SQUARE_LCCDF, chi_square_lccdf, 2, 0)                 \
  X(OP_CHI_SQUARE_LCDF, chi_square_lcdf, 2, 0)                   \
  X(OP_DOUBLE_EXPONENTIAL_CDF, double_exponential_cdf, 3, 0)     \
  X(OP_DOUBLE_EXPONENTIAL_LCCDF, double_exponential_lccdf, 3, 0) \
  X(OP_DOUBLE_EXPONENTIAL_LCDF, double_exponential_lcdf, 3, 0)   \
  X(OP_EXP_MOD_NORMAL_CDF, exp_mod_normal_cdf, 4, 0)             \
  X(OP_EXP_MOD_NORMAL_LCCDF, exp_mod_normal_lccdf, 4, 0)         \
  X(OP_EXP_MOD_NORMAL_LCDF, exp_mod_normal_lcdf, 4, 0)           \
  X(OP_EXPONENTIAL_CDF, exponential_cdf, 2, 0)                   \
  X(OP_EXPONENTIAL_LCCDF, exponential_lccdf, 2, 0)               \
  X(OP_EXPONENTIAL_LCDF, exponential_lcdf, 2, 0)                 \
  X(OP_FRECHET_CDF, frechet_cdf, 3, 0)                           \
  X(OP_FRECHET_LCCDF, frechet_lccdf, 3, 0)                       \
  X(OP_FRECHET_LCDF, frechet_lcdf, 3, 0)                         \
  X(OP_GAMMA_CDF, gamma_cdf, 3, 0)                               \
  X(OP_GAMMA_LCCDF, gamma_lccdf, 3, 0)                           \
  X(OP_GAMMA_LCDF, gamma_lcdf, 3, 0)                             \
  X(OP_GUMBEL_CDF, gumbel_cdf, 3, 0)                             \
  X(OP_GUMBEL_LCCDF, gumbel_lccdf, 3, 0)                         \
  X(OP_GUMBEL_LCDF, gumbel_lcdf, 3, 0)                           \
  X(OP_INV_CHI_SQUARE_CDF, inv_chi_square_cdf, 2, 0)             \
  X(OP_INV_CHI_SQUARE_LCCDF, inv_chi_square_lccdf, 2, 0)         \
  X(OP_INV_CHI_SQUARE_LCDF, inv_chi_square_lcdf, 2, 0)           \
  X(OP_INV_GAMMA_CDF, inv_gamma_cdf, 3, 0)                       \
  X(OP_INV_GAMMA_LCCDF, inv_gamma_lccdf, 3, 0)                   \
  X(OP_INV_GAMMA_LCDF, inv_gamma_lcdf, 3, 0)                     \
  X(OP_LOGISTIC_CDF, logistic_cdf, 3, 0)                         \
  X(OP_LOGISTIC_LCCDF, logistic_lccdf, 3, 0)                     \
  X(OP_LOGISTIC_LCDF, logistic_lcdf, 3, 0)

#define STANLI_SCALAR_CDF_LIST_B(X)                                        \
  X(OP_LOGLOGISTIC_CDF, loglogistic_cdf, 3, 0)                             \
  X(OP_LOGNORMAL_CDF, lognormal_cdf, 3, 0)                                 \
  X(OP_LOGNORMAL_LCCDF, lognormal_lccdf, 3, 0)                             \
  X(OP_LOGNORMAL_LCDF, lognormal_lcdf, 3, 0)                               \
  X(OP_NORMAL_CDF, normal_cdf, 3, 0)                                       \
  X(OP_NORMAL_LCCDF, normal_lccdf, 3, 0)                                   \
  X(OP_NORMAL_LCDF, normal_lcdf, 3, 0)                                     \
  X(OP_PARETO_CDF, pareto_cdf, 3, 0)                                       \
  X(OP_PARETO_LCCDF, pareto_lccdf, 3, 0)                                   \
  X(OP_PARETO_LCDF, pareto_lcdf, 3, 0)                                     \
  X(OP_PARETO_TYPE_2_CDF, pareto_type_2_cdf, 4, 0)                         \
  X(OP_PARETO_TYPE_2_LCCDF, pareto_type_2_lccdf, 4, 0)                     \
  X(OP_PARETO_TYPE_2_LCDF, pareto_type_2_lcdf, 4, 0)                       \
  X(OP_RAYLEIGH_CDF, rayleigh_cdf, 2, 0)                                   \
  X(OP_RAYLEIGH_LCCDF, rayleigh_lccdf, 2, 0)                               \
  X(OP_RAYLEIGH_LCDF, rayleigh_lcdf, 2, 0)                                 \
  X(OP_SCALED_INV_CHI_SQUARE_CDF, scaled_inv_chi_square_cdf, 3, 0)         \
  X(OP_SCALED_INV_CHI_SQUARE_LCCDF, scaled_inv_chi_square_lccdf, 3, 0)     \
  X(OP_SCALED_INV_CHI_SQUARE_LCDF, scaled_inv_chi_square_lcdf, 3, 0)       \
  X(OP_SKEW_NORMAL_CDF, skew_normal_cdf, 4, 0)                             \
  X(OP_SKEW_NORMAL_LCCDF, skew_normal_lccdf, 4, 0)                         \
  X(OP_SKEW_NORMAL_LCDF, skew_normal_lcdf, 4, 0)                           \
  X(OP_STD_NORMAL_CDF, std_normal_cdf, 1, 0)                               \
  X(OP_STD_NORMAL_LCCDF, std_normal_lccdf, 1, 0)                           \
  X(OP_SKEW_DOUBLE_EXPONENTIAL_CDF, skew_double_exponential_cdf, 4, 0)     \
  X(OP_SKEW_DOUBLE_EXPONENTIAL_LCDF, skew_double_exponential_lcdf, 4, 0)   \
  X(OP_SKEW_DOUBLE_EXPONENTIAL_LCCDF, skew_double_exponential_lccdf, 4, 0) \
  X(OP_STD_NORMAL_LCDF, std_normal_lcdf, 1, 0)                             \
  X(OP_STUDENT_T_CDF, student_t_cdf, 4, 0)                                 \
  X(OP_STUDENT_T_LCCDF, student_t_lccdf, 4, 0)                             \
  X(OP_STUDENT_T_LCDF, student_t_lcdf, 4, 0)                               \
  X(OP_UNIFORM_CDF, uniform_cdf, 3, 0)                                     \
  X(OP_UNIFORM_LCCDF, uniform_lccdf, 3, 0)                                 \
  X(OP_UNIFORM_LCDF, uniform_lcdf, 3, 0)                                   \
  X(OP_WEIBULL_CDF, weibull_cdf, 3, 0)                                     \
  X(OP_WEIBULL_LCCDF, weibull_lccdf, 3, 0)                                 \
  X(OP_WEIBULL_LCDF, weibull_lcdf, 3, 0)

#define STANLI_SCALAR_CDF_LIST(X) \
  STANLI_SCALAR_CDF_LIST_A(X)     \
  STANLI_SCALAR_CDF_LIST_B(X)

// The same, for distributions whose outcome is an integer: the count
// rides in idata exactly as it does for the lpmfs, and the real
// arguments behave as above. Field 3 is the count of REAL arguments, so
// the lowering entry is one more than that with one int group.
//
// Not every discrete cdf fits: discrete_range is integers all the way
// down, so those three keep waiting for a layout rather than a list line.
// The binomials carry a second int group and have their own list below.
#define STANLI_INT_CDF_LIST(X)                                 \
  X(OP_BERNOULLI_CDF, bernoulli_cdf, 1, 0)                     \
  X(OP_BERNOULLI_LCCDF, bernoulli_lccdf, 1, 0)                 \
  X(OP_BERNOULLI_LCDF, bernoulli_lcdf, 1, 0)                   \
  X(OP_BETA_NEG_BINOMIAL_CDF, beta_neg_binomial_cdf, 3, 0)     \
  X(OP_BETA_NEG_BINOMIAL_LCCDF, beta_neg_binomial_lccdf, 3, 0) \
  X(OP_BETA_NEG_BINOMIAL_LCDF, beta_neg_binomial_lcdf, 3, 0)   \
  X(OP_NEG_BINOMIAL_CDF, neg_binomial_cdf, 2, 0)               \
  X(OP_NEG_BINOMIAL_LCCDF, neg_binomial_lccdf, 2, 0)           \
  X(OP_NEG_BINOMIAL_LCDF, neg_binomial_lcdf, 2, 0)             \
  X(OP_NEG_BINOMIAL_2_CDF, neg_binomial_2_cdf, 2, 0)           \
  X(OP_POISSON_CDF, poisson_cdf, 1, 0)                         \
  X(OP_POISSON_LCCDF, poisson_lccdf, 1, 0)                     \
  X(OP_POISSON_LCDF, poisson_lcdf, 1, 0)                       \
  X(OP_YULE_SIMON_CDF, yule_simon_cdf, 1, 0)                   \
  X(OP_YULE_SIMON_LCCDF, yule_simon_lccdf, 1, 0)               \
  X(OP_YULE_SIMON_LCDF, yule_simon_lcdf, 1, 0)

// The binomials: an outcome group AND a trials group, so idata is the
// [len, vals...] pair their lpmfs already write rather than raw values,
// and the kernel unpacks it with with_int_group. Field 3 is again the
// count of REAL arguments, so the lowering entry is two more than that
// with two int groups.
//
// Neither group is a lane broadcast the lowering can do on its own: a
// scalar is spelled with a length of -1 and stan-math broadcasts it,
// which is what keeps `binomial_lcdf(n | N, theta)` with a bare int `n`
// from arriving as a size-1 container.
#define STANLI_TWO_INT_CDF_LIST(X)                     \
  X(OP_BETA_BINOMIAL_CDF, beta_binomial_cdf, 2, 0)     \
  X(OP_BETA_BINOMIAL_LCCDF, beta_binomial_lccdf, 2, 0) \
  X(OP_BETA_BINOMIAL_LCDF, beta_binomial_lcdf, 2, 0)   \
  X(OP_BINOMIAL_CDF, binomial_cdf, 1, 0)               \
  X(OP_BINOMIAL_LCCDF, binomial_lccdf, 1, 0)           \
  X(OP_BINOMIAL_LCDF, binomial_lcdf, 1, 0)

// The distribution functions that cannot go through the recorder at all,
// whatever list they are put on. These build their result with arithmetic
// on the autodiff scalar rather than through stan-math's partials
// propagator -- von_mises_cdf writes `res *= 0.0` and compares
// `x_n == -pi`, neg_binomial_2_lcdf forms `phi_vec[i] / (phi_vec[i] +
// mu_vec[i])` -- and rvar deliberately has no operators (recorder.hpp),
// so listing them above produces a compile error rather than a wrong
// answer. The discriminator is mechanical: `grep -c
// operands_and_partials` on the prim header is 2 for everything on the
// lists above and 0 for these five.
//
// They get the nested var tape ordered_probit and wiener get, in
// matrix_fns.cpp, which has no restriction on what a density does with
// its scalar type and costs a tape per gradient call. Field 3 is the
// count of REAL arguments; the int list carries one integer group on top
// of that, exactly as STANLI_INT_CDF_LIST does.
#define STANLI_TAIL_CDF_LIST(X)                \
  X(OP_VON_MISES_CDF, von_mises_cdf, 3, 0)     \
  X(OP_VON_MISES_LCCDF, von_mises_lccdf, 3, 0) \
  X(OP_VON_MISES_LCDF, von_mises_lcdf, 3, 0)

#define STANLI_TAIL_INT_CDF_LIST(X)                      \
  X(OP_NEG_BINOMIAL_2_LCCDF, neg_binomial_2_lccdf, 2, 0) \
  X(OP_NEG_BINOMIAL_2_LCDF, neg_binomial_2_lcdf, 2, 0)

// Ordinal regression. Two things make these different from the list
// above, and both are expressed in the kernel rather than here: the
// cutpoint argument is a whole vector whatever its length (field 4 is
// the VecMask that says so, since a one-element cutpoint set is a
// one-element vector and NOT a scalar), and the integer outcome has to
// reach stan-math as a std::vector<int> -- ordered_logistic asks
// scalar_seq_view for a mutable data() pointer, which an
// Eigen::Map<const VectorXi> cannot give it.
//
// reroll.cpp must never fuse these: element n of a shared cutpoint
// vector is not observation n's cutpoints. They opt in to neither re-roll
// density trait, which is the whole guard.
#define STANLI_ORDERED_DENSITY_LIST(X) \
  X(OP_ORDERED_LOGISTIC_LPMF, ordered_logistic_lpmf, 2, 0x2)
// A unary may either always chain, chain only away from zero (abs), or be
// disconnected.  Disconnected is not the same as multiplying by a zero
// derivative: an infinite upstream adjoint must not turn 0 into NaN.
enum class UnaryTopology { Chained, Nonzero, Disconnected };

constexpr bool unary_has_pullback(UnaryTopology topology, double x) {
  switch (topology) {
    case UnaryTopology::Chained:
      return true;
    case UnaryTopology::Nonzero:
      return x != 0.0;
    case UnaryTopology::Disconnected:
      return false;
  }
  return false;
}

// Scalar unary math, one line each: opcode, kernel, registration, lowering
// entry and interpreter branch all come from here. `x` is the argument, `y`
// the already-computed output and `seed` the upstream adjoint. Keeping the
// ordered delta expression here matters: algebraically equal derivative-times-
// seed formulas can round differently. The final field owns topology.
//
// These are cheap in a way densities are not. A density instantiates
// stan-math's template once per activity mask, twice for propto and again
// for the elementwise form -- 4 * 2^N per distribution, about 630 KB of
// object each. An entry here costs about 5 KB, because the pullback delta is
// written out rather than obtained by instantiating an autodiff template.
// fn_sweep.py checks every one against CmdStan, which is what makes
// hand-written derivatives safe to write at this rate.
#define STANLI_SCALAR_UNARY_LIST(X)                                            \
  X(OP_LGAMMA, lgamma, stan::math::lgamma(x), seed* stan::math::digamma(x),    \
    UnaryTopology::Chained)                                                    \
  X(OP_DIGAMMA, digamma, stan::math::digamma(x),                               \
    seed* stan::math::trigamma(x), UnaryTopology::Chained)                     \
  X(OP_LOG1P, log1p, stan::math::log1p(x), seed / (1.0 + x),                   \
    UnaryTopology::Chained)                                                    \
  X(OP_EXPM1, expm1, stan::math::expm1(x), seed*(y + 1.0),                     \
    UnaryTopology::Chained)                                                    \
  X(OP_PHI, Phi, stan::math::Phi(x),                                           \
    (seed * stan::math::INV_SQRT_TWO_PI) * std::exp(-0.5 * x * x),             \
    UnaryTopology::Chained)                                                    \
  X(OP_INV_PHI, inv_Phi, stan::math::inv_Phi(x),                               \
    seed* std::exp(-stan::math::std_normal_lpdf(y)), UnaryTopology::Chained)   \
  X(OP_ERF, erf, std::erf(x),                                                  \
    seed*(stan::math::TWO_OVER_SQRT_PI * std::exp(-x * x)),                    \
    UnaryTopology::Chained)                                                    \
  X(OP_ERFC, erfc, std::erfc(x),                                               \
    -(seed * (stan::math::TWO_OVER_SQRT_PI * std::exp(-x * x))),               \
    UnaryTopology::Chained)                                                    \
  X(OP_INV, inv, 1.0 / x, -(seed / (x * x)), UnaryTopology::Chained)           \
  X(OP_INV_SQRT, inv_sqrt, stan::math::inv_sqrt(x),                            \
    -((0.5 * seed) / (x * std::sqrt(x))), UnaryTopology::Chained)              \
  X(OP_INV_SQUARE, inv_square, 1.0 / (x * x), -((2.0 * seed) / (x * x * x)),   \
    UnaryTopology::Chained)                                                    \
  X(OP_LOG1M_EXP, log1m_exp, stan::math::log1m_exp(x),                         \
    -(seed / std::expm1(-x)), UnaryTopology::Chained)                          \
  X(OP_LOG1P_EXP, log1p_exp, stan::math::log1p_exp(x),                         \
    seed* stan::math::inv_logit(x), UnaryTopology::Chained)                    \
  X(OP_LOG_INV_LOGIT, log_inv_logit, stan::math::log_inv_logit(x),             \
    seed* stan::math::inv_logit(-x), UnaryTopology::Chained)                   \
  X(OP_LOG1M_INV_LOGIT, log1m_inv_logit, stan::math::log1m_inv_logit(x),       \
    seed * -stan::math::inv_logit(x), UnaryTopology::Chained)                  \
  X(OP_INV_CLOGLOG, inv_cloglog, stan::math::inv_cloglog(x),                   \
    seed* std::exp(x - std::exp(x)), UnaryTopology::Chained)                   \
  X(OP_SIN, sin, std::sin(x), seed* std::cos(x), UnaryTopology::Chained)       \
  X(OP_COS, cos, std::cos(x), -(seed * std::sin(x)), UnaryTopology::Chained)   \
  X(OP_TAN, tan, std::tan(x), seed * (1.0 + y * y), UnaryTopology::Chained)    \
  X(OP_ASIN, asin, std::asin(x), seed / std::sqrt(1.0 - (x * x)),              \
    UnaryTopology::Chained)                                                    \
  X(OP_ACOS, acos, std::acos(x), -(seed / std::sqrt(1.0 - (x * x))),           \
    UnaryTopology::Chained)                                                    \
  X(OP_ATAN, atan, std::atan(x), seed / (1.0 + (x * x)),                       \
    UnaryTopology::Chained)                                                    \
  X(OP_SINH, sinh, std::sinh(x), seed* std::cosh(x), UnaryTopology::Chained)   \
  X(OP_COSH, cosh, std::cosh(x), seed* std::sinh(x), UnaryTopology::Chained)   \
  X(OP_ASINH, asinh, std::asinh(x), seed / std::sqrt(x * x + 1.0),             \
    UnaryTopology::Chained)                                                    \
  X(OP_ACOSH, acosh, stan::math::acosh(x), seed / std::sqrt(x * x - 1.0),      \
    UnaryTopology::Chained)                                                    \
  X(OP_ATANH, atanh, stan::math::atanh(x), seed / (1.0 - x * x),               \
    UnaryTopology::Chained)                                                    \
  X(OP_CBRT, cbrt, std::cbrt(x), seed / ((3.0 * y) * y),                       \
    UnaryTopology::Chained)                                                    \
  X(OP_EXP2, exp2, std::exp2(x), (seed * y) * stan::math::LOG_TWO,             \
    UnaryTopology::Chained)                                                    \
  X(OP_LOG2, log2, stan::math::log2(x), seed / (stan::math::LOG_TWO * x),      \
    UnaryTopology::Chained)                                                    \
  X(OP_LOG10, log10, std::log10(x), seed / (stan::math::LOG_TEN * x),          \
    UnaryTopology::Chained)                                                    \
  /* Phi_approx's value is written out rather than called, because Math's */   \
  /* two overloads disagree: the double one cubes with pow(x, 3.0), the   */   \
  /* var one with x * x * x, and those round differently. The var one is  */   \
  /* what a parameter reaches in CmdStan and what every route here has to */   \
  /* agree with, so it is the one spelled.                                */   \
  X(OP_PHI_APPROX, Phi_approx,                                                 \
    stan::math::inv_logit(0.07056 * x * (x * x) + 1.5976 * x),                 \
    seed*(y * (1.0 - y) * (3.0 * 0.07056 * (x * x) + 1.5976)),                 \
    UnaryTopology::Chained)                                                    \
  X(OP_INV_ERFC, inv_erfc, stan::math::inv_erfc(x),                            \
    -(seed * std::exp(stan::math::LOG_SQRT_PI - stan::math::LOG_TWO + y * y)), \
    UnaryTopology::Chained)                                                    \
  X(OP_LAMBERT_W0, lambert_w0, stan::math::lambert_w0(x),                      \
    seed / (x + std::exp(y)), UnaryTopology::Chained)                          \
  X(OP_LAMBERT_WM1, lambert_wm1, stan::math::lambert_wm1(x),                   \
    seed / (x + std::exp(y)), UnaryTopology::Chained)                          \
  X(OP_STD_NORMAL_LOG_QF, std_normal_log_qf, stan::math::std_normal_log_qf(x), \
    seed* std::exp(x - stan::math::std_normal_lpdf(y)),                        \
    UnaryTopology::Chained)                                                    \
  X(OP_TGAMMA, tgamma, stan::math::tgamma(x),                                  \
    (seed * y) * stan::math::digamma(x), UnaryTopology::Chained)               \
  X(OP_ABS, abs, std::fabs(x),                                                 \
    x < 0.0   ? -seed                                                          \
    : x > 0.0 ? seed                                                           \
              : std::numeric_limits<double>::quiet_NaN(),                      \
    UnaryTopology::Nonzero)                                                    \
  X(OP_FLOOR, floor, std::floor(x), 0.0, UnaryTopology::Disconnected)          \
  X(OP_CEIL, ceil, std::ceil(x), 0.0, UnaryTopology::Disconnected)             \
  X(OP_ROUND, round, std::round(x), 0.0, UnaryTopology::Disconnected)          \
  X(OP_TRUNC, trunc, std::trunc(x), 0.0, UnaryTopology::Disconnected)          \
  X(OP_STEP, step, x < 0 ? 0.0 : 1.0, 0.0, UnaryTopology::Disconnected)

// Two-argument scalar math: opcode, language name, stan-math function.
// Unlike the unary list, no hand-written derivative: the kernel evaluates
// stan-math's own var overload on a nested tape, so the value and every
// partial are the reference's by construction. That trade is deliberate --
// these are long-tail functions (lchoose alone has a stability recursion
// and five gradient edge cases), and a transcription error here is exactly
// the class of bug the conformance sweep exists to catch. The language
// name feeds the lowering table and the MIR interpreter; lchoose is
// stanc3's name for stan-math's binomial_coefficient_log.
#define STANLI_SCALAR_BINARY_LIST(X)                                        \
  X(OP_ATAN2, atan2, atan2)                                                 \
  X(OP_BETA_FN, beta, beta)                                                 \
  X(OP_FDIM, fdim, fdim)                                                    \
  X(OP_FMAX, fmax, fmax)                                                    \
  X(OP_FMIN, fmin, fmin)                                                    \
  X(OP_FMOD, fmod, fmod)                                                    \
  X(OP_GAMMA_P, gamma_p, gamma_p)                                           \
  X(OP_GAMMA_Q, gamma_q, gamma_q)                                           \
  X(OP_HYPOT, hypot, hypot)                                                 \
  X(OP_LBETA, lbeta, lbeta)                                                 \
  X(OP_LCHOOSE, lchoose, binomial_coefficient_log)                          \
  X(OP_LMULTIPLY, lmultiply, lmultiply)                                     \
  X(OP_LOG_FALLING_FACTORIAL, log_falling_factorial, log_falling_factorial) \
  X(OP_LOG_INV_LOGIT_DIFF, log_inv_logit_diff, log_inv_logit_diff)          \
  X(OP_LOG_MODIFIED_BESSEL_1, log_modified_bessel_first_kind,               \
    log_modified_bessel_first_kind)                                         \
  X(OP_LOG_RISING_FACTORIAL, log_rising_factorial, log_rising_factorial)    \
  X(OP_OWENS_T, owens_t, owens_t)

// Two-argument scalar math where one argument is an INT. Stan vectorizes
// these exactly like the list above, but stan-math's overloads take the
// order/count/exponent as `int`, not as a promoted double, so they cannot
// ride the var,var kernel: the int side is bound as an int on both the
// forward and the reverse call, and only the real side has a derivative.
// Split by which position the int occupies, because that is the only thing
// the two kernels differ in.
#define STANLI_SCALAR_BINARY_INT_FIRST_LIST(X)            \
  X(OP_BESSEL_1, bessel_first_kind, bessel_first_kind)    \
  X(OP_BESSEL_2, bessel_second_kind, bessel_second_kind)  \
  X(OP_MODIFIED_BESSEL_1, modified_bessel_first_kind,     \
    modified_bessel_first_kind)                           \
  X(OP_MODIFIED_BESSEL_2, modified_bessel_second_kind,    \
    modified_bessel_second_kind)                          \
  X(OP_BINARY_LOG_LOSS, binary_log_loss, binary_log_loss) \
  X(OP_LMGAMMA, lmgamma, lmgamma)

#define STANLI_SCALAR_BINARY_INT_SECOND_LIST(X)                 \
  X(OP_FALLING_FACTORIAL, falling_factorial, falling_factorial) \
  X(OP_RISING_FACTORIAL, rising_factorial, rising_factorial)    \
  X(OP_LDEXP, ldexp, ldexp)

// The tier a density is actually built at. STANLI_LITE_LP drops the
// propto family from every one of them: about half the library, at the
// cost of an lp__ that differs from CmdStan's by a per-model constant on
// every `~` statement. Gradients are untouched to the bit; the chain a
// seed produces is not, because a shifted lp rounds differently inside
// the Hamiltonian (docs/lite-lp.md)
// -- which is why the browser build takes this and the wheel does not.
constexpr int density_tier(int listed) {
#ifdef STANLI_LITE_LP
  return listed & STANLI_DENSITY_FULL_MASKS;
#else
  return listed;
#endif
}

// True when this build can reproduce CmdStan's lp__ exactly, rather than
// up to a constant. Reported by stanli_version() and checked by the
// verification harnesses, which relax from bitwise to same-constant.
constexpr bool exact_lp_build() {
#ifdef STANLI_LITE_LP
  return false;
#else
  return true;
#endif
}

// Every opcode list, in enum order, named once. The enum and the name
// table both expand this, so a list added to one cannot be forgotten in
// the other. PLAIN takes (name), DENSITY takes (code, fn, n, m), UNARY
// takes (code, fn, value, delta, topology).
#define STANLI_ALL_OPCODES(PLAIN, DENSITY, UNARY) \
  STANLI_OPCODE_LIST(PLAIN)                       \
  STANLI_SCALAR_DENSITY_LIST(DENSITY)             \
  STANLI_INT_DENSITY_LIST(DENSITY)                \
  STANLI_SCALAR_CDF_LIST(DENSITY)                 \
  STANLI_INT_CDF_LIST(DENSITY)                    \
  STANLI_TWO_INT_CDF_LIST(DENSITY)                \
  STANLI_TAIL_CDF_LIST(DENSITY)                   \
  STANLI_TAIL_INT_CDF_LIST(DENSITY)               \
  STANLI_ORDERED_DENSITY_LIST(DENSITY)            \
  STANLI_SCALAR_UNARY_LIST(UNARY)

enum Opcode : uint16_t {
  OP_NONE_ = 0,
#define STANLI_OPCODE_ENUM(name) name,
#define STANLI_DENSITY_ENUM(code, fn, n, m) code,
#define STANLI_UNARY_ENUM(code, fn, value, delta, topology) code,
  STANLI_ALL_OPCODES(STANLI_OPCODE_ENUM, STANLI_DENSITY_ENUM, STANLI_UNARY_ENUM)
#undef STANLI_OPCODE_ENUM
#undef STANLI_DENSITY_ENUM
#undef STANLI_UNARY_ENUM
      OP_COUNT_
};

// Opt-in facts shared by graph rewrites. An unknown/new opcode has none.
namespace op_trait {
constexpr uint8_t kRerollDensity = 1 << 0;
constexpr uint8_t kRerollIdataDensity = 1 << 1;
constexpr uint8_t kRerollAnyDensity = kRerollDensity | kRerollIdataDensity;
constexpr uint8_t kRerollWidenable = 1 << 2;
constexpr uint8_t kBackwardValueFree = 1 << 3;
}  // namespace op_trait

constexpr uint8_t op_traits(uint16_t opcode) {
  switch (opcode) {
    // Real-argument lpdfs whose vector kernel returns a summed lp with
    // per-element partials. The long-tail density kernels deliberately do
    // not opt in until their vectorized path is supported by re-roll.
    case OP_NORMAL_LPDF:
    case OP_CAUCHY_LPDF:
    case OP_STUDENT_T_LPDF:
    case OP_GAMMA_LPDF:
    case OP_BETA_LPDF:
    case OP_LOGNORMAL_LPDF:
    case OP_UNIFORM_LPDF:
    case OP_DOUBLE_EXP_LPDF:
    case OP_EXPONENTIAL_LPDF:
    case OP_INV_GAMMA_LPDF:
    case OP_STD_NORMAL_LPDF:
      return op_trait::kRerollDensity;

    // Integer-outcome lpmfs whose scalar-lane idata can be concatenated.
    // Ordered densities are intentionally absent: their cutpoint vector is
    // shared by lanes and cannot be fused this way.
    case OP_BERNOULLI_LPMF:
    case OP_BERNOULLI_LOGIT_LPMF:
    case OP_POISSON_LPMF:
    case OP_POISSON_LOG_LPMF:
    case OP_NEG_BINOMIAL_2_LPMF:
#define STANLI_INT_DENSITY_TRAIT(code, fn, nreal, tier) case code:
      STANLI_INT_DENSITY_LIST(STANLI_INT_DENSITY_TRAIT)
#undef STANLI_INT_DENSITY_TRAIT
      return op_trait::kRerollIdataDensity;

    // Native mixture kernels both widen lane-wise and stash all partials
    // needed by reverse mode, so their backward does not reread inputs.
    case OP_LOG_MIX:
    case OP_LSE2:
      return op_trait::kRerollWidenable | op_trait::kBackwardValueFree;

    // Shape-dispatched elementwise ops that can widen scalar lanes in place.
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_FMA:
    case OP_NEG:
    case OP_EXPV:
    case OP_LOGV:
    case OP_INV_LOGIT:
    case OP_SQRT:
    case OP_SQUARE:
    case OP_LOG1M:
    case OP_TANHV:
    case OP_LOG_INV_LOGIT:
    case OP_LOG1M_INV_LOGIT:
      return op_trait::kRerollWidenable;

    // Backward routes adjoints without rereading input values. This permits
    // a later destructive update to reuse the input buffer safely.
    case OP_INDEX:
    case OP_SLICE:
    case OP_SLICE_STRIDED:
    case OP_GATHER:
    case OP_SET_INDEX:
    case OP_SET_INDEX_INPLACE:
    case OP_LOG_SUM_EXP:
      return op_trait::kBackwardValueFree;
    default:
      return 0;
  }
}

constexpr bool has_op_trait(uint16_t opcode, uint8_t trait) {
  return (op_traits(opcode) & trait) != 0;
}

// "OP_NORMAL_LPDF" for a known opcode, "OP_?" otherwise. Diagnostics and
// tooling only; never on a hot path.
const char* opcode_name(uint16_t opcode);

struct Kernel {
  // Reads ctx.in values, writes ctx.out, may stash partials in ctx.scratch.
  void (*forward)(KernelCtx&) = nullptr;
  // Reads ctx.out_adj / out_adj_vec (+ values, scratch), accumulates into
  // ctx.in_adj entries whose data is non-null.
  void (*backward)(KernelCtx&) = nullptr;
  // Scratch doubles needed, given bound slot shapes. Null means zero.
  int64_t (*scratch_size)(const Op&, const Slot* slots) = nullptr;
};

// The most common Kernel::scratch_size shape: one scratch double per
// element of every input.
inline int64_t sum_in_lens(const Op& op, const Slot* slots) {
  int64_t t = 0;
  for (int i = 0; i < op.n_in; ++i) t += slots[op.in[i]].len;
  return t;
}

Kernel& kernel(uint16_t opcode);
// Called by kernel TUs at static-init time.
void register_kernel(uint16_t opcode, Kernel k);

// The registered kernel for an opcode, or null. Registers the built-in
// kernels on first use, so it is safe from lowering, which runs before
// any Executor exists.
const Kernel* find_kernel(uint16_t opcode);

}  // namespace stanli

#endif
