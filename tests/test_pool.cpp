// The executor pool: one model, many threads.
//
// A sampler that runs chains concurrently evaluates one model handle from
// several threads at once. An Executor cannot be shared -- it owns the
// mutable value and adjoint arenas a sweep writes through -- so the pool
// hands each caller its own clone for the duration of one evaluation and
// takes it back afterwards.
//
// The failure this guards against is silent: two threads sharing one
// executor, or sharing stan-math's autodiff tape, produce numbers rather
// than crashes. So the test compares concurrent results against a
// sequential baseline BITWISE, and does it with enough threads and
// iterations to lose a race if there is one.
#include <stanli/compile.hpp>
#include <stanli/executor_pool.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A point that depends on the index, so a mixed-up executor shows up as a
// wrong number rather than as the same number everywhere.
double point(int64_t i, int k) {
  return 0.1 + 0.05 * (double)(i % 7) + 0.01 * k;
}

}  // namespace

int main() {
  using namespace stanli;
  DataMap data = DataMap::from_json_file("tests/fixtures/eight_schools.json");
  CompiledModel cm = compile_model(slurp("tests/fixtures/es.tmir.sexp"), data);
  Executor proto(std::move(cm.graph));
  cm.bind(proto);
  const int64_t n = proto.n_params();
  const int kIters = 64;

  // Sequential baseline: what every concurrent answer has to match.
  std::vector<double> want_lp(kIters);
  std::vector<std::vector<double>> want_g(kIters);
  {
    for (int k = 0; k < kIters; ++k) {
      for (int64_t i = 0; i < n; ++i) proto.params_data()[i] = point(i, k);
      want_g[(size_t)k].resize((size_t)n);
      want_lp[(size_t)k] = proto.gradient(want_g[(size_t)k].data());
    }
  }

  ExecutorPool pool(proto);
  const int kThreads = 8;
  std::vector<std::vector<double>> got_lp((size_t)kThreads);
  std::vector<std::vector<std::vector<double>>> got_g((size_t)kThreads);
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    got_lp[(size_t)t].resize((size_t)kIters);
    got_g[(size_t)t].resize((size_t)kIters);
    ts.emplace_back([&, t] {
      for (int k = 0; k < kIters; ++k) {
        auto lease = pool.acquire();
        for (int64_t i = 0; i < n; ++i) lease->params_data()[i] = point(i, k);
        got_g[(size_t)t][(size_t)k].resize((size_t)n);
        got_lp[(size_t)t][(size_t)k] =
            lease->gradient(got_g[(size_t)t][(size_t)k].data());
      }
    });
  }
  for (auto& th : ts) th.join();

  for (int t = 0; t < kThreads; ++t)
    for (int k = 0; k < kIters; ++k) {
      if (got_lp[(size_t)t][(size_t)k] != want_lp[(size_t)k]) {
        ++failures;
        std::printf("FAIL thread %d iter %d: lp %.17g want %.17g\n", t, k,
                    got_lp[(size_t)t][(size_t)k], want_lp[(size_t)k]);
        break;
      }
      if (got_g[(size_t)t][(size_t)k] != want_g[(size_t)k]) {
        ++failures;
        std::printf("FAIL thread %d iter %d: gradient differs\n", t, k);
        break;
      }
    }

  // The pool reuses what it is given back rather than cloning per call:
  // eight threads that have all finished leave at most eight clones, and
  // a ninth sequential pass allocates none of them again.
  const size_t after_threads = pool.size();
  {
    auto lease = pool.acquire();
    if (pool.size() != after_threads - 1) {
      ++failures;
      std::printf("FAIL acquire did not take from the free list (%zu -> %zu)\n",
                  after_threads, pool.size());
    }
  }
  if (pool.size() != after_threads) {
    ++failures;
    std::printf("FAIL the lease did not return its executor (%zu -> %zu)\n",
                after_threads, pool.size());
  }
  if (after_threads > (size_t)kThreads) {
    ++failures;
    std::printf("FAIL pool grew past the number of concurrent callers: %zu\n",
                after_threads);
  }

  if (failures == 0) std::printf("test_pool OK\n");
  return failures == 0 ? 0 : 1;
}
