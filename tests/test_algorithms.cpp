// The algorithm services against Stan's own generator.
//
// tests/test_capi_algorithms.cpp checks distributions. This checks streams:
// that a seed names the run it names in Stan, which is the reason the
// algorithms write their rows through the service's generator instead of
// constraining draws afterwards. Each expectation here is computed with
// Stan Math's rng functions on stan::services::util::create_rng, with no
// stanli code between, so a wrong generator, a wrong chain id, a draw taken
// out of order or a generator copied instead of advanced all show up as
// different numbers rather than as a slightly different distribution.
#include <stanli/algorithms.hpp>
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>
#include <stan/services/util/create_rng.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// fixed_param on a model with no parameters. Initialization then draws
// nothing, so the service's stream is spent on generated quantities alone:
// y = normal_rng(5, 2) and k = poisson_rng(3), row after row, from
// create_rng(seed, chain).
static void test_fixed_param_stream() {
  using namespace stanli;
  DataMap data = DataMap::from_json(slurp("tests/fixtures/alg_noparam.json"));
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/alg_noparam.tmir.sexp"), data);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  expect("noparam has no parameters", ex.n_params() == 0);
  if (!cm.write_array || cm.write_array->interp ||
      cm.write_array->columns.empty()) {
    ++failures;
    std::printf("FAIL noparam: expected a compiled write_array graph\n");
    return;
  }
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  const auto columns = cm.write_array->columns;

  AlgorithmHost host;
  host.names = CompiledModel::csv_names(columns);
  host.row = [&](const double* /*q*/, double* out, WaRng& rng) {
    wex.run_forward_only(EvalState{&rng});
    int64_t at = 0;
    for (const auto& c : columns) {
      const double* p = std::as_const(wex).value_ptr(c.slot);
      for (int64_t i = 0; i < c.len; ++i) out[at++] = p[i];
    }
  };
  expect("noparam columns", host.names.size() == 2);

  for (int chain : {1, 3}) {
    FixedParamConfig cfg;
    cfg.seed = 20261009;
    cfg.chain_id = chain;
    cfg.samples = 50;
    const AlgorithmResult r = run_fixed_param(ex, host, cfg);
    expect("noparam fixed_param runs: " + r.message, r.return_code == 0);
    expect("noparam fixed_param rows", r.rows() == 50 && r.n_columns == 2);
    if (r.rows() != 50 || r.n_columns != 2) return;
    stan::rng_t rng = stan::services::util::create_rng(cfg.seed, chain);
    bool same = true;
    for (int i = 0; i < 50; ++i) {
      const double y = stan::math::normal_rng(5.0, 2.0, rng);
      const int k = stan::math::poisson_rng(3.0, rng);
      same &= r.values[(size_t)(2 * i)] == y &&
              r.values[(size_t)(2 * i + 1)] == (double)k;
    }
    expect("fixed_param draws Stan's stream for chain " + std::to_string(chain),
           same);
  }
}

