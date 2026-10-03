#ifndef STANLI_REGION_MAP_HPP
#define STANLI_REGION_MAP_HPP

#include <stanli/island.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stanli {

inline constexpr int kRegionMapSavedGap = 64;

inline constexpr int kRegionMapTile = 64;

inline constexpr int kRegionMapProfileCodes = 128;

struct RegionMapTally {
  uint64_t evaluations = 0, tiles = 0;
  uint64_t fwd_exec = 0, fwd_lanes = 0, fwd_invariant = 0;
  uint64_t fwd_flag_stores = 0;
  uint64_t adj_exec = 0, adj_lanes = 0;
  uint64_t block_full = 0, block_partial = 0, block_empty = 0;
  uint64_t seg_full = 0, seg_partial = 0, seg_empty = 0;
  uint64_t fwd_op[kRegionMapProfileCodes] = {};
  uint64_t fwd_op_lanes[kRegionMapProfileCodes] = {};
  uint64_t fwd_op_invariant[kRegionMapProfileCodes] = {};
  uint64_t adj_op[kRegionMapProfileCodes] = {};
  uint64_t adj_op_lanes[kRegionMapProfileCodes] = {};
  void add(const RegionMapTally& o);
};

struct RegionMapProfile {
  std::string label;
  std::string form;
  int body = 0;
  int64_t iterations = 0;
  int hoisted = 0;
  std::mutex mu;
  RegionMapTally total;
};

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
    int32_t in_reg[6] = {-1, -1, -1, -1, -1, -1};
    int32_t out_reg = -1, scratch_reg = -1;
    int32_t val_in_reg[6] = {-1, -1, -1, -1, -1, -1};
    int32_t val_out_reg = -1;
    int32_t adj_in_cell[6] = {-1, -1, -1, -1, -1, -1};
    int32_t adj_out_cell = -1;
  };
  struct Operands {
    int32_t dst = -1, a = -1, b = -1, c = -1;
  };
  struct AdjOperands {
    int32_t dst = -1, a = -1, b = -1, c = -1;
    int32_t va = -1, vb = -1, vc = -1, vd = -1;
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
  std::vector<int32_t> seed_regs;
  std::vector<int32_t> cell_slot;
  std::vector<Block> blocks;
  std::vector<Operands> ops;
  std::vector<AdjOperands> adj_ops;
  std::vector<CallWindow> calls;
  std::shared_ptr<RegionMapProfile> profile;
  std::vector<char> invariant;
  std::vector<char> flag_store;
  std::vector<char> guard_reg;
  std::vector<char> adj_skip;
  std::vector<char> adj_residue;
  std::vector<std::vector<int>> segment_blocks;
  int segment_count = 0;
  int64_t mask_cells = 0;
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
  std::string label;
};

void plan_region_map_lanes(RegionMapProg& p, bool enabled,
                           int64_t storage_limit);

uint64_t region_map_lane_runs();

int64_t region_map_lane_cells(const RegionMapProg& p);
std::vector<char> region_map_clean_exempt_cells(const RegionMapProg& p);

void region_map_lanes_forward(const RegionMapProg& p, KernelCtx& ctx,
                              double* region);

void region_map_lanes_backward(const RegionMapProg& p, KernelCtx& ctx,
                               double* region, double* adj);

}  // namespace stanli

#endif
