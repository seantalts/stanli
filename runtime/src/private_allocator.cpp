// ELF/PE: only Stanli's copied archive refers to these names. The final link
// is deliberately NOT wrapped, so these fallback calls and the statically
// linked C++ runtime still use the ordinary process/CRT allocator.
#include <mimalloc.h>
#include <cstdlib>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#error "Use STANLI_SHARED_ALLOCATOR=SYSTEM with sanitizers"
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#error "Use STANLI_SHARED_ALLOCATOR=SYSTEM with sanitizers"
#endif
#endif

extern "C" {
void* stli_mi_malloc(size_t n) { return mi_malloc(n); }
void* stli_mi_calloc(size_t n, size_t size) { return mi_calloc(n, size); }
void stli_mi_free(void* p) {
  if (!p) return;
  if (mi_is_in_heap_region(p)) mi_free(p);
  else std::free(p);
}
void* stli_mi_realloc(void* p, size_t n) {
  if (!p || mi_is_in_heap_region(p)) return mi_realloc(p, n);
  return std::realloc(p, n);
}
void* stli_mi_aligned_alloc(size_t alignment, size_t n) {
  return mi_aligned_alloc(alignment, n);
}
int stli_mi_posix_memalign(void** p, size_t alignment, size_t n) {
  return mi_posix_memalign(p, alignment, n);
}
void* stli_mi_valloc(size_t n) { return mi_valloc(n); }

#ifdef _WIN32
// MinGW headers may call through UCRT import slots rather than direct
// function symbols. Only the copied Stanli slots are renamed; never UCRT's.
void* (*stli_mi_import_malloc)(size_t) = stli_mi_malloc;
void* (*stli_mi_import_calloc)(size_t, size_t) = stli_mi_calloc;
void (*stli_mi_import_free)(void*) = stli_mi_free;
void* (*stli_mi_import_realloc)(void*, size_t) = stli_mi_realloc;
void* (*stli_mi_import_aligned_alloc)(size_t, size_t) = stli_mi_aligned_alloc;
int (*stli_mi_import_posix_memalign)(void**, size_t, size_t) = stli_mi_posix_memalign;
void* (*stli_mi_import_valloc)(size_t) = stli_mi_valloc;
#endif
}
