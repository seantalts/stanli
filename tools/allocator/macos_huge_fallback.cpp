// Diagnostic-only replacement for private_allocator_macos.cpp. Only malloc
// differs: singleton-sized allocations use the system zone. This is NOT a
// shipping policy; calloc/aligned APIs and ownership-preserving realloc are
// deliberately unchanged to isolate the measured Eigen malloc path.
#include <mimalloc.h>
#include <malloc/malloc.h>
#include <cstdlib>

#if !defined(__APPLE__)
#error "Apple-only allocation-path discriminator"
#endif

extern "C" {
void* malloc(size_t n) {
  // Pinned mimalloc 3.5.1, types.h: MI_LARGE_MAX_OBJ_SIZE is 512 KiB
  // with its default MI_ENABLE_LARGE_PAGES=1. No model-specific threshold.
  return n > 512 * 1024 ? malloc_zone_malloc(malloc_default_zone(), n)
                        : mi_malloc(n);
}
void* calloc(size_t n, size_t size) { return mi_calloc(n, size); }
void free(void* p) {
  if (!p) return;
  if (mi_is_in_heap_region(p)) {
    mi_free(p);
  } else {
    malloc_zone_t* zone = malloc_zone_from_ptr(p);
    if (!zone) std::abort();
    malloc_zone_free(zone, p);
  }
}
void* realloc(void* p, size_t n) {
  if (!p || mi_is_in_heap_region(p)) return mi_realloc(p, n);
  malloc_zone_t* zone = malloc_zone_from_ptr(p);
  if (!zone) std::abort();
  return malloc_zone_realloc(zone, p, n);
}
void* aligned_alloc(size_t alignment, size_t n) {
  return mi_aligned_alloc(alignment, n);
}
int posix_memalign(void** p, size_t alignment, size_t n) {
  return mi_posix_memalign(p, alignment, n);
}
void* valloc(size_t n) { return mi_valloc(n); }
}
