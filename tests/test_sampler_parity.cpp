// Isolate sampler configuration from compiler numerics: run the same executor
// through our driver and the actual Stan service used by CmdStan, including
// the live RNG consumed by generated quantities between saved transitions.
#include "models.hpp"
#include "env_helpers.hpp"

#include <stanli/compile.hpp>
#include <stanli/model_adapter.hpp>
#include <stanli/nuts.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/io/empty_var_context.hpp>
#include <stan/services/sample/hmc_nuts_diag_e_adapt.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace {

struct Rows : stan::callbacks::writer {
  std::vector<std::vector<double>> rows;
  void operator()(const std::vector<double>& row) override {
    rows.push_back(row);
  }
};

std::unique_ptr<stanli::Executor> make_executor(bool schools) {
  using namespace stanli;
  if (schools) {
    auto m = testmodels::eight_schools();
    auto ex = std::make_unique<Executor>(std::move(m.graph));
    testmodels::fill_eight_schools_data(m, *ex);
    return ex;
  }
  Graph g;
  const int x = g.add_slot(4, true);
  const int zero = g.add_slot(1, false);
  const int one = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {x, zero, one}, lp);
  g.result_slot = lp;
  auto ex = std::make_unique<Executor>(std::move(g));
  ex->value_ptr(zero)[0] = 0.0;
  ex->value_ptr(one)[0] = 1.0;
  return ex;
}

bool compare(bool schools, int scenario) {
  stanli::NutsConfig cfg;
  cfg.seed = 17 + scenario;
  cfg.chain_id = 1 + scenario;
  cfg.samples = 30;
  cfg.warmup = scenario == 0 ? 0 : scenario == 1 ? 30 : 200;
  cfg.save_warmup = scenario == 1 || scenario == 2;
  cfg.thin = scenario == 2 ? 2 : scenario == 3 ? 3 : 1;
  cfg.init_radius = scenario == 3 ? 0.0 : 2.0;
  cfg.delta = scenario == 2 ? 0.9 : 0.8;
  cfg.max_depth = scenario == 1 ? 5 : 8;

  auto ours = make_executor(schools);
  auto reference = make_executor(schools);
  stanli::SamplerStats stats;
  const auto draws = stanli::run_nuts(*ours, cfg, &stats);

  stanli::ExecutorModel model(*reference);
  stan::io::empty_var_context init;
  stan::callbacks::interrupt interrupt;
  stan::callbacks::logger logger;
  stan::callbacks::writer init_writer, diagnostic_writer;
  Rows writer;
  const int status = stan::services::sample::hmc_nuts_diag_e_adapt(
      model, init, cfg.seed, cfg.chain_id, cfg.init_radius, cfg.warmup,
      cfg.samples, cfg.thin, cfg.save_warmup, 0, 1.0, 0.0, cfg.max_depth,
      cfg.delta, 0.05, 0.75, 10.0, 75, 50, 25, interrupt, logger, init_writer,
      writer, diagnostic_writer);
  const char* name = schools ? "eight-schools" : "normal";
  const size_t expected =
      (cfg.samples + cfg.thin - 1) / cfg.thin +
      (cfg.save_warmup ? (cfg.warmup + cfg.thin - 1) / cfg.thin : 0);
  if (status != 0 || draws.size() != expected ||
      stats.rows.size() != expected || writer.rows.size() != expected) {
    std::printf("FAIL %s case %d: status %d, rows %zu/%zu, expected %zu\n",
                name, scenario, status, draws.size(), writer.rows.size(),
                expected);
    return false;
  }
  for (size_t r = 0; r < expected; ++r) {
    std::vector<double> row(stats.rows[r].begin(), stats.rows[r].end());
    row.insert(row.end(), draws[r].begin(), draws[r].end());
    if (row.size() != writer.rows[r].size()) {
      std::printf("FAIL %s case %d row %zu: width %zu != %zu\n", name, scenario,
                  r, row.size(), writer.rows[r].size());
      return false;
    }
    for (size_t c = 0; c < row.size(); ++c) {
      const double want = writer.rows[r][c];
      if (!std::isfinite(row[c]) || !std::isfinite(want) ||
          std::memcmp(&row[c], &want, sizeof(double)) != 0) {
        std::printf("FAIL %s case %d row %zu column %zu: %.17g != %.17g\n",
                    name, scenario, r, c, row[c], want);
        return false;
      }
    }
  }
  std::printf("PASS %s case %d: %zu rows bitwise equal to Stan service\n", name,
              scenario, expected);
  return true;
}

