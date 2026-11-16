// Hand-built density ops for tests: the lowering writes every argument's
// activity explicitly, so a test graph has to as well.
#ifndef STANLI_TESTS_DENSITY_OPS_HPP
#define STANLI_TESTS_DENSITY_OPS_HPP

#include <stanli/density_layout.hpp>
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

// idata in the layouts of density_layout.hpp.
inline std::vector<int> glm_idata(std::vector<int> outcomes, int rows, int cols,
                                  int flags = 0) {
  append_glm_layout(outcomes, rows, cols, flags);
  return outcomes;
}

inline std::vector<int> vectorized_idata(std::vector<int> outcomes, int width,
                                         const std::vector<int>& counts,
                                         int array_mask) {
  append_vectorized_layout(outcomes, width, counts, array_mask);
  return outcomes;
}

// One integer argument: its values, and whether it is a language int.
struct IntArg {
  std::vector<int> values;
  bool scalar = false;
};

inline std::vector<int> int_groups_idata(const std::vector<IntArg>& args) {
  std::vector<int> idata;
  int scalar_mask = 0;
  for (size_t k = 0; k < args.size(); ++k) {
    append_int_group(idata, args[k].values.data(),
                     (int64_t)args[k].values.size());
    if (args[k].scalar) scalar_mask |= 1 << k;
  }
  idata.push_back(scalar_mask);
  return idata;
}

}  // namespace testutil
}  // namespace stanli

#endif
