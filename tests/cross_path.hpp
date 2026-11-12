// Cross-path agreement: stanli held against itself.
//
// A different axis from the conformance sweep and the corpus oracle, which
// hold stanli against CmdStan. This one compiles ONE model several times
// -- once per engine configuration -- and demands that the answers agree.
// The bugs it exists for are the ones where two of stanli's own dispatch
// surfaces disagree about what a line of Stan means: the graph lowering,
// the MIR interpreter, and the register program each have their own
// name-to-operation table, and matrix division was mir_interp.hpp knowing a
// rule lower.cpp lacked.
//
// The configurations are the switches in configs() below, each a pure
// execution choice: the shipped pipeline against islands off and forced,
// replayed adjoints, the exact ODE oracle, the graph passes off together
// and singly, the data-input interpreter, the region map off and with its
// storage plans forced, and the execution choices inside a retained loop.
// Each one is compared at kDefaultPoints points on one executor, and the
// driver requires each one to change some model, so a switch that stops
// switching fails instead of comparing a configuration with itself.
//
// The default compile also sets STANLI_WA_FORCE_INTERP, the one new switch:
// it attaches WaInterp beside a COMPLETE write_array graph, so both engines
// produce the same CSV row from one model and the rows can be differenced.
//
// The gate is bitwise for every row. A divergence fails unless
// tests/cross_path_ledger.json declares it, with a bound and a cause naming
// the mechanism. That is deliberately not a tolerance: a new 1 ULP drift
// stays visible instead of being absorbed by a budget.
//
// What this does NOT catch, stated so nobody mistakes it for the other two
// axes: both paths wrong together (a wrong shared optable entry, a wrong
// density formula) -- every config agrees and this is blind. Nor transformed
// data, which has exactly one engine: every configuration shares the
// interpreter's answer there, right or wrong. Closing that needs a second
// path built by derivation rather than forced by a flag, which is Phase 2 of
// the design and not here.
#ifndef STANLI_TESTS_CROSS_PATH_HPP
#define STANLI_TESTS_CROSS_PATH_HPP

#include "env_helpers.hpp"
#include "../runtime/third_party/nlohmann_json.hpp"

#include <stanli/compile.hpp>
#include <stanli/data.hpp>
#include <stanli/graph.hpp>
#include <stanli/graph_print.hpp>
#include <stanli/island.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/ode.hpp>
#include <stanli/optable.hpp>
#include <stanli/region_map.hpp>
#include <stanli/wa_interp.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace stanli {
namespace cross {

// ---- raw bits, the currency of every comparison here ----------------------

inline uint64_t bits_of(double d) {
  uint64_t u;
  std::memcpy(&u, &d, sizeof(u));
  return u;
}

// Bitwise equality, NaN included: two paths that both produced the same NaN
// agree, and `==` would call that a mismatch.
inline bool same_bits(double a, double b) { return bits_of(a) == bits_of(b); }

// Distance in representable doubles. Meaningful only for two finite values
// of the same sign class; callers report "not comparable" otherwise, which
// is why a nonfinite mismatch can never be covered by a ULP bound.
inline int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? std::numeric_limits<int64_t>::min() - i : i;
}

inline bool ulp_comparable(double a, double b) {
  return std::isfinite(a) && std::isfinite(b);
}

inline int64_t ulp_distance(double a, double b) {
  const int64_t k = ulp_key(a) - ulp_key(b);
  return k < 0 ? -k : k;
}

inline std::string fmt_value(double d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g  (0x%016llx)", d,
                (unsigned long long)bits_of(d));
  return buf;
}

// ---- the draw --------------------------------------------------------------

// Deterministic and index-generated, so a failure is reproduced by naming
// the variant rather than by pasting 21 floats. Variant 0 is test_solve's
// spread point (no two parameters share a value); 1..3 are the three
// wa_probe_point variants the drivers already walk, which is what gives a
// model that is out of support at one point a real comparison at another.
inline constexpr int kDrawVariants = 4;

inline double draw_point(int64_t i, int variant) {
  if (variant <= 0) return 0.07 * (double)i - 0.6;
  return wa_probe_point(i, variant - 1);
}

// Point 0 is the variant's draw; point k > 0 moves every coordinate by a
// different deterministic offset, so a branch on a parameter value can go
// the other way. All points run in order on one executor, which is also
// what shows state left behind by a previous evaluation.
inline constexpr int kDefaultPoints = 3;

inline double sweep_point(int64_t i, int variant, int k) {
  const double base = draw_point(i, variant);
  if (k == 0) return base;
  return base + 0.6 * std::sin(1.9 * k + 2.3 * (double)i + 0.4) + 0.2 * (k - 1);
}

// stanli_check's evaluation points, shared with tools/ref_driver.cpp. Some
// models are invalid at a given point (an ODE solution dips below a declared
// lower bound, say) and both engines reject it; the harness then retries the
// next variant so those models still get a real comparison.
inline double check_point(int64_t i, int variant) {
  switch (variant) {
    case 1:
      return 0.02 * static_cast<double>((i % 5) - 2);
    case 2:
      return 0.0;
    case 3:
    case 4: {
      uint64_t z = static_cast<uint64_t>(i) + 0x9E3779B97F4A7C15ull;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      z ^= z >> 31;
      const double u = static_cast<double>(z >> 11) * 0x1.0p-53;
      // Variant 4 keeps every component in (0.1, 0.9), where an
      // unconstrained real used as a scale or a probability is valid.
      return variant == 3 ? -1.5 + 3.0 * u : 0.1 + 0.8 * u;
    }
    default:
      return 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
  }
}

