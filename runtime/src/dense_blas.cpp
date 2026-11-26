// The classic Fortran BLAS and LAPACK entry points that the accelerated
// kernels call, declared here rather than through Accelerate's headers or
// Eigen's blas.h, which disagree about some prototypes. Nothing in this file
// includes Eigen.
#include <stanli/debug_switch.hpp>
#include <stanli/dense_blas.hpp>
#include <stanli/graph.hpp>

#ifdef STANLI_ACCELERATE
#include <atomic>
#include <stdexcept>

extern "C" {
void dgemm_(const char* transa, const char* transb, const int* m, const int* n,
            const int* k, const double* alpha, const double* a, const int* lda,
            const double* b, const int* ldb, const double* beta, double* c,
            const int* ldc);
void dgemv_(const char* trans, const int* m, const int* n, const double* alpha,
            const double* a, const int* lda, const double* x, const int* incx,
            const double* beta, double* y, const int* incy);
void dpotrf_(const char* uplo, const int* n, double* a, const int* lda,
             int* info);
void dtrtri_(const char* uplo, const char* diag, const int* n, double* a,
             const int* lda, int* info);
void dpotri_(const char* uplo, const int* n, double* a, const int* lda,
             int* info);
void dtrsm_(const char* side, const char* uplo, const char* transa,
            const char* diag, const int* m, const int* n, const double* alpha,
            const double* a, const int* lda, double* b, const int* ldb);
void dtrmm_(const char* side, const char* uplo, const char* transa,
            const char* diag, const int* m, const int* n, const double* alpha,
            const double* a, const int* lda, double* b, const int* ldb);
void dtrmv_(const char* uplo, const char* trans, const char* diag, const int* n,
            const double* a, const int* lda, double* x, const int* incx);
void dsymm_(const char* side, const char* uplo, const int* m, const int* n,
            const double* alpha, const double* a, const int* lda,
            const double* b, const int* ldb, const double* beta, double* c,
            const int* ldc);
}
#endif

