// Test-only complete native gradients. Derived from the retained allocator
// evaluator: same points, persistent workers, tape initialization and snapshots.
// Never compiled into or installed with the shipping library.
#include <stanli/compile.hpp>
#include <stanli/model_adapter.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#else
#include <sys/resource.h>
#endif
#ifdef STANLI_PRIVATE_MIMALLOC
#include <mimalloc.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}
static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
static std::string slurp(const char* path) {
  std::ifstream file(path);
  require(bool(file), "input not found");
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}
static uint64_t bits(double x) {
  uint64_t result;
  std::memcpy(&result, &x, sizeof(x));
  return result;
}
static void memory(const char* stage, int cycle, int sample = -1) {
  uint64_t rss = 0, peak = 0;
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS info{};
  require(GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)), "RSS query failed");
  rss = info.WorkingSetSize;
  peak = info.PeakWorkingSetSize;
#elif defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  require(task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS,
          "RSS query failed");
  rusage usage{};
  require(getrusage(RUSAGE_SELF, &usage) == 0, "peak RSS query failed");
  rss = info.resident_size;
  peak = usage.ru_maxrss;
#else
  rusage usage{};
  require(getrusage(RUSAGE_SELF, &usage) == 0, "peak RSS query failed");
  peak = uint64_t(usage.ru_maxrss) * 1024;
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.compare(0, 6, "VmRSS:") == 0) {
      std::istringstream value(line.substr(6));
      value >> rss;
      rss *= 1024;
    }
  }
#endif
  std::printf("{\"kind\":\"memory\",\"stage\":\"%s\",\"cycle\":%d,\"sample\":%d,"
              "\"rss_bytes\":%llu,\"peak_rss_bytes\":%llu}\n", stage, cycle, sample,
              static_cast<unsigned long long>(rss),
              static_cast<unsigned long long>(peak));
}
static bool private_owns(const void* p) {
#ifdef STANLI_PRIVATE_MIMALLOC
  return mi_is_in_heap_region(p);
#else
  (void)p;
  return false;
#endif
}
static void ownership(const char* allocator) {
  void* p = std::malloc(256);
  require(p != nullptr, "malloc failed");
  require(private_owns(p) == (std::strcmp(allocator, "system") != 0),
          "DSO-local allocator ownership mismatch");
  std::free(p);
}

class Barrier {
  std::mutex mutex;
  std::condition_variable ready;
  int total, remaining, generation = 0;
 public:
  explicit Barrier(int count) : total(count), remaining(count) {}
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    const int old = generation;
    if (--remaining == 0) {
      remaining = total;
      ++generation;
      ready.notify_all();
    } else {
      ready.wait(lock, [&] { return generation != old; });
    }
  }
};

struct Chain {
  std::unique_ptr<stanli::Executor> executor;
  std::vector<Eigen::VectorXd> points;
  Eigen::VectorXd gradient;
  std::vector<std::vector<uint64_t>> snapshots;
  double lp = 0;
  double first_ns = 0;
  explicit Chain(const stanli::Executor& source, int chain)
      : executor(std::make_unique<stanli::Executor>(source)),
        gradient(source.n_params()) {
    for (int point = 0; point < 8; ++point) {
      Eigen::VectorXd q(source.n_params());
      for (int64_t i = 0; i < source.n_params(); ++i)
        q[i] = 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)
               + 0.002 * (point - 3) * (1 + i % 3) + 0.001 * chain;
      points.push_back(std::move(q));
    }
    snapshots.resize(8);
  }
  void one(int index) {
    stanli::ExecutorModel model(*executor);
    stan::callbacks::logger logger;
    stan::model::gradient(model, points[index & 7], lp, gradient, logger);
    asm volatile("" : : "g"(gradient.data()), "g"(&lp) : "memory");
  }
  std::vector<uint64_t> snapshot(int point) {
    one(point);
    require(std::isfinite(lp) && gradient.allFinite(), "nonfinite gradient at test point");
    std::vector<uint64_t> result{bits(lp)};
    for (double x : gradient) result.push_back(bits(x));
    return result;
  }
};

