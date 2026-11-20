// setenv/unsetenv for the tests, portable to Windows: the CRT has
// _putenv_s instead, and passing it an empty value removes the variable,
// which is exactly unsetenv.
#ifndef STANLI_TESTS_ENV_HELPERS_HPP
#define STANLI_TESTS_ENV_HELPERS_HPP

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
inline int test_setenv(const char* k, const char* v, int = 1) {
  return _putenv_s(k, v);
}
inline int test_unsetenv(const char* k) { return _putenv_s(k, ""); }
#else
inline int test_setenv(const char* k, const char* v, int overwrite = 1) {
  return setenv(k, v, overwrite);
}
inline int test_unsetenv(const char* k) { return unsetenv(k); }
#endif

// A build with STANLI_DEBUG_SWITCHES=OFF ignores every STANLI_* switch, so a
// case that sets one to force a path or read a diagnostic has nothing to
// check there. Such a case starts with SKIP_WITHOUT_DEBUG_SWITCHES(), which
// returns from a void function and says so in the log. A case in the middle
// of main() tests debug_switches_available() instead.
inline constexpr bool debug_switches_available() {
#ifdef STANLI_NO_DEBUG_SWITCHES
  return false;
#else
  return true;
#endif
}

inline bool skip_without_debug_switches(const char* what) {
  if (debug_switches_available()) return false;
  std::printf("SKIP %s: needs developer switches\n", what);
  return true;
}

#define SKIP_WITHOUT_DEBUG_SWITCHES()                  \
  do {                                                 \
    if (skip_without_debug_switches(__func__)) return; \
  } while (0)

#endif