// An independent write_array implementation for the Stan fixtures below.
// Keep this test adapter separate from the production optimizer callback,
// which has no reason to participate in a sampler's RNG schedule.
struct RngModel : stanli::ExecutorModel {
  bool vector;
  bool reject;
  std::vector<std::string> names;
  RngModel(stanli::Executor& ex, bool v, bool r, std::vector<std::string> n)
      : ExecutorModel(ex), vector(v), reject(r), names(std::move(n)) {}

  void constrained_param_names(std::vector<std::string>& out, bool = true,
                               bool gq = true) const {
    out.insert(out.end(), names.begin(), gq ? names.end() : names.begin() + 1);
  }

  template <typename RNG>
  void write_array(RNG& rng, std::vector<double>& q, std::vector<int>&,
                   std::vector<double>& out, bool = true, bool gq = true,
                   std::ostream* = nullptr) const {
    using namespace stan::math;
    out = q;
    if (!gq) return;
    if (vector) {
      out.push_back(normal_rng(q[0], 1.0, rng));
      const auto batch =
          normal_rng(Eigen::VectorXd::Constant(3, q[0]), 0.7, rng);
      out.insert(out.end(), batch.begin(), batch.end());
      if (reject && q[0] < 0)
        throw std::domain_error("reject after random draws");
      out.push_back(normal_rng(q[0], 1.0, rng));
    } else {
      out.push_back(poisson_log_rng(0.3, rng));
      out.push_back(uniform_rng(-2.0, 3.0, rng));
      out.push_back(bernoulli_rng(0.4, rng));
      out.push_back(normal_rng(q[0], 1.2, rng));
      out.push_back(lognormal_rng(0.2, 0.7, rng));
      out.push_back(2 + binomial_rng(5, inv_logit(q[0]), rng));
      out.push_back(gumbel_rng(0.5, 1.3, rng));
      out.push_back(exponential_rng(1.6, rng));
    }
  }
};