static void run_model(const std::string& mir, const std::string& json,
                      int chains, int reps, int samples, int cycle,
                      FILE* snapshots, const char* allocator) {
  const auto begin = Clock::now();
  auto data = stanli::DataMap::from_json(json);
  auto compiled = stanli::compile_model(mir, data);
  stanli::Executor base(std::move(compiled.graph));
  compiled.bind(base);
  size_t slot_elements = 0;
  for (const auto& slot : base.graph().slots) slot_elements += slot.len;
  std::vector<std::unique_ptr<Chain>> workers;
  for (int c = 0; c < chains; ++c) workers.push_back(std::make_unique<Chain>(base, c));
  const double prep_ns = elapsed(begin);
  std::printf("{\"kind\":\"prepare\",\"cycle\":%d,\"ns\":%.1f,\"params\":%lld,"
              "\"ops\":%zu,\"slots\":%zu,\"slot_elements\":%zu,\"adjoint_elements\":%lld}\n",
              cycle, prep_ns, static_cast<long long>(base.n_params()), base.graph().ops.size(),
              base.graph().slots.size(), slot_elements,
              static_cast<long long>(base.adjoint_storage_size()));
  memory("prepared", cycle);
  Barrier barrier(chains + 1);
  std::vector<std::thread> threads;
  for (int c = 0; c < chains; ++c) threads.emplace_back([&, c] {
    // Same thread-local Stan tape initialization as production's chain workers.
    stan::math::ChainableStack tape;
    ownership(allocator);
    Chain& chain = *workers[c];
    const auto first = Clock::now();
    chain.one(0);
    chain.first_ns = elapsed(first);
    for (int p = 0; p < 8; ++p) chain.snapshots[p] = chain.snapshot(p);
    const auto warm = Clock::now();
    for (int i = 0; i < 1000; ++i) {
      chain.one(i);
      if (i >= 31 && elapsed(warm) > 20000000) break;
    }
    for (int s = 0; s < samples; ++s) {
      barrier.wait();  // warm/previous block complete
      barrier.wait();  // timed start
      for (int i = 0; i < reps; ++i) chain.one(i);
      barrier.wait();  // timed finish
      barrier.wait();  // master has recorded time and memory
    }
    for (int p = 0; p < 8; ++p)
      require(chain.snapshot(p) == chain.snapshots[p], "gradient changed after repeated evaluation");
  });
  for (int s = 0; s < samples; ++s) {
    barrier.wait();
    if (s == 0) memory("warmed", cycle);
    const auto start = Clock::now();
    barrier.wait();
    barrier.wait();
    const double wall = elapsed(start);
    std::printf("{\"kind\":\"timing\",\"cycle\":%d,\"sample\":%d,\"reps\":%d,"
                "\"chains\":%d,\"wall_ns\":%.1f,\"ns_per_gradient\":%.4f}\n",
                cycle, s, reps, chains, wall, wall / (reps * double(chains)));
    memory("measured", cycle, s);
    barrier.wait();
  }
  for (auto& thread : threads) thread.join();
  // Check each thread's complete results against the same executor on the main thread.
  for (int c = 0; c < chains; ++c) {
    Chain& chain = *workers[c];
    for (int p = 0; p < 8; ++p) {
      require(chain.snapshot(p) == chain.snapshots[p], "parallel/serial snapshot mismatch");
      if (snapshots) {
        std::fprintf(snapshots, "%d:%d:%d", cycle, c, p);
        for (auto x : chain.snapshots[p])
          std::fprintf(snapshots, ":%016llx", static_cast<unsigned long long>(x));
        std::fputc('\n', snapshots);
      }
    }
    std::printf("{\"kind\":\"first_gradient\",\"cycle\":%d,\"chain\":%d,\"ns\":%.1f}\n",
                cycle, c, chain.first_ns);
  }
  memory("joined", cycle);
}

#ifdef _WIN32
#define ALLOCATOR_BENCH_EXPORT __declspec(dllexport)
#else
#define ALLOCATOR_BENCH_EXPORT __attribute__((visibility("default")))
#endif
extern "C" ALLOCATOR_BENCH_EXPORT bool stanli_allocator_bench_owns(const void* p) {
  return private_owns(p);
}
extern "C" ALLOCATOR_BENCH_EXPORT int stanli_allocator_bench_run(int argc, char** argv) {
  try {
    require(argc == 9 || argc == 10,
            "usage: full_model mir data allocator chains reps samples cycles [snapshots]");
    const int chains = std::stoi(argv[4]), reps = std::stoi(argv[5]);
    const int samples = std::stoi(argv[6]), cycles = std::stoi(argv[7]);
    // argv[8] is an explicit run label reserved for the driver's manifest.
    require(chains > 0 && chains <= 32 && reps > 0 && samples > 0 && cycles > 0,
            "invalid benchmark counts");
    ownership(argv[3]);
    const std::string mir = slurp(argv[1]), json = slurp(argv[2]);
    FILE* snapshots = argc == 10 ? std::fopen(argv[9], "wb") : nullptr;
    require(argc != 10 || snapshots, "cannot open snapshots");
    memory("start", -1);
    for (int cycle = 0; cycle < cycles; ++cycle) {
      run_model(mir, json, chains, reps, samples, cycle, snapshots, argv[3]);
      memory("destroyed", cycle);
    }
    if (snapshots) std::fclose(snapshots);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
