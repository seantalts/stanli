// The cross-path agreement matrix, over every fixture that can be driven.
//
// tests/cross_path.hpp holds the comparison machinery and the account of
// what this axis catches and what it cannot; this file is the corpus that
// drives it. Every `tests/fixtures/*.tmir.sexp` joins automatically, with
// `<name>.json` as its data when that file exists and generated unit data
// otherwise, so a new feature's own fixture becomes a cross-check input the
// day it lands and the harness lags new features by zero fixtures.
//
// Fixtures the default configuration cannot compile or cannot evaluate at
// any draw variant are recorded as skipped rather than failed: part of the
// corpus exists precisely to pin a compile-time refusal
// (`viewc_*_bad`, `viewc_*_mismatch`), and a model out of support at every
// probe point is not a disagreement. What keeps the skip list from
// swallowing a regression is the floors at the bottom: the number of
// fixtures that drive, the number that reach each engine, and the number
// each configuration changes.
#include "cross_path.hpp"

#include <stanli/mir.hpp>
#include <stanli/sexp.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Data for a fixture that checks in none: every integer 1, every real 0.5,
// every size a literal or an integer set earlier. Empty when an input has
// another type or size, and then the fixture runs with no data as before.
std::string unit_data(const std::string& sexp) {
  using namespace stanli;
  const mir::Program p = mir::read_program(sexp::parse(sexp));
  std::map<std::string, long> ints;
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [name, t] : p.input_vars) {
    const std::string& leaf = t.base == "SArray" ? t.elem_base : t.base;
    const bool is_int = leaf == "SInt";
    if (!is_int && leaf != "SReal" && leaf != "SVector" &&
        leaf != "SRowVector" && leaf != "SMatrix")
      return "";
    std::vector<long> dims;
    for (const mir::Expr& d : t.dims) {
      if (d.kind == mir::Expr::LitInt)
        dims.push_back(d.lit_i);
      else if (d.kind == mir::Expr::Var && ints.count(d.name))
        dims.push_back(ints[d.name]);
      else
        return "";
    }
    nlohmann::json v = is_int ? nlohmann::json(1) : nlohmann::json(0.5);
    for (size_t k = dims.size(); k-- > 0;)
      v = nlohmann::json(std::vector<nlohmann::json>((size_t)dims[k], v));
    if (is_int && dims.empty()) ints[name] = 1;
    j[name] = v;
  }
  return j.dump();
}

}  // namespace

