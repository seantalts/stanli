#ifndef STANLI_REGION_MAP_HPP
#define STANLI_REGION_MAP_HPP

#include <stanli/island.hpp>
#include <stanli/tile_call.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace stanli {

inline constexpr int kRegionMapSavedGap = 64;

inline constexpr int kRegionMapTile = 64;

inline constexpr int kRegionMapProfileCodes = 128;

// STANLI_REGION_MAP_CHECK_CLEAN found an adjoint cell left nonzero after a
// recompute sweep: a kernel bug, never a property of the evaluation point.
struct RegionMapUncleanSweep : std::logic_error {
  using std::logic_error::logic_error;
};

struct RegionMapTally {
  uint64_t evaluations = 0, tiles = 0;
  uint64_t fwd_exec = 0, fwd_lanes = 0, fwd_invariant = 0;
  uint64_t adj_exec = 0, adj_lanes = 0, adj_cleared = 0;
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
    TileCallKind tile = TileCallKind::None;
    // An index read from shared cells: its adjoint takes one dynamic site
    // per selected element instead of a row for every input cell.
    bool index_sites = false;
    // An index read whose selectors take the same values in every
    // evaluation: they come from data and the loop variable, and no
    // parameter decides whether the read runs. Once an evaluation has checked
    // them, later evaluations of the same executor skip the checks.
    bool proven = false;
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
  int tile_calls = 0;
  int proven_calls = 0;
  std::vector<int32_t> reg_slot;
  std::vector<int32_t> seed_regs;
  std::vector<int32_t> cell_slot;
  std::vector<Block> blocks;
  std::vector<Operands> ops;
  std::vector<AdjOperands> adj_ops;
  std::vector<CallWindow> calls;
  std::shared_ptr<RegionMapProfile> profile;
  std::vector<char> invariant;
  std::vector<char> skip;
  std::vector<char> trail_reg;
  std::vector<char> adj_skip;
  std::vector<char> adj_sink;
  // Per adjoint instruction: a DYN_INDEX from shared cells whose adjoint
  // only clears its output, because no parameter reaches the container
  // (adj_drop), or adds straight into the container, because no other
  // instruction logs a site into its cells (adj_direct).
  std::vector<char> adj_drop;
  std::vector<char> adj_direct;
  std::vector<int> segment_block;
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
  // Check after each backward iteration that the adjoint cells it should
  // have cleared are zero (STANLI_REGION_MAP_CHECK_CLEAN, read at lowering).
  bool check_clean = false;
  // Add the iteration terms with compensation once there are more than
  // kPlainSumMax of them (pairwise_sum.hpp). Set for a loop that the program
  // budget maps instead of unrolling.
  bool compensated = false;
  RegionMapLanePlan lanes;
  std::string label;
};

void plan_region_map_lanes(RegionMapProg& p, bool enabled,
                           int64_t storage_limit);

uint64_t region_map_lane_runs();

uint64_t region_map_tile_call_runs();

// Forward runs of a lane plan that skipped the checks of proven gathers.
uint64_t region_map_proven_runs();

std::string region_map_call_summary(const RegionMapProg& p);

int64_t region_map_lane_cells(const RegionMapProg& p);
// The lane plan's storage in a region map's scratch: after the registers,
// on a 64-byte boundary.
double* region_map_lane_region(const RegionMapProg& p, double* scratch);
std::vector<char> region_map_clean_exempt_cells(const RegionMapProg& p);

// `proven`: an earlier evaluation of this executor checked the selectors
// of the proven gathers, so this one skips their checks.
void region_map_lanes_forward(const RegionMapProg& p, KernelCtx& ctx,
                              double* region, bool proven);

void region_map_lanes_backward(const RegionMapProg& p, KernelCtx& ctx,
                               double* region, double* adj);

}  // namespace stanli

#endif
