// CmdStan-side per-gradient latency, compiled per model against the
// stanc-generated header (passed via -include). Mirrors what
// stan::model::gradient does per leapfrog step: fresh vars from the
// unconstrained vector, log_prob_propto_jacobian, grad, recover_memory.
// Output: <ns per eval> <lp>
#include <stan/io/json/json_data.hpp>
#include <stan/model/model_base.hpp>
#include <stan/math.hpp>
#include "benchmark_timer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

stan::model::model_base& new_model(stan::io::var_context& data_context,
                                   unsigned int seed, std::ostream* msg_stream);

int benchmark_main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: bench_cmdstan_grad data.json N|--timed "
                         "[--warmup-ms N --measure-ms N]\n");
    return 2;
  }
  const bool timed = std::string(argv[2]) == "--timed";
  const int N = timed ? 0 : std::atoi(argv[2]);
  if (!timed && N <= 0) {
    std::fprintf(stderr, "bench_cmdstan_grad: N must be positive\n");
    return 2;
  }
  std::ifstream f(argv[1]);
  stan::json::json_data data(f);
  stan::model::model_base& model = new_model(data, 1, &std::cerr);

  const int64_t n = static_cast<int64_t>(model.num_params_r());
  std::vector<double> q(n), grad(n);
  for (int64_t i = 0; i < n; ++i)
    q[i] = 0.1 + 0.05 * static_cast<double>(i % 7) -
           0.15 * static_cast<double>(i % 3);

  double lp = 0;
  auto one = [&]() {
    Eigen::Matrix<stan::math::var, -1, 1> qv(n);
    for (int64_t i = 0; i < n; ++i) qv(i) = q[i];
    stan::math::var v = model.log_prob_propto_jacobian(qv, &std::cerr);
    v.grad();
    lp = v.val();
    for (int64_t i = 0; i < n; ++i) grad[i] = qv(i).adj();
    stan::math::recover_memory();
  };

  // Warm by elapsed time, as bench_grad does.  A fixed count is much too
  // short for small ODE systems and makes fresh-process paired measurements
  // depend on which arm happened to run first on a cold core.
  if (timed) {
    const auto opts = stanli_benchmark::options(argc, argv, 3);
    one();
    if (!std::isfinite(lp) ||
        !std::all_of(grad.begin(), grad.end(), [](double x) { return std::isfinite(x); }))
      throw std::runtime_error("benchmark point has non-finite density or gradient");
    const auto warm = stanli_benchmark::window(one, opts.warmup_ns, 1, true);
    const auto measured = stanli_benchmark::window(one, opts.measure_ns, warm.batch);
    stanli_benchmark::output(warm, measured, lp, grad);
    return 0;
  }
  stanli_benchmark::window(one, 200'000'000, 1, true);
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) one();
  auto t1 = std::chrono::steady_clock::now();
  const double ns =
      std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
  std::printf("%.1f %.17g\n", ns, lp);
  return 0;
}

int main(int argc, char** argv) {
  try { return benchmark_main(argc, argv); }
  catch (const std::exception& error) {
    std::fprintf(stderr, "bench_cmdstan_grad: %s\n", error.what());
    return 1;
  }
}
