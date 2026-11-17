// Sparse LDL^T factorization of a symmetric matrix with a fill-reducing
// ordering, and the Cholesky factor of a diagonal-plus-low-rank correction of
// it (experimental; used by the Laplace whitening builder).
#ifndef STANLI_SPARSE_FACTOR_HPP
#define STANLI_SPARSE_FACTOR_HPP

#include <cstdint>
#include <vector>

namespace stanli {

// With (P v)[i] = v[perm[i]]: P A P^T = L diag(d) L^T. L is unit lower
// triangular in compressed sparse column form, each column holding its
// diagonal entry first, then strictly increasing rows. D may have negative
// entries when A is indefinite; there is no pivoting.
struct SparseFactor {
  int64_t n = 0;
  std::vector<int64_t> perm;
  std::vector<int64_t> col_ptr;
  std::vector<int64_t> row_idx;
  std::vector<double> values;
  std::vector<double> d;
  int64_t n_negative = 0;
  double log_abs_det = 0;
};

// A is given by the lower triangle of its compressed sparse column form
// (entries above the diagonal are ignored). With perm null the ordering is
// approximate minimum degree, otherwise perm is used as given. Throws
// std::invalid_argument on malformed input and std::runtime_error when a
// pivot is zero or not finite.
SparseFactor sparse_ldlt(int64_t n, const int64_t* col_ptr,
                         const int64_t* row_idx, const double* values,
                         const int64_t* perm);

// Rows of C = diag(c) + tril(W B^T, -1), n by k row-major, with
// C C^T = diag(d) + W0 W0^T for W0 = L^{-1} P U and U the n by k
// column-major matrix given in the original ordering. Throws
// std::runtime_error when the correction is not positive definite.
struct LowRank {
  int64_t k = 0;
  std::vector<double> c, b, w;
};
LowRank sparse_lowrank(const SparseFactor& f, int64_t k, const double* u);

}  // namespace stanli

#endif
