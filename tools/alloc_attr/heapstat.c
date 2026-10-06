// Prints live malloc bytes (all zones) and peak RSS when the process exits.
//   clang -O1 -dynamiclib heapstat.c -o libheapstat.dylib
//   DYLD_INSERT_LIBRARIES=./libheapstat.dylib bench_grad ...
#include <malloc/malloc.h>
#include <stdio.h>
#include <sys/resource.h>

__attribute__((destructor)) static void report(void) {
  malloc_statistics_t st;
  malloc_zone_statistics(NULL, &st);
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  fprintf(stderr, "HEAPSTAT live_bytes=%zu live_blocks=%u max_rss_bytes=%ld\n",
          st.size_in_use, st.blocks_in_use, (long)ru.ru_maxrss);
}
