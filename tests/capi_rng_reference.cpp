// Full-output C ABI reference for test_run_rng.py. Keep this in a native
// executable so sanitizer builds initialize their runtime before libstanli.
#include <stanli/capi.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  try {
    if (argc != 3)
      throw std::runtime_error(
          "usage: capi_rng_reference MODEL.tmir.sexp THREADS");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("could not open model MIR");
    const std::string mir((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    char err[8192] = {};
    std::unique_ptr<stanli_model, decltype(&stanli_model_free)> model(
        stanli_model_new(mir.c_str(), "{}", err, sizeof err),
        stanli_model_free);
    if (!model) throw std::runtime_error(err);
    const int64_t width = stanli_wa_n_columns(model.get());
    const int64_t n = stanli_n_unconstrained(model.get());
    if (width <= 0 || n < 0) throw std::runtime_error("invalid model shape");

    // Match the CLI case in test_run_rng.py, but use explicit zero initial
    // vectors to exercise the C API's separate initialization entry point.
    stanli_sample_opts opts;
    stanli_sample_opts_init(&opts);
    opts.seed = 23;
    opts.chains = 2;
    opts.warmup = 30;
    opts.samples = 41;
    opts.thin = 3;
    opts.save_warmup = 1;
    opts.max_depth = 5;
    opts.num_threads = std::stoi(argv[2]);
    opts.init_radius = 0;
    if (opts.num_threads <= 0) throw std::runtime_error("invalid thread count");
    std::vector<double> inits(opts.chains * n, 0.0);
    opts.inits = inits.data();
    const int64_t rows = (opts.warmup + opts.thin - 1) / opts.thin +
                         (opts.samples + opts.thin - 1) / opts.thin;
    const int64_t count = opts.chains * rows;
    std::vector<double> q(count * n), stats(count * 7), values(count * width);
    if (stanli_sample_multi_write_array(model.get(), &opts, 0, q.data(),
                                        stats.data(), values.data(), nullptr,
                                        nullptr, nullptr, nullptr, nullptr,
                                        nullptr, err, sizeof err) != 0)
      throw std::runtime_error(err);

    // Round-trip every double so the Python comparison remains bitwise,
    // including signed zero; rejected output rows retain their NaNs.
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (int64_t i = 0; i < count; ++i) {
      for (int j = 0; j < 7; ++j) {
        if (j) std::cout << ',';
        std::cout << stats[i * 7 + j];
      }
      for (int64_t j = 0; j < width; ++j)
        std::cout << ',' << values[i * width + j];
      std::cout << '\n';
    }
    return std::cout ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "capi_rng_reference: " << e.what() << '\n';
    return 1;
  }
}
