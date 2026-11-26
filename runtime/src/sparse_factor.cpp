#include <stanli/sparse_factor.hpp>

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include "checked_size.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace stanli {

namespace {

using Sparse = Eigen::SparseMatrix<double, Eigen::ColMajor, int>;

Sparse lower_to_eigen(int64_t n, const int64_t* col_ptr, const int64_t* row_idx,
                      const double* values) {
  if (n < 1) throw std::invalid_argument("matrix needs at least one row");
  if (!col_ptr || !row_idx || !values)
    throw std::invalid_argument("null matrix array");
  if (col_ptr[0] != 0)
    throw std::invalid_argument("inconsistent column pointer array");
  const int64_t nnz = col_ptr[n];
  if (nnz > std::numeric_limits<int>::max() ||
      n > std::numeric_limits<int>::max())
    throw std::invalid_argument("matrix is too large");
  std::vector<Eigen::Triplet<double>> t;
  t.reserve((size_t)nnz);
  for (int64_t j = 0; j < n; ++j) {
    if (col_ptr[j + 1] < col_ptr[j] || col_ptr[j + 1] > nnz)
      throw std::invalid_argument("inconsistent column pointer array");
    for (int64_t p = col_ptr[j]; p < col_ptr[j + 1]; ++p) {
      const int64_t i = row_idx[p];
      if (i < 0 || i >= n)
        throw std::invalid_argument("row index out of range");
      if (i < j) continue;
      if (!std::isfinite(values[p]))
        throw std::invalid_argument("matrix has a non-finite entry");
      t.emplace_back((int)i, (int)j, values[p]);
    }
  }
  Sparse a((int)n, (int)n);
  a.setFromTriplets(t.begin(), t.end());
  return a;
}

}  // namespace

SparseFactor sparse_ldlt(int64_t n, const int64_t* col_ptr,
                         const int64_t* row_idx, const double* values,
                         const int64_t* perm) {
  Sparse a = lower_to_eigen(n, col_ptr, row_idx, values);
  SparseFactor f;
  f.n = n;
  Sparse l;
  Eigen::VectorXd d;
  auto finish = [&](auto& ldlt) {
    if (ldlt.info() != Eigen::Success)
      throw std::runtime_error("the matrix has a zero pivot");
    l = ldlt.matrixL();
    d = ldlt.vectorD();
  };
  if (perm == nullptr) {
    Eigen::SimplicialLDLT<Sparse, Eigen::Lower, Eigen::AMDOrdering<int>> ldlt;
    ldlt.compute(a);
    finish(ldlt);
    const auto& inv = ldlt.permutationPinv().indices();
    f.perm.assign(inv.data(), inv.data() + n);
  } else {
    std::vector<char> seen((size_t)n, 0);
    Eigen::VectorXi indices(n);
    for (int64_t i = 0; i < n; ++i) {
      if (perm[i] < 0 || perm[i] >= n || seen[(size_t)perm[i]])
        throw std::invalid_argument("perm is not a permutation of 0..n-1");
      seen[(size_t)perm[i]] = 1;
      indices[(int)perm[i]] = (int)i;
    }
    Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic, int> p(indices);
    Sparse b;
    b = a.selfadjointView<Eigen::Lower>().twistedBy(p);
    Eigen::SimplicialLDLT<Sparse, Eigen::Lower, Eigen::NaturalOrdering<int>>
        ldlt;
    ldlt.compute(b);
    finish(ldlt);
    f.perm.assign(perm, perm + n);
  }
  l.makeCompressed();
  f.col_ptr.assign((size_t)n + 1, 0);
  f.row_idx.reserve((size_t)l.nonZeros() + (size_t)n);
  f.values.reserve((size_t)l.nonZeros() + (size_t)n);
  for (int64_t j = 0; j < n; ++j) {
    f.row_idx.push_back(j);
    f.values.push_back(1.0);
    for (Sparse::InnerIterator it(l, (int)j); it; ++it) {
      if (it.row() <= j) continue;
      f.row_idx.push_back(it.row());
      f.values.push_back(it.value());
    }
    f.col_ptr[(size_t)j + 1] = (int64_t)f.row_idx.size();
  }
  f.d.assign(d.data(), d.data() + n);
  for (int64_t j = 0; j < n; ++j) {
    const double v = f.d[(size_t)j];
    if (!(v != 0.0) || !std::isfinite(v))
      throw std::runtime_error("the matrix has a zero or non-finite pivot");
    if (v < 0) ++f.n_negative;
    f.log_abs_det += std::log(std::fabs(v));
  }
  return f;
}

LowRank sparse_lowrank(const SparseFactor& f, int64_t k, const double* u) {
  const int64_t n = f.n;
  if (k < 1) throw std::invalid_argument("the low-rank width must be positive");
  const int64_t nk =
      checked_size(n, k, "the low-rank width k is too large for n parameters");
  const int64_t kk = checked_size(k, k, "the low-rank width k is too large");
  LowRank r;
  r.k = k;
  r.c.assign((size_t)n, 0.0);
  r.b.assign((size_t)nk, 0.0);
  r.w.assign((size_t)nk, 0.0);
  std::vector<double> x((size_t)n);
  for (int64_t col = 0; col < k; ++col) {
    for (int64_t i = 0; i < n; ++i)
      x[(size_t)i] = u[col * n + f.perm[(size_t)i]];
    for (int64_t j = 0; j < n; ++j) {
      const double xj = x[(size_t)j];
      for (int64_t p = f.col_ptr[(size_t)j] + 1; p < f.col_ptr[(size_t)j + 1];
           ++p)
        x[(size_t)f.row_idx[(size_t)p]] -= f.values[(size_t)p] * xj;
    }
    for (int64_t i = 0; i < n; ++i) r.w[(size_t)(i * k + col)] = x[(size_t)i];
  }
  std::vector<double> s((size_t)kk, 0.0);
  std::vector<double> v((size_t)k);
  for (int64_t j = 0; j < n; ++j) {
    const double* wj = r.w.data() + j * k;
    double quad = 0;
    for (int64_t a = 0; a < k; ++a) {
      double sw = 0;
      for (int64_t b = 0; b < k; ++b) sw += s[(size_t)(a * k + b)] * wj[b];
      v[(size_t)a] = wj[a] - sw;
      quad += wj[a] * v[(size_t)a];
    }
    const double c2 = f.d[(size_t)j] + quad;
    if (!(c2 > 0.0) || !std::isfinite(c2))
      throw std::runtime_error(
          "the corrected matrix is not positive definite at pivot " +
          std::to_string(j));
    const double c = std::sqrt(c2);
    r.c[(size_t)j] = c;
    double* bj = r.b.data() + j * k;
    for (int64_t a = 0; a < k; ++a) bj[a] = v[(size_t)a] / c;
    for (int64_t a = 0; a < k; ++a)
      for (int64_t b = 0; b < k; ++b) s[(size_t)(a * k + b)] += bj[a] * bj[b];
  }
  return r;
}

}  // namespace stanli