namespace stanli {

#ifdef STANLI_ACCELERATE
namespace blas {
namespace {

std::atomic<uint64_t> g_calls{0};

// The classic interface takes 32-bit integers. Graph sizes are int64_t, and
// a dimension past INT_MAX would be truncated silently.
int dim(int64_t v) {
  if (v < 0 || v > 2147483647)
    throw std::length_error("matrix dimension exceeds the BLAS interface");
  return static_cast<int>(v);
}
// LAPACK and BLAS reject a leading dimension below 1, even for an empty
// matrix.
int ld(int64_t v) { return v < 1 ? 1 : dim(v); }
void count() { g_calls.fetch_add(1, std::memory_order_relaxed); }

// Every call below runs inside a OneBlasThread (stanli/dense_blas.hpp).

}  // namespace

// The smallest sizes at which the accelerated kernel was faster than the
// plain one, forward plus backward on one thread, at every measured size and
// shape above them, with a step of margin where the sweep showed a dip
// (tools/bench_dense_blas). Cholesky and the triangular inverse start
// higher than that sweep alone suggests: inside a whole Gaussian process
// gradient, the ops around an accelerated Cholesky of n = 40 ran about 25%
// slower and the model as a whole 1.05x slower, and models gained only from
// n = 64. The product starts at 32 for the same reason: kronecker_gp's
// 30 x 30 x 30 products ran 2.2x faster with Accelerate and the gradient
// 1.012x slower. See notes/performance/2026-11-26-accelerate-thresholds.md.
const Thresholds& thresholds() {
  static const Thresholds t{
      /*gemm_min_dim=*/32,
      /*gemv_min_rows=*/256,
      /*gemv_min_cols=*/64,
      /*gemv_tall_min_rows=*/10000,
      /*gemv_tall_min_cols=*/10,
      /*cholesky_min_n=*/64,
      /*tri_inverse_min_n=*/64,
      /*mn_cov_min_n=*/32,
      /*glm_min_rows=*/4096,
      /*glm_min_cols=*/16,
  };
  return t;
}

uint64_t call_count() { return g_calls.load(std::memory_order_relaxed); }

void gemm(bool trans_a, bool trans_b, int64_t m, int64_t n, int64_t k,
          double alpha, const double* a, int64_t lda, const double* b,
          int64_t ldb, double beta, double* c, int64_t ldc) {
  if (m == 0 || n == 0) return;
  const char ta = trans_a ? 'T' : 'N', tb = trans_b ? 'T' : 'N';
  const int im = dim(m), in = dim(n), ik = dim(k), ia = ld(lda), ib = ld(ldb),
            ic = ld(ldc);
  count();
  const OneBlasThread one_thread;
  dgemm_(&ta, &tb, &im, &in, &ik, &alpha, a, &ia, b, &ib, &beta, c, &ic);
}

void gemv(bool trans, int64_t m, int64_t n, double alpha, const double* a,
          int64_t lda, const double* x, double beta, double* y) {
  if (m == 0 || n == 0) return;
  const char t = trans ? 'T' : 'N';
  const int im = dim(m), in = dim(n), ia = ld(lda), one = 1;
  count();
  const OneBlasThread one_thread;
  dgemv_(&t, &im, &in, &alpha, a, &ia, x, &one, &beta, y, &one);
}

int potrf_lower(int64_t n, double* a, int64_t lda) {
  if (n == 0) return 0;
  const char lo = 'L';
  const int in = dim(n), ia = ld(lda);
  int info = 0;
  count();
  const OneBlasThread one_thread;
  dpotrf_(&lo, &in, a, &ia, &info);
  return info;
}

int potri_lower(int64_t n, double* a, int64_t lda) {
  if (n == 0) return 0;
  const char lo = 'L';
  const int in = dim(n), ia = ld(lda);
  int info = 0;
  count();
  const OneBlasThread one_thread;
  dpotri_(&lo, &in, a, &ia, &info);
  return info;
}

int trtri_lower(int64_t n, double* a, int64_t lda) {
  if (n == 0) return 0;
  const char lo = 'L', nonunit = 'N';
  const int in = dim(n), ia = ld(lda);
  int info = 0;
  count();
  const OneBlasThread one_thread;
  dtrtri_(&lo, &nonunit, &in, a, &ia, &info);
  return info;
}

void trsm_lower(bool left, bool trans, int64_t m, int64_t n, double alpha,
                const double* l, int64_t ldl, double* b, int64_t ldb) {
  if (m == 0 || n == 0) return;
  const char side = left ? 'L' : 'R', lo = 'L', t = trans ? 'T' : 'N',
             nonunit = 'N';
  const int im = dim(m), in = dim(n), il = ld(ldl), ib = ld(ldb);
  count();
  const OneBlasThread one_thread;
  dtrsm_(&side, &lo, &t, &nonunit, &im, &in, &alpha, l, &il, b, &ib);
}

void trmm_lower_left(bool trans, int64_t m, int64_t n, double alpha,
                     const double* l, int64_t ldl, double* b, int64_t ldb) {
  if (m == 0 || n == 0) return;
  const char side = 'L', lo = 'L', t = trans ? 'T' : 'N', nonunit = 'N';
  const int im = dim(m), in = dim(n), il = ld(ldl), ib = ld(ldb);
  count();
  const OneBlasThread one_thread;
  dtrmm_(&side, &lo, &t, &nonunit, &im, &in, &alpha, l, &il, b, &ib);
}

void trmv_lower(bool trans, int64_t n, const double* l, int64_t ldl,
                double* x) {
  if (n == 0) return;
  const char lo = 'L', t = trans ? 'T' : 'N', nonunit = 'N';
  const int in = dim(n), il = ld(ldl), one = 1;
  count();
  const OneBlasThread one_thread;
  dtrmv_(&lo, &t, &nonunit, &in, l, &il, x, &one);
}

void symm_lower_left(int64_t m, int64_t n, double alpha, const double* s,
                     int64_t lds, const double* b, int64_t ldb, double beta,
                     double* c, int64_t ldc) {
  if (m == 0 || n == 0) return;
  const char side = 'L', lo = 'L';
  const int im = dim(m), in = dim(n), is = ld(lds), ib = ld(ldb), ic = ld(ldc);
  count();
  const OneBlasThread one_thread;
  dsymm_(&side, &lo, &im, &in, &alpha, s, &is, b, &ib, &beta, c, &ic);
}

}  // namespace blas
#endif

int accelerate_dense_ops(Graph& g) {
#ifdef STANLI_ACCELERATE
  if (debug_env("STANLI_NO_ACCELERATE") != nullptr ||
      !accelerated_kernels_enabled(blas_thread_control_available()))
    return 0;
  int changed = 0;
  for (Op& op : g.ops) {
    const uint16_t to = blas::accelerated_opcode(op, g.slots.data());
    if (to != op.opcode) {
      op.opcode = to;
      ++changed;
    }
  }
  return changed;
#else
  (void)g;
  return 0;
#endif
}

}  // namespace stanli