bool compare_rng(bool interpreted, int mode, int scenario) {
  using namespace stanli;
  const bool vector = mode != 0;
  const bool reject = mode == 2;
  const char* fixture = reject   ? "gq_rng_stream_reject"
                        : vector ? "gq_rng_stream"
                                 : "gq_scalar_rng";
  std::ifstream f(std::string("tests/fixtures/") + fixture + ".tmir.sexp");
  std::ostringstream source;
  source << f.rdbuf();
  if (interpreted)
    test_setenv("STANLI_WA_FORCE_INTERP", "1");
  else
    test_unsetenv("STANLI_WA_FORCE_INTERP");
  auto cm = compile_model(source.str(), DataMap{});
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  Executor ours(std::move(cm.graph));
  cm.bind(ours);
  Executor reference(ours);
  auto wi = cm.write_array->interp;
  std::unique_ptr<Executor> wa;
  if (!wi) {
    wa = std::make_unique<Executor>(std::move(cm.write_array->graph));
    cm.write_array->bind(*wa);
  }
  auto evaluate = [&](const double* q, WaRng& rng) {
    if (wi) {
      ours.params_data()[0] = q[0];
      ours.run_forward_only();
      return wi->eval(cm.constrained_env(ours), rng);
    }
    wa->params_data()[0] = q[0];
    wa->run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& col : cm.write_array->columns)
      for (int64_t i = 0; i < col.len; ++i)
        row.push_back(
            std::as_const(*wa).value_ptr(col.slot)[col.storage_index(i)]);
    return row;
  };
  // A rejected first discovery must not duplicate the columns on retry.
  WaRng probe(123);
  if (wi && reject) {
    const double bad = -1.0;
    try {
      evaluate(&bad, probe);
      return false;
    } catch (const std::domain_error&) {
    }
  }
  const double good = 1.0;
  evaluate(&good, probe);
  const auto names =
      CompiledModel::csv_names(wi ? wi->columns() : cm.write_array->columns);
  if (names.size() != (vector ? 6u : 9u) || (!!wi != interpreted && !reject)) {
    std::printf("FAIL RNG backend/column discovery %s interp=%d width=%zu\n",
                fixture, interpreted, names.size());
    return false;
  }
  NutsConfig cfg;
  cfg.seed = 17 + scenario;
  cfg.chain_id = 1 + scenario;
  cfg.warmup = scenario == 0 ? 0 : scenario == 1 ? 30 : 200;
  cfg.samples = 80;
  cfg.thin = scenario == 1 ? 2 : scenario >= 2 ? 3 : 1;
  cfg.save_warmup = scenario == 1 || scenario == 2;
  cfg.init_radius = scenario == 2 ? 0.0 : 2.0;
  cfg.max_depth = 5;
  cfg.retain_draws = scenario != 2;
  std::vector<std::vector<double>> values;
  std::vector<std::vector<double>> streamed_draws;
  std::vector<bool> rejected;
  cfg.on_stored = [&](int64_t index, const double* q, WaRng& rng,
                      const SamplerRow&) {
    if (index != (int64_t)streamed_draws.size())
      throw std::runtime_error("stored index skipped a row");
    streamed_draws.emplace_back(q, q + ours.n_params());
    try {
      values.push_back(evaluate(q, rng));
      rejected.push_back(false);
    } catch (const std::domain_error&) {
      values.emplace_back(names.size(),
                          std::numeric_limits<double>::quiet_NaN());
      rejected.push_back(true);
    }
  };
  SamplerStats stats;
  const auto retained = run_nuts(ours, cfg, &stats);
  if (cfg.retain_draws ? retained != streamed_draws : !retained.empty())
    return false;
  const auto& draws = streamed_draws;
  RngModel model(reference, vector, reject, names);
  stan::io::empty_var_context init;
  stan::callbacks::interrupt interrupt;
  stan::callbacks::logger logger;
  stan::callbacks::writer init_writer, diagnostic_writer;
  Rows writer;
  const int status = stan::services::sample::hmc_nuts_diag_e_adapt(
      model, init, cfg.seed, cfg.chain_id, cfg.init_radius, cfg.warmup,
      cfg.samples, cfg.thin, cfg.save_warmup, 0, 1.0, 0.0, cfg.max_depth,
      cfg.delta, 0.05, 0.75, 10.0, 75, 50, 25, interrupt, logger, init_writer,
      writer, diagnostic_writer);
  if (status || draws.size() != writer.rows.size() || draws.empty())
    return false;
  size_t failures = 0, successes = 0;
  for (size_t r = 0; r < draws.size(); ++r) {
    std::vector<double> row(stats.rows[r].begin(), stats.rows[r].end());
    row.insert(row.end(), values[r].begin(), values[r].end());
    // Stan retains the prefix of a rejected write_array. Stanli writes the
    // whole output row as NaN; the sampler state and next draws must agree.
    row[7] = draws[r][0];
    const size_t width = rejected[r] ? 8 : row.size();
    failures += rejected[r];
    successes += !rejected[r];
    if (writer.rows[r].size() != row.size() ||
        std::isnan(writer.rows[r].back()) != rejected[r])
      return false;
    for (size_t c = 0; c < width; ++c)
      if (std::memcmp(&row[c], &writer.rows[r][c], sizeof(double))) {
        std::printf(
            "FAIL RNG %s interp=%d case=%d row=%zu col=%zu: %.17g != %.17g\n",
            fixture, interpreted, scenario, r, c, row[c], writer.rows[r][c]);
        return false;
      }
  }
  if (!successes || (reject && !failures)) return false;
  std::printf(
      "PASS RNG %s backend=%s forced_interp=%d case=%d: %zu rows bitwise, %zu "
      "rejections\n",
      fixture, wi ? "interpreter" : "graph", interpreted, scenario,
      draws.size(), failures);
  return true;
}

}  // namespace

int main() {
  int failures = 0;
  for (bool schools : {false, true})
    for (int scenario = 0; scenario < 4; ++scenario)
      if (!compare(schools, scenario)) ++failures;
  for (bool interpreted : {false, true})
    for (int mode = 0; mode < 3; ++mode)
      for (int scenario = 0; scenario < 4; ++scenario)
        if (!compare_rng(interpreted, mode, scenario)) ++failures;
  return failures != 0;
}
