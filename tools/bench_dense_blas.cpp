// Crossover sweep for the accelerated dense kernels (STANLI_ACCELERATE):
// forward plus backward time of each plain kernel and its accelerated form
// at one size, on one thread. The thresholds in runtime/src/dense_blas.cpp
// come from this output.
//
//   bench_dense_blas [min_ms]
//
// Prints one line per op and size: plain ns, accelerated ns, plain/accelerated.
#include <stanli/dense_blas.hpp>
#include <stanli/optable.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

#ifdef STANLI_ACCELERATE
namespace {

using Mat = Eigen::MatrixXd;
using Clock = std::chrono::steady_clock;

Mat filled(int64_t r, int64_t c, double phase) {
  Mat m(r, c);
  for (int64_t j = 0; j < c; ++j)
    for (int64_t i = 0; i < r; ++i)
      m(i, j) = std::sin(phase + 0.37 * double(i) + 0.11 * double(j));
  return m;
}

// Best of 7 rounds, each long enough to resolve a few nanoseconds.
double ns_per_call(const std::function<void()>& f, double min_ms) {
  f();
  long reps = 1;
  for (;;) {
    const auto t0 = Clock::now();
    for (long r = 0; r < reps; ++r) f();
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (ms >= min_ms) break;
    reps *= 2;
  }
  double best = 1e300;
  for (int round = 0; round < 7; ++round) {
    const auto t0 = Clock::now();
    for (long r = 0; r < reps; ++r) f();
    best = std::min(
        best,
        std::chrono::duration<double, std::nano>(Clock::now() - t0).count() /
            double(reps));
  }
  return best;
}

struct Case {
  stanli::KernelCtx ctx;
  std::vector<std::vector<double>> buffers;
  std::vector<int> idata;
  double* add(int64_t len, double phase) {
    buffers.emplace_back((size_t)len);
    for (int64_t i = 0; i < len; ++i)
      buffers.back()[(size_t)i] = std::sin(phase + 0.29 * double(i));
    return buffers.back().data();
  }
};

void compare(const char* name, const char* shape, Case& c, uint16_t plain,
             uint16_t fast, double min_ms) {
  using namespace stanli;
  const Kernel* kp = find_kernel(plain);
  const Kernel* kf = find_kernel(fast);
  // Each backward accumulates into its input adjoints; their values do not
  // affect the timing.
  const double tp = ns_per_call(
      [&] {
        kp->forward(c.ctx);
        kp->backward(c.ctx);
      },
      min_ms);
  const double tf = ns_per_call(
      [&] {
        kf->forward(c.ctx);
        kf->backward(c.ctx);
      },
      min_ms);
  std::printf("%-22s %-16s plain %12.1f ns  accelerated %12.1f ns  %.2fx\n",
              name, shape, tp, tf, tp / tf);
  std::fflush(stdout);
}

}  // namespace
#endif

