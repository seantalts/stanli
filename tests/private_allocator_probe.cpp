// Test-only exports. These must never be compiled into libstanli.
#include <Eigen/Core>
#include <mimalloc.h>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
void* allocator_fixture_malloc(size_t);
void* allocator_dependency_malloc(size_t);
bool allocator_test_owns(const void* p) { return mi_is_in_heap_region(p); }
void* allocator_test_alloc(size_t n, int kind) {
  switch (kind) {
    case 0:
      return std::malloc(n);
    case 1:
      return std::calloc(n, 1);
    case 2:
      return ::operator new(n, std::nothrow);
    case 5:
      return allocator_fixture_malloc(n);
    case 6:
      return allocator_dependency_malloc(n);
#ifndef _WIN32
    case 3:
      return std::aligned_alloc(64, n);
    case 4: {
      void* p = nullptr;
      return posix_memalign(&p, 64, n) == 0 ? p : nullptr;
    }
#endif
    default:
      return nullptr;
  }
}
void allocator_test_free(void* p) { std::free(p); }
void allocator_test_delete(void* p) { ::operator delete(p); }
void* allocator_test_realloc(void* p, size_t n) { return std::realloc(p, n); }
int allocator_test_eigen() {
  Eigen::VectorXd v(128);
  v.setOnes();
  if (!mi_is_in_heap_region(v.data()) || v.sum() != 128) return 0;
  // Exercise allocations whose implementation can live in external libc++.
  for (int i = 0; i < 64; ++i) {
    try {
      throw std::runtime_error(std::string(256, 'x'));
    } catch (const std::exception& e) {
      if (std::string(e.what()) != std::string(256, 'x')) return 0;
    }
  }
  return 1;
}
int allocator_test_thread() {
  int result = 0;
  // The thread must be created IN the DSO: this caught the rejected private
  // C++ new/delete prototype's external-libc++ TLS destruction mismatch.
  std::thread worker([&] { result = allocator_test_eigen(); });
  worker.join();
  return result;
}
}
