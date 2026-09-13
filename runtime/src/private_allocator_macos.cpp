// DSO-local C allocation only. The export allowlist keeps these definitions
// private to libstanli; never attach this source to a static/CLI/host target.
// In particular, do NOT add new/delete overrides: libc++ can delete objects
// (notably std::thread's TLS support) that were allocated by inline caller code.
#include <mimalloc.h>
#include <malloc/malloc.h>
#include <cstdlib>

#if !defined(__APPLE__) || !(defined(__aarch64__) || defined(__x86_64__))
#error "This shim requires macOS arm64 or x86_64"
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#error "Use STANLI_SHARED_ALLOCATOR=SYSTEM with sanitizers"
#endif
#endif

extern "C" {
void* malloc(size_t n) { return mi_malloc(n); }
void* calloc(size_t n, size_t size) { return mi_calloc(n, size); }

void free(void* p) {
  if (!p) return;
  if (mi_is_in_heap_region(p)) {
    mi_free(p);
  } else {
    // External libc/libc++ calls can return system-owned buffers. They must
    // go back to their owner, not to mimalloc. Invalid ownership is never
    // guessed. Conversely, our private pointers require our matching free.
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
