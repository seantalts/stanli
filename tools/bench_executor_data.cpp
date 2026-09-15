// Large immutable input, tiny mutable state: isolate per-executor data copies.
// Run baseline and candidate in separate processes. Arguments: elements,
// chains.
#include <stanli/compile.hpp>
#include <stanli/optable.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <sys/resource.h>
int main(int argc, char** argv) {
  using namespace stanli;
  int n = argc > 1 ? std::atoi(argv[1]) : 8000000,
      count = argc > 2 ? std::atoi(argv[2]) : 8;
  if (n <= 0 || count < 1) return 2;
  CompiledModel cm;
  int x = cm.graph.add_slot(1, true), d = cm.graph.add_slot(n, false);
  int z = cm.graph.add_slot(1, false), lp = cm.graph.add_slot(1, false);
  cm.graph.add_op(OP_INDEX, {d}, z, {n / 2});
  cm.graph.add_op(OP_MUL, {x, z}, lp);
  cm.graph.result_slot = lp;
  cm.fills.push_back({d, std::vector<double>(n, 1.25)});
  Executor proto(std::move(cm.graph));
  cm.bind(proto);
  cm.fills.clear();
  auto start = std::chrono::steady_clock::now();
  std::vector<std::unique_ptr<Executor>> copies;
  for (int i = 1; i < count; ++i)
    copies.push_back(std::make_unique<Executor>(proto));
  double ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - start)
                  .count();
  for (auto& e : copies) {
    double grad;
    e->params_data()[0] = 2;
    if (e->gradient(&grad) != 2.5 || grad != 1.25) return 1;
  }
  rusage ru;
  getrusage(RUSAGE_SELF, &ru);
#ifndef __APPLE__
  ru.ru_maxrss *= 1024;  // Linux reports KiB; keep output in bytes.
#endif
  std::printf("elements=%d executors=%d clone_ms=%.6f peak_rss=%ld\n", n, count,
              ms, ru.ru_maxrss);
}
