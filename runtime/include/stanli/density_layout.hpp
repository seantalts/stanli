// The integer immediates (Op::idata) of densities whose arguments need more
// than a flat outcome array. Each layout is written by one encoder and read
// by one decoder; outcomes come first so passes that concatenate them keep
// their offsets, and a fixed-size header ends the array.
#ifndef STANLI_DENSITY_LAYOUT_HPP
#define STANLI_DENSITY_LAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stanli {

// Out of line so the decoders below stay small enough to inline into every
// kernel call. Throws std::logic_error.
[[noreturn]] void density_layout_error(const char* what);

// ---- integer groups -------------------------------------------------------
// Integer arguments, one group each: [count_0, values_0..., ...,
// count_{n-1}, values_{n-1}..., scalar_mask]. Bit k of scalar_mask says
// argument k is a language int, which broadcasts; it has count 1. A
// one-element array has count 1 and its bit clear.
struct IntGroup {
  const int* values;
  int64_t count;
  bool scalar;

  // Element n: the scalar for every n, or the array's n-th entry.
  int at(int64_t n) const { return values[scalar ? 0 : n]; }
};

inline constexpr int kMaxIntGroups = 4;

struct IntGroups {
  IntGroup group[kMaxIntGroups];
  int n = 0;
};

inline void append_int_group(std::vector<int>& idata, const int* values,
                             int64_t count) {
  if (count < 0 || count > std::numeric_limits<int>::max())
    throw std::length_error("integer density argument is too large");
  idata.push_back(static_cast<int>(count));
  idata.insert(idata.end(), values, values + count);
}

[[gnu::always_inline]] inline IntGroups int_groups(const int* idata,
                                                   int64_t n_idata,
                                                   int n_groups) {
  if (n_groups < 1 || n_groups > kMaxIntGroups || n_idata < 1)
    density_layout_error("malformed integer density groups");
  IntGroups g;
  g.n = n_groups;
  const int scalar_mask = idata[n_idata - 1];
  int64_t at = 0;
  for (int k = 0; k < n_groups; ++k) {
    if (at >= n_idata - 1)
      density_layout_error("truncated integer density groups");
    const int64_t count = idata[at++];
    if (count < 0 || count > n_idata - 1 - at)
      density_layout_error("malformed integer density groups");
    const bool scalar = (scalar_mask >> k) & 1;
    if (scalar && count != 1)
      density_layout_error("malformed scalar integer density group");
    g.group[k] = {idata + at, count, scalar};
    at += count;
  }
  if (at != n_idata - 1 || (scalar_mask >> n_groups) != 0)
    density_layout_error("trailing integer density groups");
  return g;
}

// ---- GLM ------------------------------------------------------------------
// [outcomes..., rows, cols, flags]. The outcomes are the integer groups
// expanded to one value per instance, group after group.
enum GlmLayoutFlag : int {
  kGlmScalarFirstOutcome = 1 << 0,
  kGlmScalarSecondOutcome = 1 << 1,
  // The design is one row_vector shared by every instance.
  kGlmRowVectorDesign = 1 << 2,
};

struct GlmLayout {
  int64_t rows = 0;
  int64_t cols = 0;
  int flags = 0;
  const int* outcomes = nullptr;
  int64_t n_outcomes = 0;

  bool scalar_outcome(int k) const {
    return (flags &
            (k == 0 ? kGlmScalarFirstOutcome : kGlmScalarSecondOutcome)) != 0;
  }
  bool row_vector_design() const { return (flags & kGlmRowVectorDesign) != 0; }
};

inline constexpr int64_t kGlmHeader = 3;

inline void append_glm_layout(std::vector<int>& idata, int rows, int cols,
                              int flags) {
  idata.push_back(rows);
  idata.push_back(cols);
  idata.push_back(flags);
}

[[gnu::always_inline]] inline GlmLayout glm_layout(const int* idata,
                                                   int64_t n_idata) {
  if (idata == nullptr || n_idata < kGlmHeader)
    density_layout_error("GLM op has no layout");
  const int* h = idata + n_idata - kGlmHeader;
  if (h[0] < 0 || h[1] < 0 ||
      (h[2] & ~(kGlmScalarFirstOutcome | kGlmScalarSecondOutcome |
                kGlmRowVectorDesign)) != 0)
    density_layout_error("malformed GLM layout");
  return {h[0], h[1], h[2], idata, n_idata - kGlmHeader};
}

// ---- vectors or arrays of vectors -------------------------------------------
// [outcomes..., width, count_0, ..., count_{k-1}, array_mask] for k
// vector arguments of `width` elements. Bit j of array_mask says argument j
// is an array of count_j vectors; otherwise it is one vector (count 1).
inline constexpr int kMaxVectorizedArgs = 4;

struct VectorizedLayout {
  int64_t width = 0;
  int64_t count[kMaxVectorizedArgs];  // the first n_args are set
  int array_mask = 0;
  const int* outcomes = nullptr;
  int64_t n_outcomes = 0;

  bool is_array(int j) const { return ((array_mask >> j) & 1) != 0; }
};

inline void append_vectorized_layout(std::vector<int>& idata, int width,
                                     const std::vector<int>& counts,
                                     int array_mask) {
  idata.push_back(width);
  idata.insert(idata.end(), counts.begin(), counts.end());
  idata.push_back(array_mask);
}

[[gnu::always_inline]] inline VectorizedLayout vectorized_layout(
    const int* idata, int64_t n_idata, int n_args) {
  const int64_t header = 2 + n_args;
  if (idata == nullptr || n_args < 1 || n_args > kMaxVectorizedArgs ||
      n_idata < header)
    density_layout_error("density has no vector layout");
  const int* h = idata + n_idata - header;
  VectorizedLayout v;
  v.width = h[0];
  v.array_mask = h[header - 1];
  v.outcomes = idata;
  v.n_outcomes = n_idata - header;
  if (v.width < 0 || (v.array_mask >> n_args) != 0)
    density_layout_error("malformed vector layout");
  for (int j = 0; j < n_args; ++j) {
    v.count[j] = h[1 + j];
    if (v.count[j] < 0 || (!v.is_array(j) && v.count[j] != 1))
      density_layout_error("malformed vector layout");
  }
  return v;
}

}  // namespace stanli

#endif
