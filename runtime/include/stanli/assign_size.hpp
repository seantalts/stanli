// stan::model::assign checks a sized destination against the value it
// receives, with CmdStan's messages. An array compares its size and then
// each element's, a vector or matrix compares columns and then rows, and a
// destination with no elements accepts anything.
#ifndef STANLI_ASSIGN_SIZE_HPP
#define STANLI_ASSIGN_SIZE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace stanli {

enum class AssignLeaf { None, Scalar, Vector, RowVector, Matrix };

// The message stan::model::assign throws as std::invalid_argument, or ""
// when the sizes agree. Both dims hold the array extents and then the leaf
// extents: none for a scalar, the length for a vector, rows and columns
// for a matrix.
inline std::string assign_size_mismatch(const std::string& name,
                                        AssignLeaf leaf, size_t levels,
                                        const std::vector<int64_t>& dst,
                                        const std::vector<int64_t>& src) {
  const size_t rank = leaf == AssignLeaf::Matrix   ? 2
                      : leaf == AssignLeaf::Scalar ? 0
                                                   : 1;
  if (leaf == AssignLeaf::None || dst.size() != levels + rank ||
      src.size() != dst.size())
    return "";
  const auto message = [&](const std::string& what, int64_t want,
                           const std::string& rhs, int64_t got) {
    return what + ": assigning variable " + name + " (" + std::to_string(want) +
           ") and right hand side" + rhs + " (" + std::to_string(got) +
           ") must match in size";
  };
  for (size_t d = 0; d < levels; ++d) {
    if (dst[d] == 0) return "";
    if (dst[d] != src[d])
      return message("assign array size", dst[d], "", src[d]);
  }
  int64_t leaf_size = 1;
  for (size_t d = levels; d < dst.size(); ++d) leaf_size *= dst[d];
  if (rank == 0 || leaf_size == 0) return "";
  if (leaf == AssignLeaf::Matrix) {
    if (dst[levels + 1] != src[levels + 1])
      return message("matrix assign columns", dst[levels + 1], " columns",
                     src[levels + 1]);
    if (dst[levels] != src[levels])
      return message("matrix assign rows", dst[levels], " rows", src[levels]);
    return "";
  }
  if (dst[levels] == src[levels]) return "";
  return leaf == AssignLeaf::Vector
             ? message("vector assign rows", dst[levels], " rows", src[levels])
             : message("vector assign columns", dst[levels], " columns",
                       src[levels]);
}

// The message stan::model::assign throws when a selection of one index (a
// range `between`, an open range `upfrom`, or a multi-index) and its
// right-hand side differ in size.
inline std::string selected_assign_size_message(const std::string& name,
                                                bool between, bool upfrom,
                                                bool array, int64_t want,
                                                int64_t got) {
  const bool empty_range = between && want == 0;
  std::string label;
  if (array)
    label = empty_range ? "array[negative_min_max, ...] assign"
                        : "array[multi, ...] assign";
  else if (between)
    label = empty_range ? "vector[negative_min_max] assign"
                        : "vector[min_max] assign";
  else if (upfrom)
    label = "vector[min] assign";
  else
    label = "vector[multi] assign";
  const bool size_word = array && !empty_range;
  return label + ": assigning variable " + name + " (" + std::to_string(want) +
         ") and right hand side" + (size_word ? " size" : "") + " (" +
         std::to_string(got) + ") must match in size";
}

// The message stan::model::assign throws when every index of a vector or
// matrix destination is `:`, or "" when the sizes agree. dst and src hold
// the leaf extents only. Rows come before columns here, unlike a plain
// assignment.
inline std::string omni_assign_size_mismatch(const std::string& name,
                                             AssignLeaf leaf,
                                             const std::vector<int64_t>& dst,
                                             const std::vector<int64_t>& src) {
  const auto message = [&](const std::string& what, int64_t want,
                           const std::string& rhs, int64_t got) {
    return what + ": assigning variable " + name + " (" + std::to_string(want) +
           ") and right hand side" + rhs + " (" + std::to_string(got) +
           ") must match in size";
  };
  if (leaf == AssignLeaf::Matrix && dst.size() == 2 && src.size() == 2) {
    if (dst[0] != src[0])
      return message("matrix[omni] assign rows", dst[0], " rows", src[0]);
    if (dst[1] != src[1])
      return message("matrix[omni] assign columns", dst[1], " columns", src[1]);
    return "";
  }
  if ((leaf == AssignLeaf::Vector || leaf == AssignLeaf::RowVector) &&
      dst.size() == 1 && src.size() == 1 && dst[0] != src[0])
    return message("vector[omni] assign", dst[0], "", src[0]);
  return "";
}

}  // namespace stanli

#endif
