// The cross-path agreement matrix, over every fixture that can be driven.
//
// tests/cross_path.hpp holds the comparison machinery and the account of
// what this axis catches and what it cannot; this file is the corpus that
// drives it. Every `tests/fixtures/*.tmir.sexp` joins automatically, with
// `<name>.json` as its data when that file exists and an empty DataMap
// otherwise, so a new feature's own fixture becomes a cross-check input the
// day it lands and the harness lags new features by zero fixtures.
//
// Fixtures the default configuration cannot compile or cannot evaluate at
// any draw variant are recorded as skipped rather than failed: a good third
// of the corpus exists precisely to pin a compile-time refusal
// (`viewc_*_bad`, `viewc_*_mismatch`), and a model out of support at every
// probe point is not a disagreement. What keeps the skip list from
// swallowing a regression is the floors at the bottom: the number of
// fixtures that drive, and the number that reach each engine.
#include "cross_path.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
  int with_wa_graph = 0, with_wa_interp = 0, with_ode = 0, with_ode_interp = 0;
  int wa_columns_compared = 0, wa_rng_excluded = 0;
  int island_always_gained = 0, no_island_dropped = 0;
  std::vector<std::string> skips;

  for (const std::string& name : names) {
    const std::string sexp = slurp(dir + "/" + name + ".tmir.sexp");
    DataMap data;
    const std::string json = dir + "/" + name + ".json";
    if (fs::exists(json)) {
      try {
        data = DataMap::from_json(slurp(json));
      } catch (const std::exception& e) {
        expect(false, name + " data: " + e.what());
        continue;
      }
    }
    cross::Options opt;
    opt.model = name;
    opt.repro_model = dir + "/" + name + ".stan";
    opt.repro_data = fs::exists(json) ? json : std::string("DATA.json");
    const cross::Result r = cross::run_matrix(sexp, data, opt, led);
    if (!r.report.empty()) std::fputs(r.report.c_str(), stdout);
    if (!r.skipped.empty()) {
      ++skipped;
      skips.push_back(name + ": " + r.skipped);
      continue;
    }
    ++driven;
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
    if (any(&cross::Paths::ode_interp) > 0) ++with_ode_interp;
    if (r.paths.wa == "graph+interp") ++with_wa_graph;
    if (r.paths.wa == "truncated+interp") ++with_wa_interp;
    // The switches have to still switch something. A renamed environment
    // variable makes every comparison above compare a configuration with
    // itself, and nothing else in this file would notice.
    const auto islands_of = [&r](const char* cfg) {
      auto it = r.by_config.find(cfg);
      return it == r.by_config.end() ? 0 : it->second.islands;
    };
    if (islands_of("no_island") < r.paths.islands) ++no_island_dropped;
    if (islands_of("island_always") > r.paths.islands) ++island_always_gained;
  }

  std::printf(
      "cross-path: %d fixtures driven, %d skipped, %d comparisons, "
      "%d declared divergences\n",
      driven, skipped, comparisons, declared);
  std::printf(
      "  engines: %d with islands (%d necessity, %d carved), %d with a "
      "generated adjoint, %d with a var replay, %d with an ODE (%d falling "
      "back to the interpreter)\n",
      with_islands, with_necessity, with_carved, with_native_adj,
      with_replay_adj, with_ode, with_ode_interp);
  std::printf(
      "  write_array: %d complete graphs, %d truncated, %d columns "
      "compared against the interpreter, %d excluded as RNG-tainted\n",
      with_wa_graph, with_wa_interp, wa_columns_compared, wa_rng_excluded);
  std::printf(
      "  switches: NO_ISLAND drops islands on %d, ISLAND_ALWAYS adds on %d\n",
      no_island_dropped, island_always_gained);
  // Engine-coverage floors. These are measured values, not aspirations:
  // each is what the corpus reaches today, so a vocabulary regression that
  // moves work off a path (a carve that stops carving, an ODE right-hand
  // side that stops compiling, a write_array section that starts
  // truncating) fails here instead of silently shifting to a slower engine.
  expect(driven >= 85, "at least 85 fixtures drive the matrix, got " +
                           std::to_string(driven));
  expect(with_islands >= 9, "at least 9 fixtures reach an island, got " +
                                std::to_string(with_islands));
  expect(with_necessity >= 7,
         "at least 7 fixtures reach a necessity island, got " +
             std::to_string(with_necessity));
  expect(with_carved >= 2, "at least 2 fixtures reach a carved island, got " +
                               std::to_string(with_carved));
  expect(with_native_adj >= 2,
         "at least 2 fixtures run a generated adjoint, got " +
             std::to_string(with_native_adj));
  expect(with_ode >= 1, "at least 1 fixture reaches the ODE integrator, got " +
                            std::to_string(with_ode));
  expect(with_wa_graph >= 80,
         "at least 80 fixtures lower their whole write_array section, got " +
             std::to_string(with_wa_graph));
  expect(wa_columns_compared >= 800,
         "at least 800 write_array columns are differenced against the "
         "interpreter, got " +
             std::to_string(wa_columns_compared));
  // STANLI_ISLAND_ALWAYS has to still switch something: a renamed
  // environment variable would make every comparison above compare a
  // configuration with itself, and nothing else in this file would notice.
  //
  // There is no matching floor for STANLI_NO_ISLAND, and the counter above
  // says why: the carver's cost estimate declines on every fixture at the
  // default settings (`no_island_dropped` is 0), so the switch has nothing
  // to remove and cannot be shown working here. Fixtures are small by
  // design and that is the right trade; the corpus run, where the carver
  // does fire, is where that switch becomes observable.
  expect(island_always_gained >= 1,
         "STANLI_ISLAND_ALWAYS adds islands somewhere, got " +
             std::to_string(island_always_gained));

  // Only on a failure, and last: the skip list is the first thing anyone
  // asks for when a floor drops, and 80-odd lines of it on a green run is
  // noise nobody reads.
  if (failures > 0) {
    std::printf("skipped fixtures:\n");
    for (const std::string& s : skips) std::printf("  %s\n", s.c_str());
  }

  if (failures == 0) std::printf("test_cross_path OK\n");
  return failures == 0 ? 0 : 1;
}
