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

}  // namespace stanli

#endif
