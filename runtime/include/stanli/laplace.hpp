// The Laplace whitening builder (experimental): the sparsity pattern of the
// Hessian over the latent block, the marginal mode of the hyperparameters,
// the negative Hessian there, and a factor of it repaired to be positive
// definite. stanli_laplace_whiten in capi.h runs it on a model; the pieces
// are declared here so that tests can check each one.
#ifndef STANLI_LAPLACE_HPP
#define STANLI_LAPLACE_HPP

#include <stanli/sparse_factor.hpp>

// Stan's Eigen header first: it sets the plugins that Stan Math needs.
#include <stan/math/prim/fun/Eigen.hpp>
#include <stan/services/util/create_rng.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace stanli::laplace {

using Sparse = Eigen::SparseMatrix<double, Eigen::ColMajor, int>;
using Rng = stan::rng_t;

// The model refused to evaluate a point: a rejection or any other error.
struct EvalError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Writes the gradient of the log density at q into grad and returns the log
// density. Throws EvalError when the model refuses q.
using GradientFn =
    std::function<double(const Eigen::VectorXd& q, Eigen::VectorXd& grad)>;
// The dense Hessian of the log density at q. Throws on failure.
using HessianFn = std::function<Eigen::MatrixXd(const Eigen::VectorXd& q)>;

// A GradientFn that counts its calls.
class Gradient {
 public:
  explicit Gradient(GradientFn f) : f_(std::move(f)) {}
  double operator()(const Eigen::VectorXd& q, Eigen::VectorXd& grad) {
    ++count;
    return f_(q, grad);
  }
  Eigen::VectorXd at(const Eigen::VectorXd& q) {
    Eigen::VectorXd g;
    (*this)(q, g);
    return g;
  }
  int64_t count = 0;

 private:
  GradientFn f_;
};

// The symmetric pattern, with the diagonal, of the entries of h whose
// magnitude is above tol * sqrt(|h_ii| |h_jj|) (the largest stored magnitude
// where that scale is zero). Patterns store 1 at each entry.
Sparse threshold_pattern(const Sparse& h, double tol);

// a restricted to the rows and columns `index`, which is sorted.
Sparse submatrix(const Sparse& a, const std::vector<int>& index);

// Greedy colouring of the columns of a symmetric pattern such that columns
// of one colour share no row (a distance-2 colouring of its graph); columns
// with the most entries are coloured first.
std::vector<int> distance2_colouring(const Sparse& pattern);

// How sparse_hessian perturbs the columns of a pattern: the `dense` columns,
// and any column with more than max(64, 4 sqrt(m)) entries, one at a time;
// the others together by colour.
struct HessianPlan {
  Sparse pattern;
  std::vector<int> dense;
  std::vector<int> free;
  std::vector<int> colour;  // of each entry of `free`
  int n_colours = 0;
};
HessianPlan hessian_plan(const Sparse& pattern, const std::vector<int>& dense);

// Central-difference Hessian of the log density over the coordinates `cols`
// of q, as a symmetric sparse matrix over those coordinates. Costs two
// gradients per colour of the plan and per dense column.
Sparse sparse_hessian(Gradient& grad, const Eigen::VectorXd& q,
                      const HessianPlan& plan, const std::vector<int>& cols);

// A symmetric pattern over the coordinates `iu` that contains every nonzero
// of the Hessian block, from forward differences of the gradient along
// random weights on random groups of columns. g0 is the gradient at q over
// `iu`, and step the difference step of each coordinate.
Sparse probe_candidates(Gradient& grad, const Eigen::VectorXd& q,
                        const std::vector<int>& iu, const Eigen::VectorXd& g0,
                        const Eigen::VectorXd& step, Rng& rng);

// Whether h, a Hessian block over `iu`, predicts central differences of the
// gradient along three random directions to within what a missing entry of
// the size the pattern keeps would change.
bool probe_agrees(Gradient& grad, const Eigen::VectorXd& q,
                  const std::vector<int>& iu, const Sparse& h,
                  const Eigen::VectorXd& step, Rng& rng, double tol);

// The coloured Hessian over `candidates`, thresholded at tol, or nothing
// when it has a non-finite entry or probe_agrees fails.
std::optional<Sparse> verified_pattern(Gradient& grad, const Eigen::VectorXd& q,
                                       const std::vector<int>& iu,
                                       const Eigen::VectorXd& step,
                                       const Sparse& candidates, double tol,
                                       Rng& rng);

// The pattern of the Hessian block over `iu` at q without the dense
// Hessian: probe_candidates, then verified_pattern. Nothing when the
// gradient at q is not finite or the verification fails.
std::optional<Sparse> probe_pattern(Gradient& grad, const Eigen::VectorXd& q,
                                    const std::vector<int>& iu, double tol,
                                    uint32_t seed);

// The pattern of the dense Hessian at q, thresholded at tol and restricted
// to `iu`.
Sparse dense_pattern(const Eigen::MatrixXd& hessian, const std::vector<int>& iu,
                     double tol);

// The eigenvalues of the symmetric q below floor times its largest absolute
// eigenvalue, with their eigenvectors as columns, and the smallest and
// largest eigenvalues. n_negative is the exact number of negative
// eigenvalues, which the search must reach. Dense for n <= 64; otherwise a
// shift-invert Lanczos search.
struct Eigenpairs {
  Eigen::VectorXd values;
  Eigen::MatrixXd vectors;
  double min = 0;
  double max = 0;
};
Eigenpairs bad_eigenpairs(const Sparse& q, double floor, int64_t n_negative);

enum class Repair { abs, abs_dense, clip };

struct Options {
  uint32_t seed = 1;
  int max_outer = 60;
  int inner_iter = 2000;
  double pattern_tolerance = 1e-6;
  double threshold = 1e-6;
  Repair repair = Repair::abs;
  double clip = 1e-8;
  bool dense_pattern = false;
  bool dense_hessian = false;
  bool keep_precision = false;
};

struct Result {
  int64_t n = 0;
  Eigen::VectorXd q_hat, theta_start, theta_hat;
  // The arguments of stanli_model_whiten_lowrank: L in CSC form, each
  // column's diagonal first, and the low-rank correction when rank > 0.
  std::vector<int64_t> perm, col_ptr, row_idx;
  std::vector<double> values;
  int64_t rank = 0;
  std::vector<double> c, b, w;
  int64_t n_clipped = 0;
  bool has_spectrum = false;
  double min_eigenvalue = 0;
  double max_eigenvalue = 0;
  int n_colours = 0;
  int64_t pattern_gradients = 0;
  int64_t marginal_gradients = 0;
  int64_t final_gradients = 0;
  int64_t outer_iterations = 0;
  int64_t outer_evaluations = 0;
  std::string outer_message;
  bool pattern_fallback = false;
  std::vector<std::string> warnings;
  Eigen::MatrixXd precision;  // empty unless Options::keep_precision
};

// The whole builder. `latent` is the sorted index of the latent block in the
// unconstrained vector; every other coordinate is a hyperparameter.
Result whiten(Gradient& grad, const HessianFn& hessian,
              const Eigen::VectorXd& q_start, const std::vector<int>& latent,
              const Options& options);

}  // namespace stanli::laplace

#endif
