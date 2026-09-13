#include <cstdlib>
// A separate static dependency, not selected for allocation rewriting.
extern "C" void* allocator_dependency_malloc(size_t n) {
  return std::malloc(n);
}
