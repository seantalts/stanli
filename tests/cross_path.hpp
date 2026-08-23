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
// The configurations, all of them existing compile-time switches except the
// last, which is the ten-line hook in lower.cpp:
//
//   default        the shipped pipeline
//   no_island      STANLI_NO_ISLAND: carver off, graph ops stay
//   island_always  STANLI_ISLAND_ALWAYS: carve past the cost estimate
//   no_native_adj  STANLI_NO_NATIVE_ADJ: islands replay under var
//   passes_off     STANLI_NO_REROLL, STANLI_NO_INPLACE, STANLI_NO_CONSTFOLD
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
#include <stanli/island.hpp>
#include <stanli/ode.hpp>
#include <stanli/optable.hpp>
#include <stanli/wa_interp.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
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

// ---- environment -----------------------------------------------------------

struct Config {
  std::string name;
  std::vector<std::string> vars;
};

// Every variable the harness ever sets, so a scope can clear the lot
// regardless of which config set what. A leftover STANLI_NO_ISLAND from a
// previous config would silently make the next comparison vacuous.
inline const std::vector<std::string>& harness_vars() {
  static const std::vector<std::string> v = {
      "STANLI_NO_ISLAND",      "STANLI_ISLAND_ALWAYS", "STANLI_NO_NATIVE_ADJ",
      "STANLI_NO_REROLL",      "STANLI_NO_INPLACE",    "STANLI_NO_CONSTFOLD",
      "STANLI_WA_FORCE_INTERP"};
  return v;
}

class EnvScope {
 public:
  explicit EnvScope(const std::vector<std::string>& set) {
    for (const std::string& v : harness_vars()) test_unsetenv(v.c_str());
    for (const std::string& v : set) test_setenv(v.c_str(), "1", 1);
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
      {"no_native_adj", {"STANLI_NO_NATIVE_ADJ"}},
      {"passes_off",
       {"STANLI_NO_REROLL", "STANLI_NO_INPLACE", "STANLI_NO_CONSTFOLD"}},
  };
  return c;
}

inline std::string env_prefix(const std::string& config_name) {
  for (const Config& c : configs())
    if (c.name == config_name) {
      std::string s;
      for (const std::string& v : c.vars) s += v + "=1 ";
      return s;
    }
  return "";
}

// ---- which engines actually ran --------------------------------------------

struct Paths {
  int64_t ops = 0;
  int islands = 0;
  // Necessity islands are not distinguishable from carved ones on the
  // island itself, so the split is derived: it is what survives
  // STANLI_NO_ISLAND. Zero until the matrix has that second compile.
  int necessity = 0;
  int carved = 0;
  int native_adj = 0;  // islands running their generated backward
  int ode = 0;
  int ode_prog = 0;         // right-hand side compiled
  int ode_interp = 0;       // right-hand side falls back to MirInterp under var
  std::string wa = "none";  // none | graph | interp | truncated+interp
  size_t wa_columns = 0;

  std::string line() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "islands %d (%d necessity, %d carved), native_adj %d/%d, "
                  "wa %s, ode %d (%d program, %d interp)",
                  islands, necessity, carved, native_adj, islands, wa.c_str(),
                  ode, ode_prog, ode_interp);
    return buf;
  }
};

inline void scan_graph(const Graph& g, Paths* p) {
  p->ops = (int64_t)g.ops.size();
  for (const Op& op : g.ops) {
    if (op.opcode == OP_ISLAND) {
      ++p->islands;
      if (static_cast<const IslandProg*>(op.udata)->native_adj) ++p->native_adj;
    } else if (op.opcode == OP_ODE) {
      ++p->ode;
      if (static_cast<const OdeSpec*>(op.udata)->prog.ok)
        ++p->ode_prog;
      else
        ++p->ode_interp;
    }
  }
}

// ---- one configuration's answer --------------------------------------------