// Laplace on a standard normal in D dimensions with the mode at the origin.
// The service draws D standard normals per row from create_rng(seed, 0) and
// maps them through the inverse Cholesky factor of the negative Hessian,
// which here is the identity up to finite-difference error. So row m is the
// m-th block of that stream, and log_q__ is minus half its squared norm.
static void test_laplace_stream() {
  using namespace stanli;
  const int D = 3;
  Graph g;
  const int x = g.add_slot(D, true);
  const int zero = g.add_slot(1, false);
  const int one = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {x, zero, one}, lp);
  g.result_slot = lp;
  Executor ex(std::move(g));
  ex.value_ptr(zero)[0] = 0.0;
  ex.value_ptr(one)[0] = 1.0;

  const AlgorithmHost host;  // no row: the columns are the parameters
  LaplaceConfig cfg;
  cfg.seed = 4242;
  cfg.draws = 200;
  const std::vector<double> mode(D, 0.0);
  const AlgorithmResult r = run_laplace(ex, host, mode.data(), cfg);
  expect("laplace runs: " + r.message, r.return_code == 0);
  expect("laplace rows", r.rows() == cfg.draws && r.n_columns == D);
  if (r.rows() != cfg.draws || r.n_columns != D) return;

  stan::rng_t rng = stan::services::util::create_rng(cfg.seed, 0);
  double worst = 0, worst_q = 0, p_lo = 0, p_hi = 0;
  for (int m = 0; m < cfg.draws; ++m) {
    double norm2 = 0;
    for (int d = 0; d < D; ++d) {
      const double z = stan::math::std_normal_rng(rng);
      norm2 += z * z;
      worst = std::fmax(worst, std::fabs(r.values[(size_t)(m * D + d)] - z));
    }
    worst_q = std::fmax(worst_q, std::fabs(r.lp_approx[(size_t)m] + norm2 / 2));
    // The model's density at the draw is the same quadratic plus whatever
    // constant the graph's normal carries.
    const double offset = r.lp[(size_t)m] + norm2 / 2;
    p_lo = m == 0 ? offset : std::fmin(p_lo, offset);
    p_hi = m == 0 ? offset : std::fmax(p_hi, offset);
  }
  // Finite-difference Hessian: its error scales the draws, nothing else.
  expect(
      "laplace draws are Stan's stream (worst " + std::to_string(worst) + ")",
      worst < 1e-6);
  expect("laplace log_q (worst " + std::to_string(worst_q) + ")",
         worst_q < 1e-6);
  expect("laplace log_p (spread " + std::to_string(p_hi - p_lo) + ")",
         p_hi - p_lo < 1e-6);
}

// A row that is rejected keeps the randomness it consumed, and fixed_param
// writes NaN for it and carries on, as CmdStan does.
static void test_rejected_row() {
  using namespace stanli;
  Graph g;
  const int x = g.add_slot(1, true);
  const int zero = g.add_slot(1, false);
  const int one = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {x, zero, one}, lp);
  g.result_slot = lp;
  Executor ex(std::move(g));
  ex.value_ptr(zero)[0] = 0.0;
  ex.value_ptr(one)[0] = 1.0;

  AlgorithmHost host;
  host.names = {"x", "u"};
  int calls = 0;
  host.row = [&](const double* q, double* out, WaRng& rng) {
    out[0] = q[0];
    out[1] = stan::math::uniform_rng(0.0, 1.0, rng.gen());
    if (++calls == 2) throw std::domain_error("rejected on purpose");
  };
  FixedParamConfig cfg;
  cfg.seed = 7;
  cfg.samples = 3;
  const double init = 0.25;
  cfg.init = &init;
  const AlgorithmResult r = run_fixed_param(ex, host, cfg);
  expect("rejected row: run succeeds: " + r.message, r.return_code == 0);
  expect("rejected row: three rows", r.rows() == 3);
  if (r.rows() != 3) return;
  expect("rejected row is NaN",
         std::isnan(r.values[2]) && std::isnan(r.values[3]));
  expect("rows around it are kept", r.values[0] == 0.25 && r.values[4] == 0.25);
  // Row 3's uniform is the THIRD draw of its stream, not the second: had the
  // rejected row's draw been rolled back, rows 1 and 3 would be consecutive
  // draws of one stream and a rerun that rejects nothing would reproduce
  // row 3 as its row 2.
  calls = -100;  // never reject
  const AlgorithmResult clean = run_fixed_param(ex, host, cfg);
  expect("clean rerun", clean.return_code == 0 && clean.rows() == 3);
  if (clean.rows() != 3) return;
  expect("the rejected row consumed its draw",
         r.values[1] == clean.values[1] && r.values[5] == clean.values[5] &&
             clean.values[3] != clean.values[5]);
}

int main() {
  test_fixed_param_stream();
  test_laplace_stream();
  test_rejected_row();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("all algorithm stream tests passed\n");
  return 0;
}
