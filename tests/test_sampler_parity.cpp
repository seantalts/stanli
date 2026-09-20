// Isolate sampler configuration from compiler numerics: run the same executor
// through our driver and the actual Stan service used by CmdStan. No random
// generated quantities here; their stream scheduling is a separate contract.
#include "models.hpp"

#include <stanli/model_adapter.hpp>
#include <stanli/nuts.hpp>

#include <stan/io/empty_var_context.hpp>
#include <stan/services/sample/hmc_nuts_diag_e_adapt.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
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
  const size_t expected = (cfg.samples + cfg.thin - 1) / cfg.thin
                         + (cfg.save_warmup
                                ? (cfg.warmup + cfg.thin - 1) / cfg.thin
                                : 0);
  if (status != 0 || draws.size() != expected || stats.rows.size() != expected
      || writer.rows.size() != expected) {
    std::printf("FAIL %s case %d: status %d, rows %zu/%zu, expected %zu\n",
                name, scenario, status, draws.size(), writer.rows.size(),
                expected);
    return false;
  }
  for (size_t r = 0; r < expected; ++r) {
    std::vector<double> row(stats.rows[r].begin(), stats.rows[r].end());
    row.insert(row.end(), draws[r].begin(), draws[r].end());
    if (row.size() != writer.rows[r].size()) {
      std::printf("FAIL %s case %d row %zu: width %zu != %zu\n", name,
                  scenario, r, row.size(), writer.rows[r].size());
      return false;
    }
    for (size_t c = 0; c < row.size(); ++c) {
      const double want = writer.rows[r][c];
      if (!std::isfinite(row[c]) || !std::isfinite(want)
          || std::memcmp(&row[c], &want, sizeof(double)) != 0) {
        std::printf("FAIL %s case %d row %zu column %zu: %.17g != %.17g\n",
                    name, scenario, r, c, row[c], want);
        return false;
      }
    }
  }
  std::printf("PASS %s case %d: %zu rows bitwise equal to Stan service\n",
              name, scenario, expected);
  return true;
}

}  // namespace

int main() {
  int failures = 0;
  for (bool schools : {false, true})
    for (int scenario = 0; scenario < 4; ++scenario)
      if (!compare(schools, scenario)) ++failures;
  return failures != 0;
}
