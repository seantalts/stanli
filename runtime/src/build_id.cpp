#include "build_id.hpp"

#include <stanli/capi.h>

#ifndef STANLI_BUILD_SOURCE
// A build that did not go through this project's CMake still answers, so a
// caller never has to special-case a missing id.
#define STANLI_BUILD_SOURCE "unknown"
#endif

#define STANLI_STRINGIFY_(x) #x
#define STANLI_STRINGIFY(x) STANLI_STRINGIFY_(x)

namespace stanli {

const char* runtime_build_id() {
  return "abi" STANLI_STRINGIFY(STANLI_ABI_VERSION) "-" STANLI_BUILD_SOURCE;
}

}  // namespace stanli
