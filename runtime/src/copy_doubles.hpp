#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace stanli {

// memcpy from a null pointer is undefined even for zero bytes, and an empty
// vector's data() may be null: a model with no parameters has empty rows.
inline void copy_doubles(double* to, const double* from, int64_t n) {
  if (n > 0) std::memcpy(to, from, sizeof(double) * (size_t)n);
}

}  // namespace stanli