struct Run {
  std::string config;
  bool compiled = false;
  std::string compile_error;
  bool evaluated = false;
  std::string eval_error;
  double lp = 0.0;
  std::vector<double> grad;
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

// Compile and evaluate under `cfg`. Never throws: a compile or evaluation
// failure is a first-class outcome, because "one path threw and the other
// returned" is exactly the kind of disagreement worth catching.
inline Run evaluate(const std::string& mir_text, const DataMap& data,
                    const Config& cfg, int variant, bool want_write_array) {
  Run r;
  r.config = cfg.name;
  EnvScope env(cfg.vars);
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

  try {
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const int64_t n = ex.n_params();
    for (int64_t i = 0; i < n; ++i)
      ex.params_data()[i] = draw_point(i, variant);
    r.grad.assign((size_t)n, 0.0);
    r.lp = ex.gradient(r.grad.data());
    r.evaluated = true;

    if (want_write_array && cm.write_array) {
      auto& wa = *cm.write_array;
      // The graph side, if any of the section lowered. A truncated graph
      // still holds a real prefix and the prefix is worth differencing.
      if (!wa.columns.empty()) {
        try {
          Executor wex(std::move(wa.graph));
          wa.bind(wex);
          for (int64_t i = 0; i < wex.n_params(); ++i)
            wex.params_data()[i] = draw_point(i, variant);
          wex.run_forward_only();
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
          r.interp_row = wa.interp->eval(cm.constrained_env(ex), rng_a);
          r.interp_names = CompiledModel::csv_names(wa.interp->columns());
          WaRng rng_b(987654321u);
          r.interp_row_b = wa.interp->eval(cm.constrained_env(ex), rng_b);
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
    r.eval_error = e.what();
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
  std::string quantity;  // trailing '*' matches a prefix: "grad[*]"
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
    if (!pattern.empty() && pattern.back() == '*')
      return s.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) ==
             0;
    return pattern == s;
  }

  const LedgerEntry* find(const std::string& model, const std::string& quantity,
                          const std::string& pair) const {
    for (const LedgerEntry& e : entries)
      if (e.model == model && e.pair == pair && matches(e.quantity, quantity))
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
  std::string only;  // "", "lp", "grad" or "wa": limits which rows run
  int variant = -1;  // -1 walks the variants until one is in support
};

struct Result {
  bool ok = true;
  int variant = 0;
  int comparisons = 0;
  int failures = 0;
  int declared = 0;
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
    Run base = evaluate(mir_text, data, configs()[0], variant, true);
    while (opt_.variant < 0 && base.compiled && !base.evaluated &&
           variant + 1 < kDrawVariants)
      base = evaluate(mir_text, data, configs()[0], ++variant, true);
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
    runs.push_back(std::move(base));
    for (size_t i = 1; i < configs().size(); ++i) {
      runs.push_back(evaluate(mir_text, data, configs()[i], variant, false));
      res_.compiled[configs()[i].name] = runs.back().compiled;
      res_.by_config[configs()[i].name] = runs.back().paths;
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

    for (size_t i = 1; i < runs.size(); ++i) compare_run(runs[0], runs[i]);
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
    if (want("lp")) check(a, b, pair, "lp", a.lp, b.lp);
    if (want("grad")) {
      if (a.grad.size() != b.grad.size()) {
        emit(a, b, pair, "grad.size", std::to_string(a.grad.size()) + " values",
             std::to_string(b.grad.size()) + " values", "");
        return;
      }
      for (size_t i = 0; i < a.grad.size(); ++i)
        check(a, b, pair, "grad[" + std::to_string(i) + "]", a.grad[i],
              b.grad[i]);
    }
  }

  // Row 5: the write_array graph against the interpreted write_array, same
  // model, same draw, both engines attached by STANLI_WA_FORCE_INTERP.
  void compare_write_array(const Run& a) {
    const std::string pair = "graph/wa_interp";
    if (!a.have_graph_row && !a.have_interp_row) return;  // no section
    if (a.have_graph_row && !a.have_interp_row) {
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
      // two seeds moved it. Nothing to compare -- the graph never draws.
      // Today this excludes nothing, because graph lowering TRUNCATES at
      // the first RNG call, so a shared column is always upstream of every
      // draw. It stays because that is a property of the lowering rather
      // than of the comparison, and the count is reported so a section
      // whose shared columns all become RNG-tainted reads as zero columns
      // compared instead of as a vacuous pass.
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
    std::string suffix =
        comparable ? "   " + std::to_string(dist) + " ulp" : "   not finite";
    emit_pair(pair, quantity, name_a, fmt_value(x), name_b,
              fmt_value(y) + suffix, paths);
  }

  void fail_outcome(const Run& a, const Run& b, const std::string& pair) {
    auto describe = [](const Run& r) {
      if (!r.compiled) return "compile_error: " + r.compile_error;
      if (!r.evaluated) return "eval_error: " + r.eval_error;
      return "value lp " + fmt_value(r.lp);
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
         " --cross --cross-one " + row_of(quantity) + " --draw-variant " +
         std::to_string(res_.variant));
    line("  paths: " + paths.line());
  }

  static std::string row_of(const std::string& quantity) {
    if (quantity == "lp") return "lp";
    if (quantity.compare(0, 4, "grad") == 0) return "grad";
    if (quantity == "outcome") return "lp";
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
