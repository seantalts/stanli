// Constrained-scale starting values, checked by round trip.
//
// The layout is the whole difficulty here, and it cannot be settled by
// reading either side: a user's init is SERIAL (first logical index fastest,
// what JSON and every CSV column use) while the free vector is ARENA (array
// dimensions outer-major, an innermost matrix column-major inside its batch).
// The two orders differ for every parameter with more than one dimension.
//
// So the test drives the model's own forward direction and requires the
// inverse to undo it: take a free vector, run the graph to get the
// constrained values through the documented `constrained_env` boundary, hand
// those to the init interpreter, and require the original free vector back.
// Nothing here restates the mapping; a permutation error anywhere fails it.
#include "expect.hpp"

#include <stanli/compile.hpp>
#include <stanli/init_interp.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

// A free point that is in support for every transform in the fixture.
std::vector<double> free_point(int64_t n, int variant) {
  std::vector<double> q((size_t)n);
  for (int64_t i = 0; i < n; ++i)
    q[(size_t)i] = variant == 0
                       ? 0.1 + 0.05 * (double)(i % 7) - 0.15 * (double)(i % 3)
                       : -0.4 + 0.11 * (double)(i % 5) + 0.03 * (double)(i % 4);
  return q;
}

void round_trip(stanli::CompiledModel& cm, int variant) {
  using namespace stanli;
  const std::string tag = "round trip variant " + std::to_string(variant);

  if (!cm.transform_inits || !cm.transform_inits->interp) {
    check(false, tag + ": the model has no init interpreter (" +
                     (cm.transform_inits ? cm.transform_inits->truncated
                                         : std::string("no section")) +
                     ")");
    return;
  }

  Executor ex(cm.graph);
  cm.bind(ex);
  const int64_t n = ex.n_params();
  const std::vector<double> q = free_point(n, variant);
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  ex.run_forward_only();

  // The constrained values, in the serial layout a user's init uses.
  const std::map<std::string, DataMap::Entry> constrained =
      cm.constrained_env(ex);

  std::vector<double> back;
  try {
    back = cm.transform_inits->interp->eval(constrained);
  } catch (const std::exception& e) {
    check(false, tag + ": unconstrain threw: " + e.what());
    return;
  }

  if ((int64_t)back.size() != n) {
    check(false, tag + ": got " + std::to_string(back.size()) +
                     " free values, want " + std::to_string(n));
    return;
  }
  for (int64_t i = 0; i < n; ++i) {
    if (std::abs(back[(size_t)i] - q[(size_t)i]) > 1e-9) {
      // Name the parameter the offset falls in; a bare index says nothing
      // about which permutation went wrong.
      std::string where = "?";
      int64_t at = 0;
      for (const auto& p : cm.unc_params) {
        if (i < at + p.len) {
          where = p.name + "[+" + std::to_string(i - at) + "]";
          break;
        }
        at += p.len;
      }
      ++failures;
      std::printf("FAIL %s: free %lld (%s) got %.17g want %.17g\n", tag.c_str(),
                  (long long)i, where.c_str(), back[(size_t)i], q[(size_t)i]);
      return;
    }
  }
}

// Every refusal names the parameter, because "index 4 out of bounds" would
// leave a user with no idea which starting value to fix.
void refusals(stanli::CompiledModel& cm) {
  using namespace stanli;
  if (!cm.transform_inits || !cm.transform_inits->interp) return;
  const InitInterp& interp = *cm.transform_inits->interp;

  Executor ex(cm.graph);
  cm.bind(ex);
  const int64_t n = ex.n_params();
  const std::vector<double> q = free_point(n, 0);
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  ex.run_forward_only();
  const std::map<std::string, DataMap::Entry> good = cm.constrained_env(ex);

  const auto refused = [&](const std::map<std::string, DataMap::Entry>& inits,
                           const std::string& needle, const std::string& what) {
    try {
      interp.eval(inits);
    } catch (const std::exception& e) {
      const std::string message = e.what();
      check(message.find(needle) != std::string::npos,
            what + ": message names " + needle + " (got: " + message + ")");
      return;
    }
    check(false, what + ": was accepted");
  };

  {
    auto missing = good;
    missing.erase("sigma");
    refused(missing, "sigma", "a missing parameter");
  }
  {
    auto extra = good;
    extra["not_a_parameter"] = good.at("mu");
    refused(extra, "not_a_parameter", "an unknown name");
  }
  {
    // Oversized is the case the interpreter cannot catch on its own: the
    // sequential read would take the first values and drop the rest.
    auto oversized = good;
    oversized["v"].r.push_back(1.0);
    refused(oversized, "v", "an oversized value");
  }
  {
    auto undersized = good;
    undersized["v"].r.pop_back();
    refused(undersized, "v", "an undersized value");
  }
  {
    auto below = good;
    below["sigma"].r[0] = -1.0;
    refused(below, "sigma", "a value below its lower bound");
  }
  {
    auto not_simplex = good;
    not_simplex["s"].r[0] += 0.25;
    refused(not_simplex, "s", "a simplex that does not sum to one");
  }
  {
    // The bound is another parameter, so this one is only detectable after
    // mu has been read.
    auto below_dep = good;
    below_dep["dep"].r[0] = good.at("mu").r[0] - 1.0;
    refused(below_dep, "dep", "a value below a parameter-dependent bound");
  }
}

void parameterless_model() {
  using namespace stanli;
  DataMap data;
  data.set_real_array("M", {1, 2, 3, 4, 5, 6}, {2, 3});
  CompiledModel cm = compile_model(
      slurp("tests/fixtures/view_gq_data_matrix.tmir.sexp"), data);
  check(cm.transform_inits.has_value(),
        "an explicitly empty init section remains available");
  if (!cm.transform_inits || !cm.transform_inits->interp) {
    check(false, "a parameterless model has an init interpreter");
    return;
  }
  check(cm.transform_inits->interp->eval({}).empty(),
        "a parameterless model unconstrains to an empty vector");
}

}  // namespace

int main() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/initrt.tmir.sexp");
  if (text.empty()) {
    std::printf("FAIL fixture missing (run from repo root)\n");
    return 1;
  }
  DataMap data = DataMap::from_json_file("tests/fixtures/initrt.json");
  CompiledModel cm = compile_model(text, data);

  check(cm.transform_inits.has_value(),
        "a backend-transformed MIR carries an init section");
  if (cm.transform_inits)
    check(cm.transform_inits->truncated.empty(),
          "the init section is usable: " + cm.transform_inits->truncated);

  round_trip(cm, 0);
  round_trip(cm, 1);
  refusals(cm);
  parameterless_model();

  return finish("test_init_interp");
}