// ---- environment -----------------------------------------------------------

// Region-map variables are read only while lowering or running an
// OP_REGION_MAP, so a model whose baseline graph has none is not run under
// such a configuration. Running them on every fixture changes no fixture
// this skips.
enum class Requires { Nothing, RegionMap };

// Each variable is NAME or NAME=VALUE; a bare NAME is set to "1". The
// configuration is compared with `baseline`, or with nothing when that is
// empty: such a configuration exists to be the baseline of others.
struct Config {
  std::string name;
  std::vector<std::string> vars;
  Requires requires_op = Requires::Nothing;
  std::string baseline = "default";
};

inline std::pair<std::string, std::string> split_var(const std::string& v) {
  const size_t eq = v.find('=');
  if (eq == std::string::npos) return {v, "1"};
  return {v.substr(0, eq), v.substr(eq + 1)};
}

inline const std::vector<Config>& configs();
inline const std::vector<Config>& corpus_configs();

// Every variable the harness ever sets, so a scope can clear the lot
// regardless of which config set what. A leftover STANLI_NO_ISLAND from a
// previous config would silently make the next comparison vacuous.
inline const std::vector<std::string>& harness_vars() {
  static const std::vector<std::string> v = [] {
    std::set<std::string> names;
    for (const auto* list : {&configs(), &corpus_configs()})
      for (const Config& c : *list)
        for (const std::string& var : c.vars)
          names.insert(split_var(var).first);
    return std::vector<std::string>(names.begin(), names.end());
  }();
  return v;
}

class EnvScope {
 public:
  explicit EnvScope(const std::vector<std::string>& set) {
    for (const std::string& v : harness_vars()) test_unsetenv(v.c_str());
    for (const std::string& v : set) {
      const auto [name, value] = split_var(v);
      test_setenv(name.c_str(), value.c_str(), 1);
    }
  }
  ~EnvScope() {
    for (const std::string& v : harness_vars()) test_unsetenv(v.c_str());
  }
  EnvScope(const EnvScope&) = delete;
  EnvScope& operator=(const EnvScope&) = delete;
};

inline const std::vector<Config>& configs() {
  static const std::vector<Config> c = {
      // The default compile is the one that also carries the interpreted
      // write_array, because row 5 differences two engines inside a single
      // configuration rather than across two.
      {"default", {"STANLI_WA_FORCE_INTERP"}},
      {"no_island", {"STANLI_NO_ISLAND"}},
      {"island_always", {"STANLI_ISLAND_ALWAYS"}},
      {"no_island_compact", {"STANLI_NO_ISLAND_COMPACT"}},
      {"island_always_no_compact",
       {"STANLI_ISLAND_ALWAYS", "STANLI_NO_ISLAND_COMPACT"},
       Requires::Nothing,
       "island_always"},
      {"no_native_adj", {"STANLI_NO_NATIVE_ADJ"}},
      {"no_ode_direct_rk", {"STANLI_NO_ODE_DIRECT_RK"}},
      {"passes_off",
       {"STANLI_NO_REROLL", "STANLI_NO_PARTITION", "STANLI_NO_INPLACE",
        "STANLI_NO_CONSTFOLD", "STANLI_NO_CSE"}},
      {"no_partition", {"STANLI_NO_PARTITION"}},
      {"no_cse", {"STANLI_NO_CSE"}},
      {"no_data_preload", {"STANLI_NO_DATA_PRELOAD"}},
      {"no_region_map", {"STANLI_REGION_MAP=0"}, Requires::RegionMap},
      {"region_map_recompute",
       {"STANLI_REGION_MAP_SAVE_LIMIT=0", "STANLI_REGION_MAP_CHECK_CLEAN"},
       Requires::RegionMap},
      {"region_map_checked",
       {"STANLI_REGION_MAP_TILE_CELLS=1000000000",
        "STANLI_REGION_MAP_CHECK_CLEAN"},
       Requires::RegionMap},
      {"region_map_no_lanes",
       {"STANLI_REGION_MAP_LANES=0"},
       Requires::RegionMap},
      {"region_map_tile_recompute",
       {"STANLI_REGION_MAP_TILE_CELLS=0", "STANLI_REGION_MAP_CHECK_CLEAN"},
       Requires::RegionMap},
      // Retaining a loop is not bitwise-neutral: forced retention differs
      // from the default by 1-10 ULP on a dozen fixtures. What must be
      // exact is every execution choice inside a retained loop, so those
      // compare against the forced one.
      {"structured_forced",
       {"STANLI_STRUCTURED_LOOPS=1", "STANLI_STRUCTURED_FRAMES=1",
        "STANLI_STRUCTURED_LOOP_DIAGNOSTICS"},
       Requires::Nothing,
       ""},
      {"structured_no_memo",
       {"STANLI_STRUCTURED_LOOPS=1", "STANLI_STRUCTURED_FRAMES=1",
        "STANLI_STRUCTURED_LOOP_DIAGNOSTICS", "STANLI_NO_DATA_LOOP_MEMO"},
       Requires::Nothing,
       "structured_forced"},
      {"structured_plain",
       {"STANLI_STRUCTURED_LOOPS=1", "STANLI_STRUCTURED_FRAMES=0",
        "STANLI_NO_STRUCTURED_REPLAY", "STANLI_STRUCTURED_COMPILED_RECORDING=0",
        "STANLI_STRUCTURED_DATA_RECORDING=0",
        "STANLI_STRUCTURED_PREPARED_INDEX=0",
        "STANLI_STRUCTURED_PREPARED_ITERATOR=0",
        "STANLI_NO_STRUCTURED_FRAME_INSTANCE",
        "STANLI_NO_STRUCTURED_CELL_CONSTANTS"},
       Requires::Nothing,
       "structured_forced"},
  };
  return c;
}

