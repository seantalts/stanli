#ifndef STANLI_REGION_MAP_HPP
#define STANLI_REGION_MAP_HPP

#include <stanli/island.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace stanli {

inline constexpr int kRegionMapSavedGap = 64;

inline constexpr int kRegionMapTile = 64;
constexpr int kRegionMapTileRecomputeCells = 1024;

struct RegionMapLanePlan {
  struct Block {
    int begin = 0, end = 0;
    int taken = -1;
    int fall = -1;
  };
  struct CallWindow {
    Program::Call fwd;
    int fwd_size = 0;
    int in_off[6] = {0, 0, 0, 0, 0, 0};
    int out_off = 0, scratch_off = 0;
    int val_in_off[6] = {0, 0, 0, 0, 0, 0};
    int val_out_off = 0, bwd_scratch_off = 0;
    int adj_in_off[6] = {0, 0, 0, 0, 0, 0};
    int adj_out_off = 0;
    int bwd_size = 0;
  };
  bool active = false;
  bool tile_recompute = false;
  std::string refusal;
  int fwd_regs = 0;
  int adj_cells = 0;
  int tiles = 0;
  int64_t storage = 0;
  int max_window = 0;
  int max_sites = 0;
  int max_dynamic = 0;
  std::vector<int32_t> reg_slot;
  std::vector<int32_t> cell_slot;
  std::vector<Block> blocks;
  std::vector<CallWindow> calls;
};

struct RegionMapProg : IslandProg {
  int iter_reg = -1;
  int64_t lo = 1;
  int64_t count = 0;
  std::vector<std::pair<int, int>> transient;
  std::vector<Program::Instr> prologue;
  std::vector<std::pair<int, int>> saved;
  int saved_cells = 0;
  bool recompute = false;
  RegionMapLanePlan lanes;
};

void plan_region_map_lanes(RegionMapProg& p, bool enabled,
                           int64_t storage_limit);

uint64_t region_map_lane_runs();

int64_t region_map_lane_cells(const RegionMapProg& p);

void region_map_lanes_forward(const RegionMapProg& p, KernelCtx& ctx,
                              double* region);

void region_map_lanes_backward(const RegionMapProg& p, KernelCtx& ctx,
                               double* region, double* adj);

}  // namespace stanli

#endif
