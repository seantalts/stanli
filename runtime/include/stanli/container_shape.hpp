#ifndef STANLI_CONTAINER_SHAPE_HPP
#define STANLI_CONTAINER_SHAPE_HPP

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace stanli {

// Validate every extent before multiplying: zero or paired negative extents
// must not hide an invalid dimension. Used before allocating either layout.
inline int64_t checked_container_size(const std::vector<int64_t>& dims,
                                      const std::string& function) {
  for (int64_t d : dims)
    if (d < 0) throw std::domain_error(function + ": negative extent");
  int64_t size = 1;
  for (int64_t d : dims) {
    if (d != 0 && size > std::numeric_limits<int64_t>::max() / d)
      throw std::domain_error(function + ": overflowing extent");
    size *= d;
  }
  return size;
}

// MIR/DataMap buffers follow Stan's serialized order: the first declared
// index is fastest. Graph buffers keep every outer-array element contiguous,
// with vector leaves contiguous and matrix leaves column-major. Keep the
// permutation here so every bridge between the two representations agrees.
template <typename T>
std::vector<T> graph_container_order(const std::vector<T>& source,
                                     const std::vector<int64_t>& dims,
                                     size_t outer_rank) {
  if (outer_rank > dims.size() || dims.size() - outer_rank > 2)
    throw std::domain_error("container layout: invalid leaf rank");
  const int64_t size = checked_container_size(dims, "container layout");
  if ((uint64_t)size != source.size())
    throw std::domain_error("container layout: size mismatch");
  std::vector<T> result(source.size());
  std::vector<int64_t> index(dims.size());
  for (size_t serialized = 0; serialized < source.size(); ++serialized) {
    int64_t rest = (int64_t)serialized;
    for (size_t d = 0; d < dims.size(); ++d) {
      index[d] = rest % dims[d];
      rest /= dims[d];
    }
    int64_t graph = 0;
    for (size_t d = 0; d < outer_rank; ++d) graph = graph * dims[d] + index[d];
    int64_t leaf_size = 1;
    for (size_t d = outer_rank; d < dims.size(); ++d) leaf_size *= dims[d];
    graph *= leaf_size;
    if (dims.size() == outer_rank + 1) {
      graph += index[outer_rank];
    } else if (dims.size() == outer_rank + 2) {
      graph += index[outer_rank + 1] * dims[outer_rank] + index[outer_rank];
    }
    result[(size_t)graph] = source[serialized];
  }
  return result;
}

template <typename T>
std::vector<T> serialized_container_order(const std::vector<T>& graph_source,
                                          const std::vector<int64_t>& dims,
                                          size_t outer_rank) {
  if (outer_rank > dims.size() || dims.size() - outer_rank > 2)
    throw std::domain_error("container layout: invalid leaf rank");
  const int64_t size = checked_container_size(dims, "container layout");
  if ((uint64_t)size != graph_source.size())
    throw std::domain_error("container layout: size mismatch");
  std::vector<T> result(graph_source.size());
  std::vector<int64_t> index(dims.size());
  for (size_t serialized = 0; serialized < result.size(); ++serialized) {
    int64_t rest = (int64_t)serialized;
    for (size_t d = 0; d < dims.size(); ++d) {
      index[d] = rest % dims[d];
      rest /= dims[d];
    }
    int64_t graph = 0;
    for (size_t d = 0; d < outer_rank; ++d) graph = graph * dims[d] + index[d];
    int64_t leaf_size = 1;
    for (size_t d = outer_rank; d < dims.size(); ++d) leaf_size *= dims[d];
    graph *= leaf_size;
    if (dims.size() == outer_rank + 1) {
      graph += index[outer_rank];
    } else if (dims.size() == outer_rank + 2) {
      graph += index[outer_rank + 1] * dims[outer_rank] + index[outer_rank];
    }
    result[serialized] = graph_source[(size_t)graph];
  }
  return result;
}

}  // namespace stanli

#endif
