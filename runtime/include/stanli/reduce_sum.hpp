#ifndef STANLI_REDUCE_SUM_HPP
#define STANLI_REDUCE_SUM_HPP

#include <stanli/graph.hpp>
#include <memory>
#include <vector>

namespace stanli {
// One context per concurrently evaluated chain. The caller participates;
// threads is the total concurrency, not the number of helper threads.
// Reuse across evaluations and reductions, but never concurrently. Children
// execute without this context, so nested reductions remain serial.
class ReduceExecutionContext {
 public:
  explicit ReduceExecutionContext(int threads);
  ~ReduceExecutionContext();
  ReduceExecutionContext(const ReduceExecutionContext&) = delete;
  ReduceExecutionContext& operator=(const ReduceExecutionContext&) = delete;
  int threads() const;
  void run(size_t count, void* state, void (*work)(void*, size_t));

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Immutable child programs. Bindings name operands, never parent graph slots
// or mutable arena pointers. The kernel state owns all saved forward state.
struct ReduceSumSpec {
  struct Import {
    int slot = -1;
    int input = -1;
    int64_t offset = 0;
  };
  struct Child {
    Graph graph;
    std::vector<std::pair<int, std::vector<double>>> fills;
    std::vector<Import> imports;
  };
  std::vector<Child> children;
};

void register_reduce_sum_kernel();
}  // namespace stanli
#endif
