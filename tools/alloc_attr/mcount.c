// malloc interposer that counts allocations per call site.
//
//   clang -O1 -g -dynamiclib mcount.c -o libmcount.dylib
//   MCOUNT_OUT=run.txt DYLD_INSERT_LIBRARIES=./libmcount.dylib bench_grad ...
//
// Every malloc, calloc, realloc and posix_memalign records a frame-pointer
// backtrace into a fixed table keyed by the frame addresses, so the
// interposer never allocates. The exit dump lists one line per call site
// and the loaded images, which tools/alloc_attr.py symbolizes.
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 24
#define SKIP 2
#define TABLE (1 << 16)

typedef struct {
  uint64_t hash;
  unsigned long count, bytes, big, maxsz;
  int nf;
  void* f[MAXF];
} entry;

static entry table[TABLE];
static atomic_flag lock = ATOMIC_FLAG_INIT;
static unsigned long total, total_big, dropped;

__attribute__((noinline)) static void record(size_t sz) {
  void* f[MAXF + SKIP];
  int n = backtrace(f, MAXF + SKIP);
  if (n <= SKIP) return;
  n -= SKIP;
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < n; ++i) {
    h ^= (uint64_t)(uintptr_t)f[i + SKIP];
    h *= 1099511628211ull;
  }
  while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {
  }
  total++;
  if (sz > 64) total_big++;
  size_t i = h & (TABLE - 1);
  for (int probe = 0; probe < 64; ++probe, i = (i + 1) & (TABLE - 1)) {
    entry* e = &table[i];
    if (e->count == 0) {
      e->hash = h;
      e->nf = n;
      memcpy(e->f, f + SKIP, sizeof(void*) * n);
    } else if (e->hash != h) {
      continue;
    }
    e->count++;
    e->bytes += sz;
    if (sz > 64) e->big++;
    if (sz > e->maxsz) e->maxsz = sz;
    goto done;
  }
  dropped++;
done:
  atomic_flag_clear_explicit(&lock, memory_order_release);
}

void* my_malloc(size_t s) {
  record(s);
  return malloc(s);
}
void* my_calloc(size_t a, size_t b) {
  record(a * b);
  return calloc(a, b);
}
void* my_realloc(void* p, size_t s) {
  record(s);
  return realloc(p, s);
}
int my_posix_memalign(void** p, size_t al, size_t s) {
  record(s);
  return posix_memalign(p, al, s);
}

__attribute__((destructor)) static void dump(void) {
  const char* path = getenv("MCOUNT_OUT");
  FILE* out = path ? fopen(path, "w") : stderr;
  if (!out) return;
  fprintf(out, "TOTAL %lu BIG %lu DROPPED %lu\n", total, total_big, dropped);
  uint32_t ni = _dyld_image_count();
  for (uint32_t k = 0; k < ni; ++k) {
    Dl_info di;
    const void* hdr = (const void*)_dyld_get_image_header(k);
    if (dladdr(hdr, &di))
      fprintf(out, "IMAGE %p %s\n", di.dli_fbase, di.dli_fname);
  }
  for (size_t i = 0; i < TABLE; ++i) {
    entry* e = &table[i];
    if (e->count == 0) continue;
    fprintf(out, "SITE %lu %lu %lu %lu", e->count, e->bytes, e->big, e->maxsz);
    for (int k = 0; k < e->nf; ++k) fprintf(out, " %p", e->f[k]);
    fprintf(out, "\n");
  }
  if (path) fclose(out);
}

__attribute__((used)) static struct {
  const void* r;
  const void* o;
} interp[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void*)my_malloc, (const void*)malloc},
    {(const void*)my_calloc, (const void*)calloc},
    {(const void*)my_realloc, (const void*)realloc},
    {(const void*)my_posix_memalign, (const void*)posix_memalign}};