// Run only under --cross-corpus, by the posteriordb loop: the pass set the
// old graph-pass A/B harness turned off together, islands included.
inline const std::vector<Config>& corpus_configs() {
  static const std::vector<Config> c = {
      {"passes_islands_off",
       {"STANLI_NO_REROLL", "STANLI_NO_INPLACE", "STANLI_NO_CONSTFOLD",
        "STANLI_NO_ISLAND"}},
  };
  return c;
}

inline const Config* find_config(const std::string& name) {
  for (const auto* list : {&configs(), &corpus_configs()})
    for (const Config& c : *list)
      if (c.name == name) return &c;
  return nullptr;
}

inline const Config& config_named(const std::string& name) {
  const Config* c = find_config(name);
  return c ? *c : configs()[0];
}

inline std::string env_prefix(const std::string& config_name) {
  const Config* c = find_config(config_name);
  if (!c) return "";
  std::string s;
  for (const std::string& v : c->vars) {
    const auto [name, value] = split_var(v);
    s += name + "=" + value + " ";
  }
  return s;
}

// ---- which engines actually ran --------------------------------------------

struct Paths {
  int64_t ops = 0;
  // FNV-1a of the printed graph, island programs and region-map plans: two
  // configurations with the same value compiled the same thing.
  uint64_t fingerprint = 0;
  int region_maps = 0;
  int loops = 0;
  // The structured-loop diagnostics of every evaluation, hashed, and the
  // data-loop memo hits they report; zero unless the config turns them on.
  uint64_t diagnostics = 0;
  int64_t memo_hits = 0;
  int islands = 0;
  // Necessity islands are not distinguishable from carved ones on the
  // island itself, so the split is derived: it is what survives
  // STANLI_NO_ISLAND. Zero until the matrix has that second compile.
  int necessity = 0;
  int carved = 0;
  int native_adj = 0;  // islands running their generated backward
  int ode = 0;
  int ode_prog = 0;         // right-hand side compiled
  int ode_direct_rk = 0;    // eligible and enabled coupled RK45/CKRK path
  int ode_interp = 0;       // right-hand side falls back to MirInterp under var
  std::string wa = "none";  // none | graph | interp | truncated+interp
  size_t wa_columns = 0;

  std::string line() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "islands %d (%d necessity, %d carved), native_adj %d/%d, "
                  "region maps %d, loops %d, wa %s, ode %d (%d program, %d "
                  "direct RK, %d interp)",
                  islands, necessity, carved, native_adj, islands, region_maps,
                  loops, wa.c_str(), ode, ode_prog, ode_direct_rk, ode_interp);
    return buf;
  }
};

