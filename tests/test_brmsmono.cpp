// brms's monotonic effects, mo().
//
// The model is here for one line of it:
//
//   return rows(scale) * sum(scale[1:i]);
//
// `rows` was answered only where an INTEGER was expected -- a size, a
// loop bound, a declaration dimension. brms writes it in the middle of
// real arithmetic, which reached "unsupported function rows" instead, so
// every model with an `mo()` term failed to compile. The corpus models
// sw_mono and s2_mo_simo_prior carry the same function and are replayed
// against CmdStan, which pins the folded constant. What stays here is
// what that replay cannot see: island activation and destination
// forwarding on this fixture, and repeated evaluation at one point.
#include <stanli/compile.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include "expect.hpp"
#include "env_helpers.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main() {
  using namespace stanli;

  DataMap data = DataMap::from_json_file("tests/fixtures/brmsmono.json");
  const std::string mir = slurp("tests/fixtures/brmsmono.tmir.sexp");
  CompiledModel cm = compile_model(mir, data);
  auto island_count = [](const Graph& g) {
    int n = 0;
    for (const Op& op : g.ops)
      if (op.opcode == OP_ISLAND) ++n;
    return n;
  };
  // Destination forwarding makes this marginal program look cheaper but the
  // resulting newly activated island measures slower than the graph. It may
  // optimize an explicitly forced island, but must not change the default
  // activation decision used to price the feature.
  expect_true("destination forwarding does not activate a marginal island",
              island_count(cm.graph) == 0);
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
  CompiledModel forced = compile_model(mir, data);
  test_setenv("STANLI_NO_PROGRAM_DEST_FORWARD", "1", 1);
  CompiledModel unforwarded = compile_model(mir, data);
  test_unsetenv("STANLI_NO_PROGRAM_DEST_FORWARD");
  test_unsetenv("STANLI_ISLAND_ALWAYS");
  expect_true("marginal island remains available when forced",
              island_count(forced.graph) == 1);
  auto island_instructions = [](const Graph& g) {
    size_t n = 0;
    for (const Op& op : g.ops)
      if (op.opcode == OP_ISLAND)
        n += static_cast<const IslandProg*>(op.udata)->code.size();
    return n;
  };
  expect_true("forced island still receives destination forwarding",
              island_instructions(forced.graph) <
                  island_instructions(unforwarded.graph));
  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  // Intercept, sigma, bsp_1, and simo_1 (a simplex[2], so 1 free).
  const int64_t n = ex.n_params();
  expect_true("4 unconstrained parameters, got " + std::to_string(n), n == 4);
  if (n != 4) return 1;

  std::vector<double> q((size_t)n), grad((size_t)n);
  for (int64_t i = 0; i < n; ++i) q[(size_t)i] = 0.2 - 0.13 * (double)i;
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  const double lp = ex.gradient(grad.data());
  expect_true("lp is finite", std::isfinite(lp));

  // Repeated evaluation must be idempotent. This is the check that
  // found the constant-folding bug behind this fixture: `mu` was folded
  // to a bind-time fill and the in-place element writes then accumulated
  // across evaluations, so the same point gave a different lp every
  // time. Nothing structural showed it, and the corpus rig cannot --
  // it evaluates one point per process.
  for (int64_t k = 0; k < n; ++k) ex.params_data()[k] = q[(size_t)k];
  const double again = ex.forward();
  expect_true("evaluating the same point twice gives the same lp", again == lp);
  // bsp_1 is the monotonic slope; if the mo() term were folded to zero
  // (the shape a wrong `rows` could take) its gradient would vanish.
  expect_true("the monotonic term reaches the target",
              std::fabs(grad[2]) > 1e-8);

  return finish("test_brmsmono");
}
