// Cost of one pool lease + gradient on eight schools, from threads in the
// three states a caller can be in: no stan-math stack (a BridgeStan
// caller's own worker under STAN_THREADS), its own ChainableStack (what
// nuts.cpp gives chain threads), and the main thread (registered by
// stan-math's static scheduler observer). Build: cmake --build build-rel
// --target bench_pool; run from the repo root.
#include <stanli/compile.hpp>
#include <stanli/executor_pool.hpp>

#include <stan/math/rev/core/chainablestack.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace stanli;

static std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int main() {
  DataMap data = DataMap::from_json_file("tests/fixtures/eight_schools.json");
  CompiledModel cm = compile_model(slurp("tests/fixtures/es.tmir.sexp"), data);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  ExecutorPool pool(ex);
  const int64_t n = ex.n_params();

  auto body = [&](const char* label) {
    std::vector<double> g((size_t)n);
    volatile double sink = 0;
    const auto once = [&] {
      auto lease = pool.acquire();
      for (int64_t i = 0; i < n; ++i)
        lease->params_data()[i] = 0.05 * (double)i;
      sink = sink + lease->gradient(g.data());
    };
    for (int i = 0; i < 20000; ++i) once();
    const int N = 300000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) once();
    const auto t1 = std::chrono::steady_clock::now();
    const double ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
    std::printf(
        "%-58s %7.1f ns per lease+gradient (instance_ null before: %d)\n",
        label, ns, stan::math::ChainableStack::instance_ == nullptr);
  };

  body("main thread");
  std::thread([&] { body("worker thread, no stack of its own"); }).join();
  std::thread([&] {
    stan::math::ChainableStack own;
    body("worker thread with its own ChainableStack");
  }).join();
  return 0;
}
