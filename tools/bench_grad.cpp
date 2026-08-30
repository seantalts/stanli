// Per-gradient latency at a fixed point, or file-to-bound-executor latency
// with --prep (no warmup or model evaluation).
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

static std::string slurp(const char* p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: bench_grad mir.sexp data.json N|--prep "
                 "[--point 0|1|2] [--set-param INDEX VALUE]...\n");
    return 2;
  }
  const bool prep_only = std::string(argv[3]) == "--prep";
  int N = 0;
  if (!prep_only) {
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(argv[3], &end, 10);
    if (errno != 0 || end == argv[3] || *end != '\0' || parsed <= 0 ||
        parsed > std::numeric_limits<int>::max()) {
      std::fprintf(stderr, "bench_grad: N must be a positive integer\n");
      return 2;
    }
    N = static_cast<int>(parsed);
  }
  std::vector<std::pair<int64_t, double>> param_overrides;
  std::unordered_set<int64_t> overridden;
  int point = 0;
  bool point_set = false;
  for (int arg = 4; arg < argc;) {
    if (std::string(argv[arg]) == "--point") {
      if (prep_only || point_set || arg + 1 >= argc ||
          (std::string(argv[arg + 1]) != "0" &&
           std::string(argv[arg + 1]) != "1" &&
           std::string(argv[arg + 1]) != "2")) {
        std::fprintf(stderr,
                     "bench_grad: --point requires one of 0, 1, 2 and "
                     "may appear once in evaluation mode\n");
        return 2;
      }
      point = argv[arg + 1][0] - '0';
      point_set = true;
      arg += 2;
      continue;
    }
    if (std::string(argv[arg]) != "--set-param") {
      std::fprintf(stderr, "bench_grad: unknown argument: %s\n", argv[arg]);
      return 2;
    }
    if (prep_only) {
      std::fprintf(stderr, "bench_grad: --set-param is invalid with --prep\n");
      return 2;
    }
    if (arg + 2 >= argc) {
      std::fprintf(stderr,
                   "bench_grad: --set-param requires INDEX and VALUE\n");
      return 2;
    }
    errno = 0;
    char* index_end = nullptr;
    const long long parsed_index = std::strtoll(argv[arg + 1], &index_end, 10);
    if (errno != 0 || index_end == argv[arg + 1] || *index_end != '\0' ||
        parsed_index < 0) {
      std::fprintf(stderr,
                   "bench_grad: parameter index must be a nonnegative "
                   "integer: %s\n",
                   argv[arg + 1]);
      return 2;
    }
    errno = 0;
    char* value_end = nullptr;
    const double value = std::strtod(argv[arg + 2], &value_end);
    if (errno != 0 || value_end == argv[arg + 2] || *value_end != '\0' ||
        !std::isfinite(value)) {
      std::fprintf(stderr, "bench_grad: parameter value must be finite: %s\n",
                   argv[arg + 2]);
      return 2;
    }
    const int64_t index = static_cast<int64_t>(parsed_index);
    if (!overridden.insert(index).second) {
      std::fprintf(stderr,
                   "bench_grad: parameter index specified more than once: "
                   "%lld\n",
                   parsed_index);
      return 2;
    }
    param_overrides.emplace_back(index, value);
    arg += 3;
  }
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;
  const char* prep_env = std::getenv("STANLI_PROFILE_PREP");
  const bool prep_profile = prep_env && prep_env[0] != '0';
  const bool measure_prep = prep_profile || prep_only;
  const auto prep_now = [&]() { return measure_prep ? Clock::now() : Time{}; };
  const auto prep_ns = [&](Time from) {
    return measure_prep ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                              Clock::now() - from)
                              .count()
                        : int64_t{0};
  };
  const Time driver_start = prep_now();
  Time pt = prep_now();
  int64_t data_bytes = 0;
  int64_t read_data_ns = 0;
  int64_t parse_data_ns = 0;
  stanli::DataMap data;
  {
    const std::string data_text = slurp(argv[2]);
    read_data_ns = prep_ns(pt);
    data_bytes = static_cast<int64_t>(data_text.size());
    pt = prep_now();
    data = stanli::DataMap::from_json(data_text);
    parse_data_ns = prep_ns(pt);
  }
  int64_t mir_bytes = 0;
  int64_t read_mir_ns = 0;
  int64_t compile_ns = 0;
  stanli::CompiledModel cm;
  {
    pt = prep_now();
    const std::string mir_text = slurp(argv[1]);
    read_mir_ns = prep_ns(pt);
    mir_bytes = static_cast<int64_t>(mir_text.size());
    pt = prep_now();
    cm = stanli::compile_model(mir_text, data);
    compile_ns = prep_ns(pt);
  }
  pt = prep_now();
  stanli::Executor ex(std::move(cm.graph));
  const int64_t executor_ns = prep_ns(pt);
  pt = prep_now();
  cm.bind(ex);
  const int64_t bind_ns = prep_ns(pt);
  const auto report_driver = [&](int64_t total_ns, int64_t warm_ns,
                                 int warm_evals) {
    if (!prep_profile) return;
    std::fprintf(
        stderr, "stanli_prep graph=driver stage=read_data ns=%lld bytes=%lld\n",
        (long long)read_data_ns, (long long)data_bytes);
    std::fprintf(stderr, "stanli_prep graph=driver stage=parse_data ns=%lld\n",
                 (long long)parse_data_ns);
    std::fprintf(stderr,
                 "stanli_prep graph=driver stage=read_mir ns=%lld bytes=%lld\n",
                 (long long)read_mir_ns, (long long)mir_bytes);
    std::fprintf(stderr, "stanli_prep graph=driver stage=compile ns=%lld\n",
                 (long long)compile_ns);
    std::fprintf(stderr, "stanli_prep graph=driver stage=executor ns=%lld\n",
                 (long long)executor_ns);
    std::fprintf(stderr, "stanli_prep graph=driver stage=bind ns=%lld\n",
                 (long long)bind_ns);
    if (warm_ns >= 0)
      std::fprintf(stderr,
                   "stanli_prep graph=driver stage=warmup ns=%lld evals=%d\n",
                   (long long)warm_ns, warm_evals);
    std::fprintf(stderr, "stanli_prep graph=driver stage=total ns=%lld\n",
                 (long long)total_ns);
  };
  if (prep_only) {
    const int64_t total_ns = prep_ns(driver_start);
    report_driver(total_ns, -1, 0);
    const double seconds = total_ns / 1e9;
    std::printf("%.9f %lld\n", seconds, (long long)ex.n_params());
    return 0;
  }
  // STANLI_PROFILE=1: per-opcode time/count/element accounting on stderr.
  // Enabled for the measured loop only, so warmup does not pollute it.
  const char* prof_env = std::getenv("STANLI_PROFILE");
  const bool profile = prof_env && prof_env[0] != '0';
  const int64_t n = ex.n_params();
  // Same point ladder as stanli_check and ref_driver. The corpus harness
  // chooses a single jointly valid point, never a different point per build.
  for (int64_t i = 0; i < n; ++i) {
    ex.params_data()[i] = point == 2   ? 0.0
                          : point == 1 ? 0.02 * static_cast<double>((i % 5) - 2)
                                       : 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3);
  }
  for (const auto& override : param_overrides) {
    if (override.first >= n) {
      std::fprintf(stderr,
                   "bench_grad: parameter index %lld is out of range for "
                   "%lld parameters\n",
                   (long long) override.first, (long long)n);
      return 2;
    }
    ex.params_data()[override.first] = override.second;
  }
  std::vector<double> grad(n);
  double sink = 0;
  // Warm up by time, not by count: 1000 evaluations is nothing on a scalar
  // model and 90 seconds on an ODE one.
  int warmup_evals = 0;
  const Time warmup_start = prep_now();
  {
    auto w0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
      sink += ex.gradient(grad.data());
      ++warmup_evals;
      if (std::chrono::steady_clock::now() - w0 >
          std::chrono::milliseconds(200))
        break;
    }
  }
  const int64_t warmup_ns = prep_ns(warmup_start);
  const int64_t driver_ns = prep_ns(driver_start);
  if (profile) ex.set_profile(true);
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) sink += ex.gradient(grad.data());
  auto t1 = std::chrono::steady_clock::now();
  if (profile) std::fprintf(stderr, "%s", ex.profile_report().c_str());
  const double ns =
      std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
  // Forward-only, for splitting a cost between the two sweeps.
  auto t2 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) ex.run_forward_only();
  auto t3 = std::chrono::steady_clock::now();
  const double fwd_ns =
      std::chrono::duration<double, std::nano>(t3 - t2).count() / N;
  report_driver(driver_ns, warmup_ns, warmup_evals);
  // Machine-readable: <ns/grad> <sink> <ns/forward> <n_params>, consumed by
  // tools/bench_models.py (which reads field 0 and the last field).
  std::printf("%.1f %.6g %.1f %lld\n", ns, sink, fwd_ns, (long long)n);
  return 0;
}
