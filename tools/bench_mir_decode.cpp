// Decode-only microbenchmark for comparing MIR wire formats without requiring
// model data or paying graph-lowering time.
//
//   bench_mir_decode MIR [REPETITIONS]
//   bench_mir_decode --paired MIR_A MIR_B [REPETITIONS]
//
// --paired decodes the two inputs in alternating order within each round, so
// a slow interval on a shared runner lands on both, and prints one line per
// input.
#include <stanli/mir_decode.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string slurp(const char* path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

size_t program_size(const stanli::mir::Program& program) {
  return program.input_vars.size() + program.prepare_data.size() +
         program.log_prob.size() + program.generate_quantities.size() +
         program.fun_defs.size() + program.output_vars.size();
}

struct Input {
  std::string wire;
  std::vector<int64_t> samples;
};

int64_t time_decode(const std::string& wire, volatile size_t& checksum) {
  const auto start = std::chrono::steady_clock::now();
  stanli::mir::Program program = stanli::decode_program(wire);
  const auto stop = std::chrono::steady_clock::now();
  checksum += program_size(program);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
      .count();
}

size_t rank(size_t count, size_t percent) {
  return (percent * count + 99) / 100 - 1;
}

void report(const Input& input, int repetitions, size_t checksum) {
  std::vector<int64_t> sorted = input.samples;
  std::sort(sorted.begin(), sorted.end());
  std::printf(
      "bytes=%zu repetitions=%d min_ns=%lld q1_ns=%lld median_ns=%lld "
      "p95_ns=%lld max_ns=%lld checksum=%zu\n",
      input.wire.size(), repetitions, static_cast<long long>(sorted.front()),
      static_cast<long long>(sorted[rank(sorted.size(), 25)]),
      static_cast<long long>(sorted[sorted.size() / 2]),
      static_cast<long long>(sorted[rank(sorted.size(), 95)]),
      static_cast<long long>(sorted.back()), checksum);
}

}  // namespace

int main(int argc, char** argv) {
  const bool paired = argc > 1 && std::string(argv[1]) == "--paired";
  const int files = paired ? 2 : 1;
  const int first = paired ? 2 : 1;
  if (argc < first + files || argc > first + files + 1) {
    std::fprintf(
        stderr,
        "usage: bench_mir_decode MIR [REPETITIONS]\n"
        "       bench_mir_decode --paired MIR_A MIR_B [REPETITIONS]\n");
    return 2;
  }
  const int repetitions =
      argc == first + files + 1 ? std::atoi(argv[first + files]) : 51;
  if (repetitions <= 0) {
    std::fprintf(stderr, "bench_mir_decode: repetitions must be positive\n");
    return 2;
  }
  std::vector<Input> inputs(static_cast<size_t>(files));
  for (int i = 0; i < files; ++i) {
    inputs[i].wire = slurp(argv[first + i]);
    if (inputs[i].wire.empty()) {
      std::fprintf(stderr, "bench_mir_decode: could not read %s\n",
                   argv[first + i]);
      return 1;
    }
    inputs[i].samples.reserve(static_cast<size_t>(repetitions));
  }

  try {
    volatile size_t checksum = 0;
    for (const Input& input : inputs)
      checksum += program_size(stanli::decode_program(input.wire));
    for (int round = 0; round < repetitions; ++round) {
      for (int k = 0; k < files; ++k) {
        Input& input = inputs[(k + round) % files];
        input.samples.push_back(time_decode(input.wire, checksum));
      }
    }
    for (const Input& input : inputs) report(input, repetitions, checksum);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "bench_mir_decode: %s\n", error.what());
    return 1;
  }
  return 0;
}
