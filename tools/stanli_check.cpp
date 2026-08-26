// stanli_check: compile model.stan + data.json and evaluate log_prob +
// gradient at a deterministic unconstrained point. Machine-readable output:
//   OK <lp> <g0> <g1> ...        on success
//   COMPILE_FAIL <first line of error>
//   EVAL_FAIL <what>       only when evaluation THREW; a nonfinite lp
//                          or gradient is printed as a value, matching
//                          ref_driver.cpp so the two can be compared
// Used by tools/corpus.py to build the coverage scoreboard, and by the
// reference harness to compare against CmdStan at the same point.
//
// Two other modes, both stanli-against-itself rather than stanli against
// CmdStan (tests/cross_path.hpp has the full account):
//   --paths   which engines this model reaches -- islands, adjoints,
//             write_array mode, ODE mode
//   --cross   the cross-path agreement matrix: recompile once per engine
//             configuration and demand the answers agree bitwise
#include "../tests/cross_path.hpp"

#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/wa_interp.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Deterministic evaluation points, shared with tools/ref_driver.cpp. Some
// models are invalid at a given point (an ODE solution dips below a declared
// lower bound, say) and both engines reject it; the harness then retries the
// next variant so those models still get a real comparison.
static double eval_point(int64_t i, int variant) {
  switch (variant) {
    case 1:
      return 0.02 * static_cast<double>((i % 5) - 2);
    case 2:
      return 0.0;
    default:
      return 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
  }
}

