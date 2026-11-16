// Developer switches: the STANLI_* environment variables that turn a pass
// off, force a path, set a tuning limit or print diagnostics, for tests and
// investigation. CMake's STANLI_DEBUG_SWITCHES=OFF compiles every read to
// "unset", so such a runtime cannot change behavior through them. Read them
// while preparing a model or binding an executor, never per evaluation.
#ifndef STANLI_DEBUG_SWITCH_HPP
#define STANLI_DEBUG_SWITCH_HPP

#include <cstdlib>

namespace stanli {

// Says once, through the diagnostic sink, that a set switch is ignored.
void debug_switch_ignored(const char* name);

inline const char* debug_env(const char* name) {
#ifdef STANLI_NO_DEBUG_SWITCHES
  if (std::getenv(name) != nullptr) debug_switch_ignored(name);
  return nullptr;
#else
  return std::getenv(name);
#endif
}

}  // namespace stanli

#endif
