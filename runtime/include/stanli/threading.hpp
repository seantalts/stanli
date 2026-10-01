#ifndef STANLI_THREADING_HPP
#define STANLI_THREADING_HPP

namespace stanli {

// Stan Math's autodiff stack is thread_local only in STAN_THREADS builds.
// Sharing it between workers otherwise corrupts their tapes.
inline bool thread_safe_build() {
#ifdef STAN_THREADS
  return true;
#else
  return false;
#endif
}

}  // namespace stanli

#endif
