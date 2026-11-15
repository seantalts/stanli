// Hand-built density ops for tests: the lowering writes every argument's
// activity explicitly, so a test graph has to as well.
#ifndef STANLI_TESTS_DENSITY_OPS_HPP
#define STANLI_TESTS_DENSITY_OPS_HPP

#include <stanli/graph.hpp>
#include <stanli/kernel_types.hpp>

#include <initializer_list>
#include <utility>
#include <vector>

namespace stanli {
namespace testutil {

// A density op with every argument autodiff.
inline int add_density(Graph& g, uint16_t opcode,
                       std::initializer_list<int> ins, int out,
                       std::vector<int> idata = {}) {
  const int id = g.add_op(opcode, ins, out, std::move(idata));
  g.ops[(size_t)id].variant = DensityMode::all_active((int)ins.size()).byte();
  return id;
}

}  // namespace testutil
}  // namespace stanli

#endif