inline uint64_t fnv1a(const std::string& s,
                      uint64_t h = 1469598103934665603ull) {
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

inline void scan_graph(const Graph& g, Paths* p) {
  p->ops = (int64_t)g.ops.size();
  std::string text;
  print_graph(text, g);
  print_islands(text, g);
  for (const Op& op : g.ops) {
    if (op.opcode == OP_REGION_MAP) {
      ++p->region_maps;
      const auto* m = static_cast<const RegionMapProg*>(op.udata);
      text += "\nregion_map recompute=" + std::to_string(m->recompute) +
              " saved=" + std::to_string(m->saved_cells) +
              " lanes=" + std::to_string(m->lanes.active) +
              " tile_recompute=" + std::to_string(m->lanes.tile_recompute) +
              " " + m->lanes.refusal;
    } else if (op.opcode == OP_LOOP) {
      ++p->loops;
    }
    if (op.opcode == OP_ISLAND) {
      ++p->islands;
      if (static_cast<const IslandProg*>(op.udata)->native_adj) ++p->native_adj;
    } else if (op.opcode == OP_ODE) {
      ++p->ode;
      const auto* spec = static_cast<const OdeSpec*>(op.udata);
      if (spec->prog.ok)
        ++p->ode_prog;
      else
        ++p->ode_interp;
      if (spec->direct_rk && spec->direct_rk_enabled) ++p->ode_direct_rk;
      text += "\node program=" + std::to_string(spec->prog.ok) + " direct_rk=" +
              std::to_string(spec->direct_rk && spec->direct_rk_enabled);
    }
  }
  p->fingerprint = fnv1a(text);
}

// ---- one configuration's answer --------------------------------------------

struct PointRun {
  bool evaluated = false;
  std::string eval_error;
  double lp = 0.0;
  std::vector<double> grad;
};

struct Run {
  std::string config;
  bool compiled = false;
  std::string compile_error;
  // The point-0 outcome, which decides whether a variant is in support.
  bool evaluated = false;
  std::string eval_error;
  std::vector<PointRun> points;
  // stanli_check's default point, evaluated after the sweep when asked for.
  bool has_check = false;
  PointRun check;
  Paths paths;

  // Row 5's two engines, filled only for the configuration that asked for
  // them. Names come from CompiledModel::csv_names on each side, because a
  // column-set difference is itself a finding.
  bool have_graph_row = false;
  std::vector<std::string> graph_names;
  std::vector<double> graph_row;
  bool have_interp_row = false;
  std::vector<std::string> interp_names;
  std::vector<double> interp_row;    // seed A
  std::vector<double> interp_row_b;  // seed B, for RNG-taint detection
  std::string wa_error;
};

inline bool sets_var(const Config& cfg, const char* name) {
  for (const std::string& v : cfg.vars)
    if (split_var(v).first == name) return true;
  return false;
}

class DiagnosticCapture {
 public:
  explicit DiagnosticCapture(bool on) : on_(on) {
    if (on_)
      set_diagnostic_sink([this](const char* text, std::size_t len) {
        text_.append(text, len);
      });
  }
  ~DiagnosticCapture() {
    if (on_) set_diagnostic_sink(nullptr);
  }
  int64_t sum(const std::string& key) const {
    int64_t total = 0;
    for (size_t at = text_.find(key); at != std::string::npos;
         at = text_.find(key, at + key.size()))
      total += std::strtoll(text_.c_str() + at + key.size(), nullptr, 10);
    return total;
  }
  const std::string& text() const { return text_; }
  DiagnosticCapture(const DiagnosticCapture&) = delete;
  DiagnosticCapture& operator=(const DiagnosticCapture&) = delete;

 private:
  bool on_;
  std::string text_;
};

// Compile and evaluate under `cfg` at `n_points` points, then at
// stanli_check's default point when `with_check` is set. Never throws: a
// compile or evaluation failure is a first-class outcome, because "one path
// threw and the other returned" is exactly the kind of disagreement worth
// catching. Later points are skipped when point 0 is out of support, since
// the caller then retries the next variant.
inline Run evaluate(const std::string& mir_text, const DataMap& data,
                    const Config& cfg, int variant, bool want_write_array,
                    int n_points, bool with_check) {
  Run r;
  r.config = cfg.name;
  EnvScope env(cfg.vars);
  DiagnosticCapture diagnostics(
      sets_var(cfg, "STANLI_STRUCTURED_LOOP_DIAGNOSTICS"));
  CompiledModel cm;
  try {
    cm = compile_model(mir_text, data);
  } catch (const std::exception& e) {
    r.compile_error = e.what();
    const size_t nl = r.compile_error.find('\n');
    if (nl != std::string::npos) r.compile_error.resize(nl);
    return r;
  }
  r.compiled = true;
  scan_graph(cm.graph, &r.paths);
  if (!cm.write_array) {
    r.paths.wa = "none";
  } else if (cm.write_array->interp) {
    r.paths.wa =
        cm.write_array->truncated.empty() ? "graph+interp" : "truncated+interp";
  } else {
    r.paths.wa = cm.write_array->columns.empty() ? "empty" : "graph";
  }

  std::unique_ptr<Executor> ex;
  try {
    ex = std::make_unique<Executor>(std::move(cm.graph));
    cm.bind(*ex);
  } catch (const std::exception& e) {
    r.eval_error = e.what();
    return r;
  }
  const int64_t n = ex->n_params();
  const auto at = [&](auto&& point) {
    PointRun pr;
    try {
      for (int64_t i = 0; i < n; ++i) ex->params_data()[i] = point(i);
      pr.grad.assign((size_t)n, 0.0);
      pr.lp = ex->gradient(pr.grad.data());
      pr.evaluated = true;
    } catch (const std::exception& e) {
      pr.eval_error = e.what();
      pr.grad.clear();
    }
    return pr;
  };
  for (int k = 0; k < n_points; ++k) {
    r.points.push_back(
        at([&](int64_t i) { return sweep_point(i, variant, k); }));
    if (k == 0) {
      r.evaluated = r.points[0].evaluated;
      r.eval_error = r.points[0].eval_error;
      if (!r.evaluated) break;
    }
  }
  if (with_check) {
    r.check = at([](int64_t i) { return check_point(i, 0); });
    r.has_check = true;
  }
  r.paths.memo_hits = diagnostics.sum("memo_hits=");
  r.paths.diagnostics = fnv1a(diagnostics.text());
  if (!r.evaluated) return r;

  try {
    if (want_write_array && cm.write_array) {
      if (n_points > 1 || with_check) {
        for (int64_t i = 0; i < n; ++i)
          ex->params_data()[i] = draw_point(i, variant);
        std::vector<double> grad((size_t)n, 0.0);
        ex->gradient(grad.data());
      }
      auto& wa = *cm.write_array;
      // The graph side, if any of the section lowered. A truncated graph
      // still holds a real prefix and the prefix is worth differencing.
      if (!wa.columns.empty()) {
        try {
          Executor wex(std::move(wa.graph));
          wa.bind(wex);
          for (int64_t i = 0; i < wex.n_params(); ++i)
            wex.params_data()[i] = draw_point(i, variant);
          WaRng graph_rng(1234);
          wex.run_forward_only(EvalState{&graph_rng});
          r.graph_names = CompiledModel::csv_names(wa.columns);
          for (const auto& c : wa.columns) {
            const double* p = wex.value_ptr(c.slot);
            for (int64_t k = 0; k < c.len; ++k) r.graph_row.push_back(p[k]);
          }
          r.have_graph_row = true;
        } catch (const std::exception& e) {
          r.wa_error = std::string("graph write_array: ") + e.what();
        }
      }
      if (wa.interp) {
        try {
          // Two seeds, one interpreter: the columns that move between them
          // are the RNG-tainted ones, and they are excluded rather than
          // compared. Column DISCOVERY happens on the first eval, so the
          // second eval is guaranteed the same column list.
          WaRng rng_a(1234);
          r.interp_row = wa.interp->eval(cm.constrained_env(*ex), rng_a);
          r.interp_names = CompiledModel::csv_names(wa.interp->columns());
          WaRng rng_b(987654321u);
          r.interp_row_b = wa.interp->eval(cm.constrained_env(*ex), rng_b);
          r.have_interp_row = true;
        } catch (const std::exception& e) {
          r.wa_error += std::string(r.wa_error.empty() ? "" : "; ") +
                        "interp write_array: " + e.what();
        }
      }
      r.paths.wa_columns =
          r.have_graph_row ? r.graph_names.size() : r.interp_names.size();
    }
  } catch (const std::exception& e) {
    r.wa_error += std::string(r.wa_error.empty() ? "" : "; ") + e.what();
  }
  return r;
}

// ---- the ledger ------------------------------------------------------------

// A declared divergence: visible, bounded, explained and dated. `cause` is
// mandatory -- an entry that does not name the mechanism is a tolerance
// wearing a disguise, and load() refuses it.
//
// `bound_ulp` is the largest distance the entry excuses. -1 excuses any
// distance and is reserved for an OPEN BUG: a divergence that is not
// acceptable, only known, so the corpus can keep driving the fixture that
// exposes it instead of quietly dropping it. Every -1 entry is a debt with
// a date on it.
struct LedgerEntry {
  std::string model;
  std::string quantity;  // one '*' matches any run: "grad[*]"
  std::string pair;
  int64_t bound_ulp = 0;
  std::string cause;
  std::string since;

  bool excuses(bool comparable, int64_t distance) const {
    if (bound_ulp < 0) return true;
    return comparable && distance <= bound_ulp;
  }
};

struct Ledger {
  std::vector<LedgerEntry> entries;
  std::string error;  // nonempty means the file is malformed; that fails

  static bool matches(const std::string& pattern, const std::string& s) {
    const size_t star = pattern.find('*');
    if (star == std::string::npos) return pattern == s;
    const size_t tail = pattern.size() - star - 1;
    return s.size() >= star + tail &&
           s.compare(0, star, pattern, 0, star) == 0 &&
           s.compare(s.size() - tail, tail, pattern, star + 1, tail) == 0;
  }

  // An entry names the mechanism, not the point, so "lp" also excuses
  // "lp@2".
  const LedgerEntry* find(const std::string& model, const std::string& quantity,
                          const std::string& pair) const {
    const std::string q = quantity.substr(0, quantity.find('@'));
    for (const LedgerEntry& e : entries)
      if (e.model == model && e.pair == pair && matches(e.quantity, q))
        return &e;
    return nullptr;
  }

  static Ledger load(const std::string& path) {
    Ledger led;
    std::ifstream f(path);
    if (!f) {
      // An absent ledger is an empty ledger: every divergence fails. That
      // is the right default for a checkout that has not run the harness.
      return led;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    nlohmann::json j;
    try {
      j = nlohmann::json::parse(ss.str());
    } catch (const std::exception& e) {
      led.error = std::string(path) + ": " + e.what();
      return led;
    }
    if (!j.is_array()) {
      led.error = path + ": expected a JSON array of entries";
      return led;
    }
    for (const auto& item : j) {
      LedgerEntry e;
      e.model = item.value("model", std::string());
      e.quantity = item.value("quantity", std::string());
      e.pair = item.value("pair", std::string());
      e.bound_ulp = item.value("bound_ulp", (int64_t)0);
      e.cause = item.value("cause", std::string());
      e.since = item.value("since", std::string());
      if (e.model.empty() || e.quantity.empty() || e.pair.empty()) {
        led.error = path + ": entry needs model, quantity and pair";
        return led;
      }
      if (e.cause.empty()) {
        led.error = path + ": entry " + e.model + " " + e.quantity + " " +
                    e.pair + " has no cause; the field is mandatory";
        return led;
      }
      led.entries.push_back(std::move(e));
    }
    return led;
  }
};

// ---- the comparison matrix -------------------------------------------------

struct Options {
  std::string model;        // the name the report prints and the ledger keys
  std::string repro_model;  // paths for the repro line
  std::string repro_data;
  std::string repro_mir;  // the MIR file the matrix ran, when it read one
  std::string only;       // "", "lp", "grad" or "wa": limits which rows run
  int variant = -1;       // -1 walks the variants until one is in support
  int points = kDefaultPoints;
  // For the by-hand posteriordb loop only, where reassociating passes are
  // expected to move low bits: a finite mismatch whose scaled error
  // |a - b| / max(|a|, 1e-300) is at most this passes. 0 keeps every row
  // bitwise, which is what CTest runs.
  double rel_tol = 0.0;
  // Also compare at stanli_check's default point and under
  // corpus_configs().
  bool corpus = false;
};

struct Result {
  bool ok = true;
  int variant = 0;
  int comparisons = 0;
  int failures = 0;
  int declared = 0;
  int within_tol = 0;
  // Row 5's accounting, so a fully RNG-tainted section reports zero columns
  // compared instead of passing vacuously.
  int wa_compared = 0;
  int wa_rng_excluded = 0;
  int wa_name_excluded = 0;
  bool wa_ran = false;
  Paths paths;  // the default configuration's
  // Per configuration, because "which engines does this model reach" has a
  // different answer under each switch and the coverage floors want all of
  // them: the carver's islands only exist under island_always on models
  // this small, and their generated adjoints only exist with them.
  std::map<std::string, Paths> by_config;
  std::map<std::string, bool> compiled;  // config name -> compiled at all
  // Config name -> it compiled or ran something its baseline did not: a
  // different compile outcome, fingerprint or structured-loop diagnostics.
  std::map<std::string, bool> changed;
  std::string skipped;  // nonempty: the default config could not be driven
  std::string report;
};

class Matrix {
 public:
  Matrix(const Options& opt, const Ledger& led) : opt_(opt), led_(led) {}

  Result run(const std::string& mir_text, const DataMap& data) {
    // Variant walk: a model out of support at one point is not a finding,
    // and the drivers already treat it that way. The winning probe IS the
    // default configuration's run, so walking costs nothing extra once a
    // point is in support.
    std::vector<Run> runs;
    int variant = opt_.variant < 0 ? 0 : opt_.variant;
    Run base = evaluate(mir_text, data, configs()[0], variant, true,
                        opt_.points, opt_.corpus);
    while (opt_.variant < 0 && base.compiled && !base.evaluated &&
           variant + 1 < kDrawVariants)
      base = evaluate(mir_text, data, configs()[0], ++variant, true,
                      opt_.points, opt_.corpus);
    res_.variant = variant;
    res_.compiled["default"] = base.compiled;
    res_.by_config["default"] = base.paths;
    if (!base.compiled) {
      res_.skipped = "COMPILE_FAIL " + base.compile_error;
      return std::move(res_);
    }
    if (!base.evaluated && opt_.variant < 0) {
      res_.skipped = "EVAL_FAIL at every draw variant: " + base.eval_error;
      return std::move(res_);
    }
    res_.paths = base.paths;
    // An ODE whose right-hand side runs in MirInterp costs about a hundred
    // ordinary evaluations, so those models are compared at point 0 only.
    const int points = base.paths.ode_interp > 0 ? 1 : opt_.points;
    if ((int)base.points.size() > points) base.points.resize(points);
    runs.push_back(std::move(base));
    std::map<std::string, size_t> index{{"default", 0}};
    std::vector<Config> run_configs(configs().begin() + 1, configs().end());
    if (opt_.corpus)
      run_configs.insert(run_configs.end(), corpus_configs().begin(),
                         corpus_configs().end());
    for (const Config& c : run_configs) {
      const auto base_it =
          index.find(c.baseline.empty() ? "default" : c.baseline);
      if (base_it == index.end()) continue;
      const Paths& base_paths = runs[base_it->second].paths;
      if (c.requires_op == Requires::RegionMap && base_paths.region_maps == 0)
        continue;
      index[c.name] = runs.size();
      runs.push_back(
          evaluate(mir_text, data, c, variant, false, points, opt_.corpus));
      res_.compiled[c.name] = runs.back().compiled;
      res_.by_config[c.name] = runs.back().paths;
    }
    // Necessity islands are the ones STANLI_NO_ISLAND leaves standing: it
    // disables the carver and nothing else, and the carver cannot absorb an
    // OP_ISLAND (islands do not nest), so the subtraction is exact.
    if (res_.compiled["no_island"]) {
      const int necessity = res_.by_config["no_island"].islands;
      for (auto& [name, p] : res_.by_config) {
        p.necessity = necessity;
        p.carved = p.islands - necessity;
      }
      res_.paths = res_.by_config["default"];
      // So the failure blocks below print the split too.
      for (size_t i = 0; i < runs.size(); ++i)
        runs[i].paths = res_.by_config[runs[i].config];
    }

    for (size_t i = 1; i < runs.size(); ++i) {
      const std::string& baseline = config_named(runs[i].config).baseline;
      const Run& base = runs[index.at(baseline.empty() ? "default" : baseline)];
      res_.changed[runs[i].config] =
          base.compiled != runs[i].compiled ||
          base.paths.fingerprint != runs[i].paths.fingerprint ||
          base.paths.diagnostics != runs[i].paths.diagnostics;
      if (!baseline.empty()) compare_run(base, runs[i]);
    }
    if (want("wa")) compare_write_array(runs[0]);
    res_.ok = res_.failures == 0;
    return std::move(res_);
  }

 private:
  bool want(const char* row) const {
    return opt_.only.empty() || opt_.only == row;
  }

  // Rows 1, 2, 4 and 6: lp and gradient, default against each other config.
  // The gate is bitwise for all of them. The gradient rows are where the
  // design pre-authorizes ledger entries up to 2 ULP with the nested-tape
  // regrouping named as the cause -- pre-authorized, not automatic: a
  // 1 ULP drift still needs its entry, so it stays visible.
  void compare_run(const Run& a, const Run& b) {
    const std::string pair = a.config + "/" + b.config;
    if (a.compiled != b.compiled || a.evaluated != b.evaluated) {
      fail_outcome(a, b, pair);
      return;
    }
    if (!a.evaluated) {
      // Both refused, and refusing identically is agreement. Different
      // refusals are not.
      if (a.eval_error != b.eval_error) fail_outcome(a, b, pair);
      return;
    }
    if (a.points.size() != b.points.size()) {
      fail_outcome(a, b, pair);
      return;
    }
    for (size_t k = 0; k < a.points.size(); ++k)
      compare_point(a, b, pair, a.points[k], b.points[k],
                    k == 0 ? "" : "@" + std::to_string(k));
    if (a.has_check && b.has_check)
      compare_point(a, b, pair, a.check, b.check, "@check");
  }

  void compare_point(const Run& a, const Run& b, const std::string& pair,
                     const PointRun& pa, const PointRun& pb,
                     const std::string& at) {
    if (pa.evaluated != pb.evaluated ||
        (!pa.evaluated && pa.eval_error != pb.eval_error)) {
      emit(a, b, pair, "outcome" + at,
           pa.evaluated ? "value lp " + fmt_value(pa.lp)
                        : "eval_error: " + pa.eval_error,
           pb.evaluated ? "value lp " + fmt_value(pb.lp)
                        : "eval_error: " + pb.eval_error,
           "");
      return;
    }
    if (!pa.evaluated) return;
    if (want("lp")) check(a, b, pair, "lp" + at, pa.lp, pb.lp);
    if (want("grad")) {
      if (pa.grad.size() != pb.grad.size()) {
        emit(a, b, pair, "grad.size" + at,
             std::to_string(pa.grad.size()) + " values",
             std::to_string(pb.grad.size()) + " values", "");
        return;
      }
      for (size_t i = 0; i < pa.grad.size(); ++i)
        check(a, b, pair, "grad[" + std::to_string(i) + "]" + at, pa.grad[i],
              pb.grad[i]);
    }
  }

  // Row 5: the write_array graph against the interpreted write_array, same
  // model, same draw, both engines attached by STANLI_WA_FORCE_INTERP.
  void compare_write_array(const Run& a) {
    const std::string pair = "graph/wa_interp";
    if (!a.have_graph_row && !a.have_interp_row) return;  // no section
    if (a.have_graph_row && !a.have_interp_row) {
      // A truncated graph evaluated only a prefix, so success says nothing
      // about a rejection in the unlowered suffix. Keep complete-graph
      // outcome comparisons strict; report this incomplete comparison.
      if (a.paths.wa == "truncated+interp") {
        line(
            "CROSS NOTE " + opt_.model +
            " write_array prefix succeeded but the full interpreter refused: " +
            a.wa_error);
        return;
      }
      // The graph lowered the section and the interpreter refused it. That
      // is the loud form of the gap this axis exists to find: a
      // generated-quantities function one vocabulary table has and another
      // does not, so the section has one implementation rather than two and
      // no cross-check at all. Declarable, like any other divergence, but
      // never silent.
      if (led_.find(opt_.model, "write_array", pair)) {
        ++res_.declared;
        return;
      }
      emit_pair(pair, "write_array", "graph", "produced a row", "wa_interp",
                a.wa_error.empty() ? "produced no row" : a.wa_error, a.paths);
      return;
    }
    if (!a.have_graph_row) {
      // The mirror case is not a failure: a section the graph could not
      // lower at all is reported as truncation on the coverage line, and
      // the interpreter being its only engine is the thing that line says.
      line("CROSS NOTE " + opt_.model +
           " write_array is interpreter-only, no graph row to compare");
      return;
    }
    res_.wa_ran = true;
    // Name sets first: an RNG draw feeding an integer size or index moves
    // the column SET, not just values, and intersecting without saying so
    // would hide that.
    std::map<std::string, size_t> gi, ii;
    for (size_t k = 0; k < a.graph_names.size(); ++k) gi[a.graph_names[k]] = k;
    for (size_t k = 0; k < a.interp_names.size(); ++k)
      ii[a.interp_names[k]] = k;
    std::vector<std::string> only_graph, only_interp;
    for (const auto& [n, k] : gi)
      if (!ii.count(n)) only_graph.push_back(n);
    for (const auto& [n, k] : ii)
      if (!gi.count(n)) only_interp.push_back(n);
    res_.wa_name_excluded = (int)(only_graph.size() + only_interp.size());
    if (res_.wa_name_excluded > 0)
      line("CROSS NOTE " + opt_.model + " write_array column sets differ: " +
           std::to_string(only_graph.size()) + " graph-only" +
           (only_graph.empty() ? "" : " (" + only_graph.front() + " ...)") +
           ", " + std::to_string(only_interp.size()) + " interp-only" +
           (only_interp.empty() ? "" : " (" + only_interp.front() + " ...)"));

    for (const auto& [name, k] : gi) {
      auto it = ii.find(name);
      if (it == ii.end()) continue;
      const size_t j = it->second;
      if (k >= a.graph_row.size() || j >= a.interp_row.size()) continue;
      // RNG taint: the interpreter drew this column from a stream, so the
      // two seeds moved it. The graph receives the first interpreter seed,
      // which tests exact same-stream behavior separately; this matrix still
      // excludes stochastic values so it compares only deterministic columns.
      if (!same_bits(a.interp_row[j], a.interp_row_b[j])) {
        ++res_.wa_rng_excluded;
        continue;
      }
      ++res_.wa_compared;
      check_values(pair, name, a.graph_row[k], a.interp_row[j], "graph",
                   "wa_interp", a.paths);
    }
  }

  void check(const Run& a, const Run& b, const std::string& pair,
             const std::string& quantity, double x, double y) {
    check_values(pair, quantity, x, y, a.config, b.config, a.paths);
  }

  void check_values(const std::string& pair, const std::string& quantity,
                    double x, double y, const std::string& name_a,
                    const std::string& name_b, const Paths& paths) {
    ++res_.comparisons;
    if (same_bits(x, y)) return;
    const bool comparable = ulp_comparable(x, y);
    const int64_t dist = comparable ? ulp_distance(x, y) : -1;
    const LedgerEntry* e = led_.find(opt_.model, quantity, pair);
    if (e && e->excuses(comparable, dist)) {
      ++res_.declared;
      return;
    }
    if (opt_.rel_tol > 0.0 && comparable &&
        std::abs(x - y) / std::max(std::abs(x), 1e-300) <= opt_.rel_tol) {
      ++res_.within_tol;
      return;
    }
    std::string suffix =
        comparable ? "   " + std::to_string(dist) + " ulp" : "   not finite";
    emit_pair(pair, quantity, name_a, fmt_value(x), name_b,
              fmt_value(y) + suffix, paths);
  }

  void fail_outcome(const Run& a, const Run& b, const std::string& pair) {
    auto describe = [](const Run& r) {
      if (!r.compiled) return "compile_error: " + r.compile_error;
      if (!r.evaluated) return "eval_error: " + r.eval_error;
      return "value lp " + fmt_value(r.points[0].lp);
    };
    emit_pair(pair, "outcome", a.config, describe(a), b.config, describe(b),
              a.paths);
  }

  void emit(const Run& a, const Run& b, const std::string& pair,
            const std::string& quantity, const std::string& va,
            const std::string& vb, const std::string&) {
    emit_pair(pair, quantity, a.config, va, b.config, vb, a.paths);
  }

  // The failure block, designed backwards from "paste one line and see it
  // again": both values to 17 digits with raw bits and a ULP distance, the
  // pair and quantity named, the draw identified by variant index rather
  // than by its floats, and the coverage line so "which engine even ran"
  // is not a follow-up question.
  void emit_pair(const std::string& pair, const std::string& quantity,
                 const std::string& name_a, const std::string& va,
                 const std::string& name_b, const std::string& vb,
                 const Paths& paths) {
    ++res_.failures;
    const size_t slash = pair.find('/');
    const std::string right =
        slash == std::string::npos ? pair : pair.substr(slash + 1);
    std::string prefix = env_prefix(right);
    if (right == "wa_interp" || right == "graph")
      prefix = "STANLI_WA_FORCE_INTERP=1 ";
    const size_t w =
        name_a.size() > name_b.size() ? name_a.size() : name_b.size();
    line("CROSS FAIL " + opt_.model + " " + quantity + " " +
         (slash == std::string::npos ? pair
                                     : pair.substr(0, slash) + " vs " + right));
    line("  " + name_a + std::string(w - name_a.size() + 2, ' ') + va);
    line("  " + name_b + std::string(w - name_b.size() + 2, ' ') + vb);
    line("  repro: " + prefix + "build/stanli_check " +
         (opt_.repro_model.empty() ? "MODEL.stan" : opt_.repro_model) + " " +
         (opt_.repro_data.empty() ? "DATA.json" : opt_.repro_data) +
         (opt_.repro_mir.empty() ? "" : " --mir " + opt_.repro_mir) +
         " --cross --cross-one " + row_of(quantity) + " --draw-variant " +
         std::to_string(res_.variant) +
         (opt_.points == kDefaultPoints
              ? std::string()
              : " --cross-points " + std::to_string(opt_.points)) +
         (opt_.rel_tol > 0.0 ? " --cross-rel-tol " + fmt_tol(opt_.rel_tol)
                             : std::string()) +
         (opt_.corpus ? " --cross-corpus" : ""));
    line("  paths: " + paths.line());
  }

  static std::string fmt_tol(double t) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", t);
    return buf;
  }

  static std::string row_of(const std::string& quantity) {
    const std::string q = quantity.substr(0, quantity.find('@'));
    if (q == "lp" || q == "outcome") return "lp";
    if (q.compare(0, 4, "grad") == 0) return "grad";
    return "wa";
  }

  void line(const std::string& s) { res_.report += s + "\n"; }

  Options opt_;
  const Ledger& led_;
  Result res_;
};

inline Result run_matrix(const std::string& mir_text, const DataMap& data,
                         const Options& opt, const Ledger& led) {
  return Matrix(opt, led).run(mir_text, data);
}

// ---- the coverage report ---------------------------------------------------

// `--paths`: which engines a model actually reaches. Two compiles, because
// the necessity/carved split is the difference the carver makes and there
// is no per-island flag saying which produced it.
inline Paths path_report(const std::string& mir_text, const DataMap& data,
                         std::string* error) {
  Paths p;
  {
    EnvScope env({"STANLI_WA_FORCE_INTERP"});
    try {
      CompiledModel cm = compile_model(mir_text, data);
      scan_graph(cm.graph, &p);
      if (!cm.write_array)
        p.wa = "none";
      else if (cm.write_array->interp)
        p.wa = cm.write_array->truncated.empty() ? "graph+interp"
                                                 : "truncated+interp";
      else
        p.wa = cm.write_array->columns.empty() ? "empty" : "graph";
      p.wa_columns =
          cm.write_array ? cm.write_array->columns.size() : (size_t)0;
    } catch (const std::exception& e) {
      if (error) *error = e.what();
      return p;
    }
  }
  {
    EnvScope env({"STANLI_NO_ISLAND"});
    try {
      CompiledModel cm = compile_model(mir_text, data);
      Paths q;
      scan_graph(cm.graph, &q);
      p.necessity = q.islands;
      p.carved = p.islands - q.islands;
    } catch (const std::exception&) {
      // The carver-off compile failing is itself worth nothing here; the
      // matrix reports it as an outcome mismatch.
    }
  }
  return p;
}

}  // namespace cross
}  // namespace stanli

#endif
