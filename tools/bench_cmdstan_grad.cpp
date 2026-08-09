// CmdStan-side per-gradient latency, compiled per model against the
// stanc-generated header (passed via -include). Mirrors what
// stan::model::gradient does per leapfrog step: fresh vars from the
// unconstrained vector, log_prob_propto_jacobian, grad, recover_memory.
// Output: <ns per eval> <lp>
#include <stan/io/json/json_data.hpp>
#include <stan/model/model_base.hpp>
#include <stan/math.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

stan::model::model_base& new_model(stan::io::var_context& data_context,
                                   unsigned int seed, std::ostream* msg_stream);

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: bench_cmdstan_grad data.json N\n");
    return 2;
  }
  const int N = std::atoi(argv[2]);
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

  for (int i = 0; i < 200; ++i) one();
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) one();
  auto t1 = std::chrono::steady_clock::now();
  const double ns =
      std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
  std::printf("%.1f %.17g\n", ns, lp);
  return 0;
}