static std::string run_stanc(const std::string& stanc,
                             const std::string& model) {
  const std::string cmd =
      stanc + " --O1 --debug-optimized-mir '" + model + "' 2>/dev/null";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) throw std::runtime_error("cannot run stanc");
  std::string out;
  std::array<char, 1 << 16> buf;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), pipe.get())) > 0)
    out.append(buf.data(), n);
  return out;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: stanli_check model.stan data.json "
                 "[--stanc PATH] [--point N] [--columns]\n"
                 "       [--paths] [--cross [--cross-one lp|grad|wa] "
                 "[--draw-variant N] [--ledger PATH]]\n");
    return 2;
  }
  std::string stanc = "deps/stanc3/stanc";
  int variant = 0;
  bool columns_only = false;
  bool wa_values = false;
  bool paths_only = false;
  bool cross = false;
  std::string cross_one;
  std::string ledger_path = "tests/cross_path_ledger.json";
  int draw_variant = -1;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--columns")
      columns_only = true;
    else if (a == "--wa-values")
      wa_values = true;
    else if (a == "--paths")
      paths_only = true;
    else if (a == "--cross")
      cross = true;
    else if (a == "--cross-one" && i + 1 < argc)
      cross_one = argv[++i];
    else if (a == "--draw-variant" && i + 1 < argc)
      draw_variant = std::atoi(argv[++i]);
    else if (a == "--ledger" && i + 1 < argc)
      ledger_path = argv[++i];
    else if (a == "--stanc" && i + 1 < argc)
      stanc = argv[++i];
    else if (a == "--point" && i + 1 < argc)
      variant = std::atoi(argv[++i]);
  }
  if (const char* env = std::getenv("STANC")) stanc = env;

  std::string mir;
  try {
    mir = run_stanc(stanc, argv[1]);
    if (mir.empty()) {
      std::printf("COMPILE_FAIL stanc produced no MIR\n");
      return 1;
    }
  } catch (const std::exception& e) {
    std::printf("COMPILE_FAIL stanc: %s\n", e.what());
    return 1;
  }

  // --paths / --cross: stanli against itself. Both need the data and the
  // MIR but not the ordinary single compile, so they answer here and exit.
  if (paths_only || cross) {
    stanli::DataMap data;
    try {
      data = stanli::DataMap::from_json_file(argv[2]);
    } catch (const std::exception& e) {
      std::printf("COMPILE_FAIL data: %s\n", e.what());
      return 1;
    }
    // The model name the report prints and the ledger keys on: the .stan
    // basename, so an entry survives the file being moved.
    std::string name = argv[1];
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const size_t dot = name.rfind(".stan");
    if (dot != std::string::npos) name = name.substr(0, dot);

    if (paths_only) {
      std::string err;
      const stanli::cross::Paths p =
          stanli::cross::path_report(mir, data, &err);
      if (!err.empty()) {
        std::printf("COMPILE_FAIL %s\n", err.c_str());
        return 1;
      }
      std::printf("PATHS %s ops %lld, %s\n", name.c_str(), (long long)p.ops,
                  p.line().c_str());
      if (!cross) return 0;
    }

    const stanli::cross::Ledger led = stanli::cross::Ledger::load(ledger_path);
    if (!led.error.empty()) {
      std::printf("CROSS LEDGER_BAD %s\n", led.error.c_str());
      return 1;
    }
    stanli::cross::Options opt;
    opt.model = name;
    opt.repro_model = argv[1];
    opt.repro_data = argv[2];
    opt.only = cross_one;
    opt.variant = draw_variant;
    const stanli::cross::Result r =
        stanli::cross::run_matrix(mir, data, opt, led);
    std::fputs(r.report.c_str(), stdout);
    if (!r.skipped.empty()) {
      std::printf("CROSS SKIP %s %s\n", name.c_str(), r.skipped.c_str());
      return 0;
    }
    std::printf(
        "CROSS %s %s variant %d: %d comparisons, %d failures, %d declared; "
        "write_array %d columns compared, %d rng-excluded, %d "
        "name-set-excluded\n",
        r.ok ? "OK" : "FAIL", name.c_str(), r.variant, r.comparisons,
        r.failures, r.declared, r.wa_compared, r.wa_rng_excluded,
        r.wa_name_excluded);
    std::printf("  paths: %s\n", r.paths.line().c_str());
    return r.ok ? 0 : 1;
  }

  stanli::CompiledModel cm;
  try {
    stanli::DataMap data = stanli::DataMap::from_json_file(argv[2]);
    cm = stanli::compile_model(mir, data);
  } catch (const std::exception& e) {
    std::string what = e.what();
    const size_t nl = what.find('\n');
    if (nl != std::string::npos) what = what.substr(0, nl);
    std::printf("COMPILE_FAIL %s\n", what.c_str());
    return 1;
  }

  // --columns: the CSV header the write_array graph would write, for
  // comparison against CmdStan's (verify_refs.py --wa-headers).
  if (columns_only) {
    if (!cm.write_array || cm.write_array->columns.empty()) {
      std::printf("NO_COLUMNS %s\n", cm.write_array
                                         ? cm.write_array->truncated.c_str()
                                         : "no generate_quantities section");
      return 1;
    }
    std::string h;
    for (const auto& n :
         stanli::CompiledModel::csv_names(cm.write_array->columns)) {
      if (!h.empty()) h += ',';
      h += n;
    }
    std::printf("%s\n", h.c_str());
    if (!cm.write_array->truncated.empty())
      std::fprintf(stderr, "TRUNCATED %s\n", cm.write_array->truncated.c_str());
    return 0;
  }

  try {
    stanli::Executor ex(std::move(cm.graph));
    cm.bind(ex);
    // STANLI_PROFILE=1: per-opcode accounting for the single gradient
    // evaluation, printed to stderr. Same switch as stanli_run.
    const char* prof_env = std::getenv("STANLI_PROFILE");
    if (prof_env && prof_env[0] != '0') ex.set_profile(true);
    const int64_t n = ex.n_params();
    for (int64_t i = 0; i < n; ++i)
      ex.params_data()[i] = eval_point(i, variant);
    std::vector<double> grad(n, 0.0);
    const double lp = ex.gradient(grad.data());
    // A nonfinite value is reported, not refused. ref_driver.cpp -- the
    // CmdStan side of every comparison -- prints whatever log_prob
    // returned, and the comparisons are all nonfinite-safe (pair_dev in
    // verify_refs.py: the same infinity on both sides is agreement, one
    // side alone is an infinite deviation). Refusing here made the two
    // drivers asymmetric, so the oracle could never confirm agreement at
    // -inf and reported a real disagreement as a run failure instead of a
    // mismatch. bernoulli_lccdf(1 | theta) is log(0) by definition and
    // both engines return -inf with a zero gradient; that is a pass.
    //
    // The diagnostic value of noticing goes to stderr, where it does not
    // disturb the machine-readable contract on stdout.
    int n_bad = 0;
    for (double g : grad)
      if (!std::isfinite(g)) ++n_bad;
    if (!std::isfinite(lp) || n_bad > 0)
      std::fprintf(stderr, "stanli_check: nonfinite lp=%d gradients=%d\n",
                   std::isfinite(lp) ? 0 : 1, n_bad);
    if (prof_env && prof_env[0] != '0')
      std::fprintf(stderr, "%s", ex.profile_report().c_str());

    // The write_array graph, exercised at the same point. Reported on stderr
    // so the machine-readable stdout contract is unchanged; this is what the
    // corpus sweep reads to say how many models get their transformed
    // parameters and generated quantities.
    if (!cm.write_array) {
      std::fprintf(stderr, "WA none\n");
    } else if (cm.write_array->interp) {
      // The graph could not express the whole section; the per-draw
      // interpreter runs all of it. Failures stay on stderr; the final
      // machine-readable status is emitted after this section so a Stan
      // print whose text begins with OK/EVAL_FAIL cannot shadow it.
      try {
        stanli::WaInterp& wi = *cm.write_array->interp;
        stanli::WaRng rng(1234);
        const std::vector<double> row = wi.eval(cm.constrained_env(ex), rng);
        int64_t bad = 0;
        for (double x : row)
          if (!std::isfinite(x)) ++bad;
        std::fprintf(stderr,
                     "WA %zu vars %lld values %lld nonfinite complete "
                     "(interpreted: %s)\n",
                     wi.columns().size(), (long long)row.size(), (long long)bad,
                     cm.write_array->truncated.c_str());
        if (wa_values) {
          std::string joined;
          for (const auto& nm :
               stanli::CompiledModel::csv_names(wi.columns())) {
            if (!joined.empty()) joined += ',';
            joined += nm;
          }
          std::printf("WANAMES %s\nWAVALS", joined.c_str());
          for (double x : row) std::printf(" %.17g", x);
          std::printf("\n");
        }
      } catch (const std::exception& we) {
        std::fprintf(stderr, "WA empty interp: %s\n", we.what());
        if (wa_values) std::printf("WANAMES FAIL %s\nWAVALS FAIL\n", we.what());
      }
    } else if (cm.write_array->columns.empty()) {
      std::fprintf(stderr, "WA empty %s\n", cm.write_array->truncated.c_str());
    } else {
      try {
        stanli::Executor wex(std::move(cm.write_array->graph));
        cm.write_array->bind(wex);
        for (int64_t i = 0; i < wex.n_params(); ++i)
          wex.params_data()[i] = eval_point(i, variant);
        stanli::WaRng rng(1234);
        wex.run_forward_only(stanli::EvalState{&rng});
        int64_t width = 0, bad = 0;
        for (const auto& c : cm.write_array->columns) {
          const double* p = wex.value_ptr(c.slot);
          for (int64_t i = 0; i < c.len; ++i, ++width)
            if (!std::isfinite(p[i])) ++bad;
        }
        std::fprintf(stderr, "WA %zu vars %lld values %lld nonfinite %s\n",
                     cm.write_array->columns.size(), (long long)width,
                     (long long)bad,
                     cm.write_array->truncated.empty()
                         ? "complete"
                         : ("truncated: " + cm.write_array->truncated).c_str());
        if (wa_values) {
          std::string joined;
          for (const auto& nm :
               stanli::CompiledModel::csv_names(cm.write_array->columns)) {
            if (!joined.empty()) joined += ',';
            joined += nm;
          }
          std::printf("WANAMES %s\nWAVALS", joined.c_str());
          for (const auto& c : cm.write_array->columns) {
            const double* p = wex.value_ptr(c.slot);
            for (int64_t i = 0; i < c.len; ++i) std::printf(" %.17g", p[i]);
          }
          std::printf("\n");
        }
      } catch (const std::exception& we) {
        std::fprintf(stderr, "WA empty graph: %s\n", we.what());
        if (wa_values) std::printf("WANAMES FAIL %s\nWAVALS FAIL\n", we.what());
      }
    }
    if (wa_values && (!cm.write_array || (!cm.write_array->interp &&
                                          cm.write_array->columns.empty())))
      std::printf("WANAMES FAIL no write_array\nWAVALS FAIL\n");
    std::printf("OK %.17g", lp);
    for (double g : grad) std::printf(" %.17g", g);
    std::printf("\n");
  } catch (const std::exception& e) {
    std::printf("EVAL_FAIL %s\n", e.what());
    return 1;
  }
  return 0;
}
