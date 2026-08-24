// Per-gradient latency at a fixed point, or file-to-bound-executor latency
// with --prep (no warmup or model evaluation).
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string slurp(const char* p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: bench_grad mir.sexp data.json N|--prep\n");
    return 2;
  }
  const bool prep_only = std::string(argv[3]) == "--prep";
  const int N = prep_only ? 0 : std::atoi(argv[3]);
  if (!prep_only && N <= 0) {
    std::fprintf(stderr, "bench_grad: N must be positive\n");
    return 2;
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
  for (int64_t i = 0; i < n; ++i)
    ex.params_data()[i] = 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3);
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
