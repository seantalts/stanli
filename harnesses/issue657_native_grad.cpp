// C ABI counterpart to tools/bench_cmdstan_grad.cpp. Both use the exact
// same native-loop timer and point protocol; model setup is outside timing.
#include <stanli/capi.h>

#include "../tools/benchmark_timer.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static std::string read_file(const char* path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error(std::string("cannot read ") + path);
  return {std::istreambuf_iterator<char>(in), {}};
}

int main(int argc, char** argv) {
  try {
    if (argc < 3) {
      std::cerr << "usage: issue657_native_grad model.stan data.json [timer "
                   "options]\n";
      return 2;
    }
    const std::string source = read_file(argv[1]);
    const std::string data = read_file(argv[2]);
    char error[8192]{};
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::unique_ptr<char, decltype(&stanli_string_free)> mir(
        stanli_stan_to_mir_with_opts(source.c_str(), nullptr, 0, 0, error,
                                     sizeof(error)),
        stanli_string_free);
    if (!mir) throw std::runtime_error(error);
    const auto compiled = Clock::now();
    stanli_model_opts options;
    stanli_model_opts_init(&options);
    options.allow_interpreter = 0;
    std::unique_ptr<stanli_model, decltype(&stanli_model_free)> model(
        stanli_model_new_with_opts(mir.get(), data.c_str(), &options, error,
                                   sizeof(error)),
        stanli_model_free);
    if (!model) throw std::runtime_error(error);
    const auto prepared = Clock::now();
    const int64_t n = stanli_n_unconstrained(model.get());
    std::vector<double> q(static_cast<size_t>(n)), grad(q.size());
    double lp = 0;
    const auto one = [&] {
#ifdef STANLI_BENCH_LEGACY_GRAD
      // Older ABI6 releases predate grad_err. Compile both comparison arms
      // with this flag when measuring those releases, keeping the same call.
      const int status = stanli_grad(model.get(), q.data(), &lp, grad.data());
      if (status != 0)
        throw std::runtime_error("stanli_grad failed with status " +
                                 std::to_string(status));
#else
      if (stanli_grad_err(model.get(), q.data(), &lp, grad.data(), error,
                          sizeof(error)) != STANLI_EVAL_OK)
        throw std::runtime_error(error);
#endif
    };
    one();
    const auto first = Clock::now();
    std::cerr << "build_id=" << stanli_build_id() << " compile_s="
              << std::chrono::duration<double>(compiled - start).count()
              << " preparation_s="
              << std::chrono::duration<double>(prepared - compiled).count()
              << " first_gradient_s="
              << std::chrono::duration<double>(first - prepared).count()
              << '\n';
    const auto timer = stanli_benchmark::options(argc, argv, 3);
    stanli_benchmark::timed(
        timer, n, [&](const std::vector<double>& point) { q = point; }, one,
        [&] {
          std::vector<double> values{lp};
          values.insert(values.end(), grad.begin(), grad.end());
          return values;
        });
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "issue657_native_grad: " << e.what() << '\n';
    return 1;
  }
}
