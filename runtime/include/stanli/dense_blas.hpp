#ifndef STANLI_DENSE_BLAS_HPP
#define STANLI_DENSE_BLAS_HPP

// Direct calls into the system BLAS and LAPACK (Apple's Accelerate) for the
// large dense kernels, in a STANLI_ACCELERATE build. Matrices are
// column-major doubles with explicit leading dimensions. Eigen itself never
// calls BLAS: only the ops that accelerate_dense_ops gives an accelerated
// opcode reach these functions, so every smaller op runs exactly the code of
// a build without the option.

#include <cstdint>

namespace stanli {

struct Graph;
struct Op;
struct Slot;

// "accelerate" in a STANLI_ACCELERATE build, "none" otherwise.
const char* blas_provider();

// Keeps the BLAS calls made on this thread on this thread while it lives,
// then restores the thread's previous setting. Accelerate otherwise spreads
// a large product or factorization over threads of its own, which
// oversubscribes the cores when chains already run one per core, and gives
// results that differ in the last digits from the single-threaded ones for
// some large factorizations. Needs macOS 15 (BLASSetThreading, a per-thread
// setting); elsewhere, and in builds without Accelerate, it does nothing.
class SingleThreadedBlas {
 public:
  SingleThreadedBlas();
  ~SingleThreadedBlas();
  SingleThreadedBlas(const SingleThreadedBlas&) = delete;
  SingleThreadedBlas& operator=(const SingleThreadedBlas&) = delete;

 private:
  unsigned saved_ = 0;
  bool active_ = false;
};

// Gives each op of the graph whose sizes are at or above its operation's
// threshold the accelerated opcode. Does nothing in a build without
// STANLI_ACCELERATE. Returns the number of ops it changed.
int accelerate_dense_ops(Graph& g);

#ifdef STANLI_ACCELERATE
namespace blas {

// The smallest sizes at which each accelerated kernel is used. See
// runtime/src/dense_blas.cpp for the measurements behind them.
struct Thresholds {
  int64_t gemm_min_dim;   // smallest of rows, inner and columns
  int64_t gemv_min_rows;  // matrix-vector product: rows and columns
  int64_t gemv_min_cols;
  int64_t cholesky_min_n;     // factor and adjoint
  int64_t tri_inverse_min_n;  // multi_normal_cholesky's inverse of L
};
const Thresholds& thresholds();

// The accelerated opcode for this op at its bound sizes, or its own opcode
// when it stays on the plain kernel. Defined beside the kernels.
uint16_t accelerated_opcode(const Op& op, const Slot* slots);

// Number of calls into BLAS or LAPACK since the process started, for tests.
uint64_t call_count();

// C = alpha * op(A) * op(B) + beta * C
void gemm(bool trans_a, bool trans_b, int64_t m, int64_t n, int64_t k,
          double alpha, const double* a, int64_t lda, const double* b,
          int64_t ldb, double beta, double* c, int64_t ldc);
// y = alpha * op(A) * x + beta * y
void gemv(bool trans, int64_t m, int64_t n, double alpha, const double* a,
          int64_t lda, const double* x, double beta, double* y);
// Lower Cholesky factor in place; returns LAPACK's info.
int potrf_lower(int64_t n, double* a, int64_t lda);
// Inverse of a lower triangular matrix in place; returns LAPACK's info.
int trtri_lower(int64_t n, double* a, int64_t lda);
// B = alpha * op(L)^-1 * B (left) or alpha * B * op(L)^-1 (right), L lower.
void trsm_lower(bool left, bool trans, int64_t m, int64_t n, double alpha,
                const double* l, int64_t ldl, double* b, int64_t ldb);
// B = alpha * op(L) * B, L lower, from the left.
void trmm_lower_left(bool trans, int64_t m, int64_t n, double alpha,
                     const double* l, int64_t ldl, double* b, int64_t ldb);
// x = op(L) * x, L lower.
void trmv_lower(bool trans, int64_t n, const double* l, int64_t ldl, double* x);
// C = alpha * S * B + beta * C, S symmetric with its lower triangle given.
void symm_lower_left(int64_t m, int64_t n, double alpha, const double* s,
                     int64_t lds, const double* b, int64_t ldb, double beta,
                     double* c, int64_t ldc);

}  // namespace blas
#endif

}  // namespace stanli

#endif
