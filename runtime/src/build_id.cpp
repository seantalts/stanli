#include "build_id.hpp"

#ifndef STANLI_BUILD_ID
// A build that did not go through this project's CMake still answers, so a
// caller never has to special-case a missing id.
#define STANLI_BUILD_ID "unknown"
#endif

namespace stanli {

const char* runtime_build_id() { return STANLI_BUILD_ID; }

}  // namespace stanli
