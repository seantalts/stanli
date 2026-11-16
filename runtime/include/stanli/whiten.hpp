// Affine change of variables for an existing model graph (experimental).
//
// With q the original unconstrained vector, a point q_hat, and a sparse lower
// triangular L with fill-reducing permutation P, the whitened model has
// coordinates z with q = q_hat + P^T L^{-T} z, where (P v)[i] = v[perm[i]].
// Its log density is the original one at q(z); the constant log|det| is
// dropped.
//
// A spec may also carry a diagonal-plus-low-rank right factor: C =
// diag(c) + tril(W B^T, -1), with W and B of size n by k, so that
// q = q_hat + P^T L^{-T} C^{-T} z. Then L is typically unit lower triangular
// and C C^T = D + W W^T, which makes the implied precision
// P^T L (D + W W^T) L^T P.
#ifndef STANLI_WHITEN_HPP
#define STANLI_WHITEN_HPP

#include <stanli/graph.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace stanli {

struct WhitenSpec {
  int64_t n = 0;
  std::vector<double> q_hat;
  std::vector<int64_t> perm;
  // Lower triangle of L in compressed sparse column form. Every column holds
  // its diagonal entry first, then strictly increasing row indices below it.
  std::vector<int64_t> col_ptr;
  std::vector<int64_t> row_idx;
  std::vector<double> values;
  // Empty unless the spec has the factor C; then c has n entries and cb, cw
  // (the B and W above) hold n by k values each, row-major.
  int64_t k = 0;
  std::vector<double> c;
  std::vector<double> cb;
  std::vector<double> cw;

  bool has_c() const { return !c.empty(); }
  // x <- C^{-T} x, x <- C^{-1} x, x <- C^T x
  void solve_c_transpose(double* x, double* acc) const;
  void solve_c(double* x, double* acc) const;
  void multiply_c_transpose(double* x, double* acc) const;

  void solve_lower(double* x) const;
  void solve_lower_transpose(double* x) const;
  void multiply_lower_transpose(const double* w, double* out) const;
  // out = q_hat + P^T L^{-T} z
  void to_q(const double* z, double* out) const;
  // out = L^T P (q - q_hat)
  void to_z(const double* q, double* out) const;
};

// Throws std::invalid_argument naming the first thing wrong with the inputs.
std::shared_ptr<const WhitenSpec> make_whiten_spec(
    const double* q_hat, int64_t n, const int64_t* perm, const int64_t* col_ptr,
    const int64_t* row_idx, const double* values, int64_t nnz);
std::shared_ptr<const WhitenSpec> make_whiten_spec(
    const double* q_hat, int64_t n, const int64_t* perm, const int64_t* col_ptr,
    const int64_t* row_idx, const double* values, int64_t nnz, int64_t k,
    const double* c, const double* b, const double* w);

// The graph of the model over z. Slot indices of the input are preserved and
// the former parameter slots become written values, so the result binds over
// the same shared data as the input (see Executor's data-source constructor).
// Throws std::invalid_argument when the parameter count is not spec.n.
Graph whiten_graph(const Graph& inner, std::shared_ptr<const WhitenSpec> spec);

void register_whiten_kernel();

}  // namespace stanli

#endif
