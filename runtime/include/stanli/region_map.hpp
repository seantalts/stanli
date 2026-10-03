#ifndef STANLI_REGION_MAP_HPP
#define STANLI_REGION_MAP_HPP

#include <stanli/island.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace stanli {

struct RegionMapProg : IslandProg {
  int iter_reg = -1;
  int64_t lo = 1;
  int64_t count = 0;
  std::vector<std::pair<int, int>> transient;
  std::vector<Program::Instr> prologue;
  std::vector<std::pair<int, int>> saved;
  int saved_cells = 0;
};

}  // namespace stanli

#endif