int main(int argc, char** argv) {
#ifndef STANLI_ACCELERATE
  (void)argc;
  (void)argv;
  std::printf("bench_dense_blas needs a STANLI_ACCELERATE build\n");
  return 1;
#else
  using namespace stanli;
  const double min_ms = argc > 1 ? std::atof(argv[1]) : 20.0;
  const ChainBlasThreads one_thread(ChainBlasThreads::kUnknownParallelism);
  char shape[64];

  // gemm: square, and the rectangular shapes of corpus models.
  static const int kGemmShapes[][3] = {
      {4, 4, 4},       {6, 6, 6},       {8, 8, 8},       {10, 10, 10},
      {12, 12, 12},    {16, 16, 16},    {24, 24, 24},    {32, 32, 32},
      {48, 48, 48},    {64, 64, 64},    {96, 96, 96},    {128, 128, 128},
      {192, 192, 192}, {256, 256, 256}, {512, 512, 512}, {512, 128, 128},
      {512, 128, 512}, {1000, 10, 10},  {1000, 100, 10}, {100, 100, 100},
      {64, 64, 4},     {256, 256, 8},   {4096, 64, 64},  {32, 32, 512},
      {100, 2, 2},     {1000, 2, 1},    {2, 100, 2},     {4, 4, 100},
      {8, 8, 64},      {64, 8, 8},      {8, 64, 8},      {100, 3, 3},
      {20, 20, 2},     {16, 16, 1},     {64, 64, 1},     {3, 3, 1000},
      {14, 14, 14},    {24, 1000, 24},  {1000, 24, 24},  {24, 24, 1000},
      {24, 24, 24}};
  for (const auto& s : kGemmShapes) {
    Case c;
    c.idata = {s[0], s[1], s[2]};
    c.ctx.n_in = 2;
    c.ctx.idata = c.idata.data();
    c.ctx.n_idata = 3;
    const int64_t la = int64_t(s[0]) * s[1], lb = int64_t(s[1]) * s[2],
                  lo = int64_t(s[0]) * s[2];
    c.ctx.in[0] = {c.add(la, 0.1), la};
    c.ctx.in[1] = {c.add(lb, 0.2), lb};
    c.ctx.out = {c.add(lo, 0.3), lo};
    c.ctx.in_adj[0] = {c.add(la, 0.4), la};
    c.ctx.in_adj[1] = {c.add(lb, 0.5), lb};
    c.ctx.out_adj_vec = {c.add(lo, 0.6), lo};
    std::snprintf(shape, sizeof shape, "%dx%dx%d", s[0], s[1], s[2]);
    compare("gemm", shape, c, OP_GEMM, OP_GEMM_BLAS, min_ms);
  }

  // matrix-vector product with data X: rows x cols.
  static const int kMatvecShapes[][2] = {
      {32, 32},   {48, 48},    {64, 64},     {96, 96},     {128, 128},
      {256, 256}, {512, 512},  {1024, 1024}, {2048, 2048}, {100, 5},
      {100, 10},  {100, 24},   {100, 64},    {1000, 5},    {1000, 10},
      {1000, 16}, {1000, 24},  {1000, 32},   {1000, 64},   {1000, 100},
      {10000, 5}, {10000, 10}, {10000, 20},  {10000, 100}, {100000, 10},
      {5000, 24}, {20, 1000},  {5, 10000},   {256, 32},    {512, 32},
      {2048, 32}, {5000, 10},  {256, 64},    {300, 40},    {2000, 16}};
  for (const auto& s : kMatvecShapes) {
    Case c;
    c.idata = {s[0], s[1]};
    c.ctx.n_in = 2;
    c.ctx.idata = c.idata.data();
    c.ctx.n_idata = 2;
    const int64_t lx = int64_t(s[0]) * s[1];
    c.ctx.in[0] = {c.add(lx, 0.1), lx};
    c.ctx.in[1] = {c.add(s[1], 0.2), s[1]};
    c.ctx.out = {c.add(s[0], 0.3), s[0]};
    c.ctx.in_adj[0] = {nullptr, lx};
    c.ctx.in_adj[1] = {c.add(s[1], 0.5), s[1]};
    c.ctx.out_adj_vec = {c.add(s[0], 0.6), s[0]};
    std::snprintf(shape, sizeof shape, "%dx%d", s[0], s[1]);
    compare("matvec", shape, c, OP_MATVEC, OP_MATVEC_BLAS, min_ms);
  }

  for (int n : {4,  8,  12, 16, 24,  32,  36,  40,  44,  48,
                56, 64, 80, 96, 128, 192, 256, 384, 512, 1024}) {
    const int64_t nn = int64_t(n) * n;
    Mat m = filled(n, n, 0.3);
    Mat a = (m * m.transpose()).eval() / double(n);
    a.diagonal().array() += 1.0;
    const Mat L = a.llt().matrixL();

    Case c;
    c.idata = {n};
    c.ctx.n_in = 1;
    c.ctx.idata = c.idata.data();
    c.ctx.n_idata = 1;
    double* in = c.add(nn, 0);
    std::copy(a.data(), a.data() + nn, in);
    c.ctx.in[0] = {in, nn};
    c.ctx.out = {c.add(nn, 0.1), nn};
    c.ctx.in_adj[0] = {c.add(nn, 0.2), nn};
    c.ctx.out_adj_vec = {c.add(nn, 0.3), nn};
    c.ctx.scratch = c.add(nn, 0.4);
    std::snprintf(shape, sizeof shape, "%d", n);
    compare("cholesky", shape, c, OP_CHOLESKY, OP_CHOLESKY_BLAS, min_ms);

    Case d;
    d.idata = {n, 1, 1, 0};
    d.ctx.n_in = 3;
    d.ctx.idata = d.idata.data();
    d.ctx.n_idata = 4;
    d.ctx.variant = 0x84;
    d.ctx.in[0] = {d.add(n, 0.5), n};
    d.ctx.in[1] = {d.add(n, 0.6), n};
    double* l = d.add(nn, 0);
    std::copy(L.data(), L.data() + nn, l);
    d.ctx.in[2] = {l, nn};
    d.ctx.in_adj[0] = {nullptr, n};
    d.ctx.in_adj[1] = {nullptr, n};
    d.ctx.in_adj[2] = {d.add(nn, 0.7), nn};
    double out = 0;
    d.ctx.out = {&out, 1};
    d.ctx.out_adj = 1.0;
    d.ctx.scratch = d.add(nn, 0.8);
    compare("multi_normal_cholesky", shape, d, OP_MULTI_NORMAL_CHOL_LPDF,
            OP_MULTI_NORMAL_CHOL_LPDF_BLAS, min_ms);
  }
  return 0;
#endif
}
