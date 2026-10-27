// lowering_digest: compile model.stan against data.json and print a
// deterministic account of the lowered programs (tests/lowering_digest.hpp),
// or time model construction.
//   lowering_digest model.stan data.json [--mir PATH] [--hash]
//   lowering_digest model.stan data.json [--mir PATH] --time K
//   lowering_digest model.stan data.json [--mir PATH] --wa-time K
// --hash prints one line (a hash of the whole account) for diffing many
// models cheaply. --time K constructs the model K times and prints the
// construction time of each, in milliseconds, excluding stanc. --wa-time K
// constructs once and times K write_array evaluations at a fixed point,
// printing nanoseconds per draw.
#include "../tests/lowering_digest.hpp"

#include <stan/math/rev/core/chainablestack.hpp>
#include <stanli/compile.hpp>
#include <stanli/wa_interp.hpp>

#include "stanc_embedded.hpp"
#include "stanc_process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: lowering_digest model.stan data.json [--mir PATH] "
                 "[--hash | --time K]\n");
    return 2;
  }
  std::string mir_path;
  bool hash_only = false;
  int timed = 0;
  int wa_timed = 0;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--mir" && i + 1 < argc)
      mir_path = argv[++i];
    else if (a == "--hash")
      hash_only = true;
    else if (a == "--time" && i + 1 < argc)
      timed = std::atoi(argv[++i]);
    else if (a == "--wa-time" && i + 1 < argc)
      wa_timed = std::atoi(argv[++i]);
    else {
      std::fprintf(stderr, "lowering_digest: unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  std::string mir;
  stanli::DataMap data;
  try {
    mir = mir_path.empty()
              ? stanli::tooling::compile_source("", "", argv[1], false)
              : slurp(mir_path);
    data = stanli::DataMap::from_json_file(argv[2]);
  } catch (const std::exception& e) {
    std::printf("PREP_FAIL %s\n", e.what());
    return 1;
  }
  if (timed > 0) {
    for (int k = 0; k < timed; ++k) {
      const auto t0 = std::chrono::steady_clock::now();
      try {
        stanli::CompiledModel cm = stanli::compile_model(mir, data);
      } catch (const std::exception& e) {
        std::printf("COMPILE_FAIL %s\n", e.what());
        return 1;
      }
      const auto t1 = std::chrono::steady_clock::now();
      std::printf("CONSTRUCT_MS %.3f\n",
                  std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return 0;
  }
  if (wa_timed > 0) {
    using namespace stanli;
    try {
      std::optional<stan::math::ChainableStack> tape;
      if (stan::math::ChainableStack::instance_ == nullptr) tape.emplace();
      CompiledModel cm = compile_model(mir, data);
      if (!cm.write_array) {
        std::printf("NO_WRITE_ARRAY\n");
        return 1;
      }
      Executor ex(Graph(cm.graph));
      cm.bind(ex);
      for (int64_t i = 0; i < ex.n_params(); ++i)
        ex.params_data()[i] = lowering_digest::point(i, 0);
      std::vector<double> grad(static_cast<size_t>(ex.n_params()));
      ex.gradient(grad.data());
      WaRng rng(1234);
      const auto t0 = std::chrono::steady_clock::now();
      if (cm.write_array->interp) {
        const auto env = cm.constrained_env(ex);
        for (int k = 0; k < wa_timed; ++k)
          cm.write_array->interp->eval(env, rng);
      } else {
        Executor wex(Graph(cm.write_array->graph));
        cm.write_array->bind(wex);
        for (int64_t i = 0; i < wex.n_params(); ++i)
          wex.params_data()[i] = lowering_digest::point(i, 0);
        const auto t1 = std::chrono::steady_clock::now();
        for (int k = 0; k < wa_timed; ++k)
          wex.run_forward_only(EvalState{&rng});
        const auto t2 = std::chrono::steady_clock::now();
        std::printf("WA_NS_PER_DRAW %.0f\n",
                    std::chrono::duration<double, std::nano>(t2 - t1).count() /
                        wa_timed);
        return 0;
      }
      const auto t3 = std::chrono::steady_clock::now();
      std::printf(
          "WA_NS_PER_DRAW %.0f\n",
          std::chrono::duration<double, std::nano>(t3 - t0).count() / wa_timed);
    } catch (const std::exception& e) {
      std::printf("WA_TIME_FAIL %s\n", e.what());
      return 1;
    }
    return 0;
  }
  const std::string text = stanli::lowering_digest::digest(mir, data);
  if (hash_only) {
    stanli::lowering_digest::Fnv f;
    f.str(text);
    std::printf("%s\n", stanli::lowering_digest::hex(f.h).c_str());
  } else {
    std::fputs(text.c_str(), stdout);
  }
  return 0;
}
