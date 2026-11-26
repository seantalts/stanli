// Fuzzes the sizes and arrays that the C API passes to make_whiten_spec
// (stanli_model_whiten_lowrank) and to sparse_ldlt and sparse_lowrank
// (stanli_sparse_factor_new and stanli_sparse_factor_lowrank).
//
// These functions take raw pointers, so the harness allocates every array at
// the length the API documents. An n by k array whose n * k does not fit in
// memory cannot be passed by any caller, and the function must refuse it, so
// for those the harness passes a one-element array.
#include <stanli/sparse_factor.hpp>
#include <stanli/whiten.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <vector>

namespace {

struct Bytes {
  const uint8_t* p;
  size_t left;

  // Reads past the end as zero bytes.
  template <class T>
  T take() {
    T v{};
    const size_t m = std::min(left, sizeof(T));
    std::memcpy(&v, p, m);
    p += m;
    left -= m;
    return v;
  }

  // Indices from one signed byte, with the two extremes standing for the
  // int64_t limits.
  int64_t index() {
    const auto b = take<int8_t>();
    if (b == std::numeric_limits<int8_t>::max())
      return std::numeric_limits<int64_t>::max();
    if (b == std::numeric_limits<int8_t>::min())
      return std::numeric_limits<int64_t>::min();
    return b;
  }

  double value() { return take<int8_t>() * 0.25; }

  // Usually a small width, sometimes a full 64-bit one, so that the widths
  // whose n * k overflows are reachable.
  int64_t width() {
    const auto tag = take<uint8_t>();
    return tag & 1 ? take<int64_t>() : tag >> 1;
  }
};

constexpr int64_t kMaxCells = 1 << 16;

// The documented length of an n by k array, 1 when no caller could pass one,
// or -1 when it is too long to fuzz.
int64_t extent(int64_t n, int64_t k) {
  int64_t nk = 0;
  if (n < 0 || k < 0 || __builtin_mul_overflow(n, k, &nk) ||
      (uint64_t)nk > std::vector<double>().max_size())
    return 1;
  return nk > kMaxCells ? -1 : nk;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Bytes in{data, size};
  const int64_t n = in.take<uint8_t>() % 17;
  const auto mode = in.take<uint8_t>();
  const int64_t nnz = in.take<uint8_t>() % 65;
  std::vector<int64_t> perm((size_t)n), col_ptr((size_t)n + 1),
      row_idx((size_t)nnz);
  std::vector<double> q_hat((size_t)n), values((size_t)nnz);
  for (auto& v : perm) v = in.index();
  for (auto& v : col_ptr) v = in.index();
  for (auto& v : row_idx) v = in.index();
  for (auto& v : q_hat) v = in.value();
  for (auto& v : values) v = in.value();
  try {
    if (mode & 1) {
      const int64_t k = in.width();
      const int64_t m = extent(n, k);
      if (m < 0) return 0;
      std::vector<double> c((size_t)n), b((size_t)m), w((size_t)m);
      for (auto& v : c) v = in.value();
      for (auto& v : b) v = in.value();
      for (auto& v : w) v = in.value();
      stanli::make_whiten_spec(q_hat.data(), n, perm.data(), col_ptr.data(),
                               row_idx.data(), values.data(), nnz, k, c.data(),
                               b.data(), w.data());
    } else {
      // sparse_ldlt takes the entry count from col_ptr[n].
      col_ptr[(size_t)n] = nnz;
      const stanli::SparseFactor f =
          stanli::sparse_ldlt(n, col_ptr.data(), row_idx.data(), values.data(),
                              mode & 2 ? nullptr : perm.data());
      const int64_t k = in.width();
      const int64_t m = extent(n, k);
      // sparse_lowrank also allocates k by k.
      if (m < 0 || extent(k, k) < 0) return 0;
      std::vector<double> u((size_t)m);
      for (auto& v : u) v = in.value();
      stanli::sparse_lowrank(f, k, u.data());
    }
  } catch (const std::exception&) {
  }
  return 0;
}
