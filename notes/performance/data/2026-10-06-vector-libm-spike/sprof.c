// THROWAWAY (spike/vector-libm): LD_PRELOAD sampling profiler used because
// perf_event_paranoid=4 on this machine. A CLOCK_MONOTONIC timer signals the
// main thread; the handler records a timestamp and a short backtrace. At exit
// the samples and /proc/self/maps go to $SPROF_OUT.
#define _GNU_SOURCE
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define DEPTH 12
#define MAXS 400000
struct rec {
  int64_t t;
  int32_t n;
  int32_t pad;
  void* pc[DEPTH];
};
static struct rec* g_buf;
static volatile int g_n;
static timer_t g_timer;
static const volatile int64_t* g_width;

void sprof_register_width(const volatile int64_t* p) { g_width = p; }

static void handler(int sig, siginfo_t* si, void* uc) {
  (void)sig;
  (void)si;
  (void)uc;
  int i = g_n;
  if (i >= MAXS) return;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  struct rec* r = &g_buf[i];
  r->t = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
  r->pad = g_width ? (int32_t)(*g_width > 0x7fffffff ? 0x7fffffff : *g_width) : -1;
  r->n = backtrace(r->pc, DEPTH);
  g_n = i + 1;
}

__attribute__((constructor)) static void sprof_init(void) {
  const char* out = getenv("SPROF_OUT");
  if (!out) return;
  g_buf = calloc(MAXS, sizeof(struct rec));
  void* prime[4];
  backtrace(prime, 4);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGPROF, &sa, NULL);
  struct sigevent sev;
  memset(&sev, 0, sizeof sev);
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGPROF;
  sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
  timer_create(CLOCK_MONOTONIC, &sev, &g_timer);
  const char* hz_s = getenv("SPROF_HZ");
  long hz = hz_s ? atol(hz_s) : 1999;
  struct itimerspec its;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 1000000000L / hz;
  its.it_value = its.it_interval;
  timer_settime(g_timer, 0, &its, NULL);
}

__attribute__((destructor)) static void sprof_fini(void) {
  const char* out = getenv("SPROF_OUT");
  if (!out || !g_buf) return;
  struct itimerspec its;
  memset(&its, 0, sizeof its);
  timer_settime(g_timer, 0, &its, NULL);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t t_end = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
  FILE* f = fopen(out, "wb");
  if (!f) return;
  int32_t n = g_n, depth = DEPTH;
  fwrite(&t_end, sizeof t_end, 1, f);
  fwrite(&n, sizeof n, 1, f);
  fwrite(&depth, sizeof depth, 1, f);
  fwrite(g_buf, sizeof(struct rec), (size_t)n, f);
  fclose(f);
  char path[4096];
  snprintf(path, sizeof path, "%s.maps", out);
  int in = open("/proc/self/maps", O_RDONLY);
  int o = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  char b[65536];
  ssize_t k;
  while (in >= 0 && o >= 0 && (k = read(in, b, sizeof b)) > 0)
    if (write(o, b, (size_t)k) != k) break;
  if (in >= 0) close(in);
  if (o >= 0) close(o);
}
