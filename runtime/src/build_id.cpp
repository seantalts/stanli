#include "build_id.hpp"

#ifndef STANLI_BUILD_ID
// A build that did not go through this project's CMake still answers, so a
// caller never has to special-case a missing id.
#define STANLI_BUILD_ID "unknown"
#endif

namespace stanli {

const char* runtime_build_id() {
#ifdef STANLI_PRIVATE_MIMALLOC
  return STANLI_BUILD_ID "-mimalloc-3.5.1-private-c";
#else
  return STANLI_BUILD_ID;
#endif
}

}  // namespace stanli
