#ifndef STANLI_GP_COV_FUSION_HPP
#define STANLI_GP_COV_FUSION_HPP

#include <stanli/graph.hpp>

namespace stanli {

// Keep a covariance and its private scalar diagonal additions on one Stan
// Math tape. Some covariance families return aliased diagonal variables;
// materializing the matrix between these operations loses their reverse
// accumulation order. Refuses incomplete or externally observed chains.
int fuse_gp_diagonal_updates(Graph& g, const std::vector<int>& roots);

}  // namespace stanli
#endif