int main() {
  using namespace stanli;
  namespace fs = std::filesystem;

  const std::string dir = "tests/fixtures";
  const cross::Ledger led = cross::Ledger::load("tests/cross_path_ledger.json");
  if (!led.error.empty()) {
    std::printf("FAIL ledger %s\n", led.error.c_str());
    return 1;
  }

  // Sorted, because directory order is unspecified and a report whose lines
  // move between runs is a report nobody can diff.
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(dir)) {
    const std::string p = e.path().filename().string();
    const std::string suffix = ".tmir.sexp";
    if (p.size() > suffix.size() &&
        p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0)
      names.push_back(p.substr(0, p.size() - suffix.size()));
  }
  std::sort(names.begin(), names.end());
  expect(!names.empty(), "found fixtures in " + dir);

  int driven = 0, skipped = 0, comparisons = 0, declared = 0;
  int with_islands = 0, with_carved = 0, with_necessity = 0;
  int with_native_adj = 0, with_replay_adj = 0;
  int with_wa_graph = 0, with_wa_interp = 0, with_ode = 0;
  int with_ode_direct_rk = 0, with_ode_interp = 0;
  int wa_columns_compared = 0, wa_rng_excluded = 0;
  std::vector<std::string> skips;
  std::map<std::string, int> changed, ran;
  int unit_data_fixtures = 0, unit_data_driven = 0;

  for (const std::string& name : names) {
    const std::string sexp = slurp(dir + "/" + name + ".tmir.sexp");
    DataMap data;
    const std::string json = dir + "/" + name + ".json";
    const bool checked_in = fs::exists(json);
    const std::string generated = checked_in ? "" : unit_data(sexp);
    try {
      if (checked_in)
        data = DataMap::from_json(slurp(json));
      else if (!generated.empty())
        data = DataMap::from_json(generated);
    } catch (const std::exception& e) {
      expect(false, name + " data: " + e.what());
      continue;
    }
    if (!generated.empty() && generated != "{}") ++unit_data_fixtures;
    cross::Options opt;
    opt.model = name;
    opt.repro_model = dir + "/" + name + ".stan";
    opt.repro_data = checked_in ? json : std::string("DATA.json");
    const cross::Result r = cross::run_matrix(sexp, data, opt, led);
    if (!r.report.empty()) std::fputs(r.report.c_str(), stdout);
    if (!r.ok && !generated.empty())
      std::printf("  DATA.json for %s: %s\n", name.c_str(), generated.c_str());
    if (!r.skipped.empty()) {
      ++skipped;
      skips.push_back(name + ": " + r.skipped);
      continue;
    }
    ++driven;
    if (!generated.empty() && generated != "{}") ++unit_data_driven;
    comparisons += r.comparisons;
    declared += r.declared;
    wa_columns_compared += r.wa_compared;
    wa_rng_excluded += r.wa_rng_excluded;
    expect(r.ok, "cross-path agreement for " + name);

    // Engine coverage is asked of EVERY configuration, not just the
    // default: on fixtures this small the carver's cost estimate declines
    // almost everywhere, so carved islands -- and the generated adjoints
    // that come with them -- exist only under STANLI_ISLAND_ALWAYS. That
    // is still the carver's compiler and the carver's vocabulary running
    // over real models, which is the thing worth having covered.
    const auto any = [&r](int cross::Paths::* field) {
      int best = 0;
      for (const auto& [name, p] : r.by_config)
        if (p.*field > best) best = p.*field;
      return best;
    };
    if (any(&cross::Paths::islands) > 0) ++with_islands;
    if (any(&cross::Paths::carved) > 0) ++with_carved;
    if (r.paths.necessity > 0) ++with_necessity;
    if (any(&cross::Paths::native_adj) > 0) ++with_native_adj;
    if (r.paths.islands > r.paths.native_adj) ++with_replay_adj;
    if (any(&cross::Paths::ode) > 0) ++with_ode;
    if (r.paths.ode_direct_rk > 0) ++with_ode_direct_rk;
    if (any(&cross::Paths::ode_interp) > 0) ++with_ode_interp;
    if (r.paths.wa == "graph+interp") ++with_wa_graph;
    if (r.paths.wa == "truncated+interp") ++with_wa_interp;
    for (const auto& [cfg, did] : r.changed) {
      ++ran[cfg];
      if (did) ++changed[cfg];
    }
  }

  std::printf(
      "cross-path: %d fixtures driven, %d skipped, %d comparisons, "
      "%d declared divergences\n",
      driven, skipped, comparisons, declared);
  std::printf("  unit data generated for %d fixtures, %d of them drive\n",
              unit_data_fixtures, unit_data_driven);
  std::printf(
      "  engines: %d with islands (%d necessity, %d carved), %d with a "
      "generated adjoint, %d with a var replay, %d with an ODE (%d direct "
      "RK, %d falling back to the interpreter)\n",
      with_islands, with_necessity, with_carved, with_native_adj,
      with_replay_adj, with_ode, with_ode_direct_rk, with_ode_interp);
  std::printf(
      "  write_array: %d complete graphs, %d truncated, %d columns "
      "compared against the interpreter, %d excluded as RNG-tainted\n",
      with_wa_graph, with_wa_interp, wa_columns_compared, wa_rng_excluded);
  // Engine-coverage floors. These are measured values, not aspirations:
  // each is what the corpus reaches today, so a vocabulary regression that
  // moves work off a path (a carve that stops carving, an ODE right-hand
  // side that stops compiling, a write_array section that starts
  // truncating) fails here instead of silently shifting to a slower engine.
  expect(driven >= 430, "at least 430 fixtures drive the matrix, got " +
                            std::to_string(driven));
  expect(unit_data_driven >= 65,
         "at least 65 fixtures drive on generated unit data, got " +
             std::to_string(unit_data_driven));
  expect(with_islands >= 91, "at least 91 fixtures reach an island, got " +
                                 std::to_string(with_islands));
  expect(with_necessity >= 84,
         "at least 84 fixtures reach a necessity island, got " +
             std::to_string(with_necessity));
  expect(with_carved >= 8, "at least 8 fixtures reach a carved island, got " +
                               std::to_string(with_carved));
  expect(with_native_adj >= 79,
         "at least 79 fixtures run a generated adjoint, got " +
             std::to_string(with_native_adj));
  expect(with_ode >= 28, "at least 28 fixtures reach the ODE integrator, got " +
                             std::to_string(with_ode));
  expect(with_ode_direct_rk >= 13,
         "at least 13 fixtures reach the direct RK path, got " +
             std::to_string(with_ode_direct_rk));
  expect(with_wa_graph >= 428,
         "at least 428 fixtures lower their whole write_array section, got " +
             std::to_string(with_wa_graph));
  expect(wa_columns_compared >= 3836,
         "at least 3836 write_array columns are differenced against the "
         "interpreter, got " +
             std::to_string(wa_columns_compared));

  // Every configuration has to still switch something: a renamed
  // environment variable would make its comparison compare a configuration
  // with itself, and nothing else in this file would notice. `ran` counts
  // the fixtures a configuration was compared on, `changed` those it
  // compiled or ran differently from its baseline. region_map_checked only
  // adds an assertion, so it has no change to show.
  struct Floor {
    const char* config;
    int ran, changed;
  };
  const Floor floors[] = {
      {"no_island", 430, 2},
      {"island_always", 430, 2},
      {"no_native_adj", 430, 120},
      {"no_ode_direct_rk", 430, 13},
      {"passes_off", 430, 127},
      {"no_partition", 430, 1},
      {"no_cse", 430, 44},
      {"no_data_preload", 430, 2},
      {"no_region_map", 46, 46},
      {"region_map_recompute", 46, 46},
      {"region_map_checked", 46, 0},
      {"region_map_no_lanes", 46, 46},
      {"region_map_tile_recompute", 46, 45},
      {"structured_forced", 430, 192},
      {"structured_no_memo", 118, 17},
      {"structured_plain", 118, 118},
  };
  for (const cross::Config& c : cross::configs()) {
    if (c.name == "default") continue;
    std::printf("  config %s ran on %d, changed %d\n", c.name.c_str(),
                ran[c.name], changed[c.name]);
    const Floor* f = nullptr;
    for (const Floor& g : floors)
      if (c.name == g.config) f = &g;
    if (!f) {
      expect(false, "config " + c.name + " has a floor");
      continue;
    }
    expect(ran[c.name] >= f->ran && changed[c.name] >= f->changed,
           "config " + c.name + " runs on at least " + std::to_string(f->ran) +
               " fixtures and changes at least " + std::to_string(f->changed) +
               ", got " + std::to_string(ran[c.name]) + " and " +
               std::to_string(changed[c.name]));
  }

  // Only on a failure, and last: the skip list is the first thing anyone
  // asks for when a floor drops, and 40-odd lines of it on a green run is
  // noise nobody reads.
  if (failures > 0) {
    std::printf("skipped fixtures:\n");
    for (const std::string& s : skips) std::printf("  %s\n", s.c_str());
  }

  if (failures == 0) std::printf("test_cross_path OK\n");
  return failures == 0 ? 0 : 1;
}
