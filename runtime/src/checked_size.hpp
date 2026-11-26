#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stanli {

// a * b for non-negative a and b, or invalid_argument(what) when the product
// would not fit in an int64_t or in a std::vector<double>. The vector bound
// is the tighter one where size_t has 32 bits (wasm32).
inline int64_t checked_size(int64_t a, int64_t b, const char* what) {
  const uint64_t limit = std::min<uint64_t>(std::numeric_limits<int64_t>::max(),
                                            std::vector<double>().max_size());
  if (a == 0 || b == 0) return 0;
  if ((uint64_t)a > limit / (uint64_t)b) throw std::invalid_argument(what);
  return a * b;
}

}  // namespace stanli
