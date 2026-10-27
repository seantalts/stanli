// lowering_digest: compile model.stan against data.json and print a
// deterministic account of the lowered programs (tests/lowering_digest.hpp),
// or time model construction.
//   lowering_digest model.stan data.json [--mir PATH] [--hash]
//   lowering_digest model.stan data.json [--mir PATH] --time K
// --hash prints one line (a hash of the whole account) for diffing many
// models cheaply. --time K constructs the model K times and prints the
// construction time of each, in milliseconds, excluding stanc.
#include "../tests/lowering_digest.hpp"

#include <stanli/compile.hpp>

#include "stanc_embedded.hpp"
#include "stanc_process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--mir" && i + 1 < argc)
      mir_path = argv[++i];
    else if (a == "--hash")
      hash_only = true;
    else if (a == "--time" && i + 1 < argc)
      timed = std::atoi(argv[++i]);
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
