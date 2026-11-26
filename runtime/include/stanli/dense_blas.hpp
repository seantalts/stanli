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

// Whether this system has BLASSetThreading (macOS 15 or later).
bool blas_thread_control_available();

// Whether the accelerated kernels run on a system without
// BLASSetThreading (macOS 11 to 14). There Accelerate would choose its own
// threads for every call, and a large factorization can give different
// results from one run to the next on a busy machine, so the selection pass
// keeps every op on the plain kernels: no speedup, and the results of a
// build without the option.
inline constexpr bool kAccelerateWithoutThreadControl = false;
inline bool accelerated_kernels_enabled(bool thread_control_available) {
  return thread_control_available || kAccelerateWithoutThreadControl;
}

// Accelerate's own threads for one accelerated call: only in a program
// lowered with parallel_blas, and only while the chains that run in
// parallel leave cores free, parallel_chains * 2 <= performance_cores.
// Otherwise the call runs on the calling thread alone, and its result is the
// same in every run.
bool blas_may_thread(bool parallel_blas, int parallel_chains,
                     int performance_cores);
// hw.perflevel0.physicalcpu on Apple, else std::thread's
// hardware_concurrency, read once. The developer switch
// STANLI_PERFORMANCE_CORES replaces it for tests.
int performance_cores();

// While it lives, the evaluation on this thread belongs to a program
// lowered with (or without) parallel_blas. Executor sets it around each
// forward and reverse sweep; nested scopes restore the outer value.
class ParallelBlasScope {
 public:
  explicit ParallelBlasScope(bool parallel_blas);
  ~ParallelBlasScope();
  ParallelBlasScope(const ParallelBlasScope&) = delete;
  ParallelBlasScope& operator=(const ParallelBlasScope&) = delete;

 private:
  bool previous_;
};

// While it lives, this thread is one of `chains` that run in parallel. A
// thread outside a chain pool counts as one chain alone.
class ParallelChainsScope {
 public:
  // For threads that cannot tell how many others run beside them, such as a
  // reduce_sum team inside a chain: never Accelerate's threads.
  static constexpr int kUnknownParallelism = 1 << 20;
  explicit ParallelChainsScope(int chains);
  ~ParallelChainsScope();
  ParallelChainsScope(const ParallelChainsScope&) = delete;
  ParallelChainsScope& operator=(const ParallelChainsScope&) = delete;

 private:
  int previous_;
};

// Sets the calling thread's BLASSetThreading mode for one accelerated call,
// by blas_may_thread, then restores the thread's previous setting, so that a
// host such as numpy on the same thread keeps its own. Every accelerated
// BLAS and LAPACK call runs inside one. With its own threads, Accelerate's
// dpotri (through dlauum) sometimes returns a wrong inverse, so the same
// gradient gave different results in different processes
// (tests/test_blas_determinism).
// Needs BLASSetThreading; elsewhere it does nothing.
class BlasCallThreads {
 public:
  BlasCallThreads();
  ~BlasCallThreads();
  BlasCallThreads(const BlasCallThreads&) = delete;
  BlasCallThreads& operator=(const BlasCallThreads&) = delete;

 private:
  unsigned saved_ = 0;
  bool restore_ = false;
};

// Gives each op of the graph whose sizes are at or above its operation's
// threshold the accelerated opcode. Does nothing in a build without
// STANLI_ACCELERATE, when the developer switch STANLI_NO_ACCELERATE is set,
// or when accelerated_kernels_enabled says no. Returns the number of ops it
// changed.
int accelerate_dense_ops(Graph& g);
// The same with the availability of BLASSetThreading given, for tests.
int accelerate_dense_ops(Graph& g, bool thread_control_available);

#ifdef STANLI_ACCELERATE
namespace blas {

// The smallest sizes at which each accelerated kernel is used. See
// runtime/src/dense_blas.cpp for the measurements behind them.
struct Thresholds {
  int64_t gemm_min_dim;  // smallest of rows, inner and columns
  // A matrix-vector product with at least these rows and columns, or at
  // least the tall pair.
  int64_t gemv_min_rows, gemv_min_cols;
  int64_t gemv_tall_min_rows, gemv_tall_min_cols;
  int64_t cholesky_min_n;     // factor and adjoint
  int64_t tri_inverse_min_n;  // multi_normal_cholesky's inverse of L
  int64_t mn_cov_min_n;       // multi_normal's covariance
  // normal_id_glm's design matrix: at least these rows and columns.
  int64_t glm_min_rows, glm_min_cols;
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
// Inverse of a symmetric positive definite matrix from its lower Cholesky
// factor, in place, lower triangle only; returns LAPACK's info.
int potri_lower(int64_t n, double* a, int64_t lda);
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
