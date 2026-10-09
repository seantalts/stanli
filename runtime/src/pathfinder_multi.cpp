// Must stay first: see the header.
#include "tbb_serial.hpp"

#include <stanli/algorithms.hpp>

#include "service_model.hpp"

#include <stan/callbacks/structured_writer.hpp>
#include <stan/services/pathfinder/multi.hpp>
#include <stan/services/pathfinder/single.hpp>

#include <cmath>
#include <memory>

namespace stanli {

int64_t pathfinder_max_draws(const PathfinderRunConfig& cfg) {
  if (cfg.num_paths < 1 || cfg.num_draws < 1) return 0;
  const int64_t all = (int64_t)cfg.num_paths * cfg.num_draws;
  const bool resampled =
      cfg.num_paths > 1 && cfg.psis_resample && cfg.calculate_lp;
  return resampled ? (int64_t)(cfg.num_psis_draws > 0 ? cfg.num_psis_draws : 0)
                   : all;
}

AlgorithmResult run_pathfinder_paths(Executor& ex, const AlgorithmHost& host,
                                     const PathfinderRunConfig& cfg) {
  AlgorithmResult out;
  const auto refuse = [&out](bool bad, const char* message) {
    if (!bad) return false;
    out.return_code = 1;
    out.message = message;
    return true;
  };
  if (refuse(cfg.num_paths < 1, "num_paths must be positive") ||
      refuse(cfg.num_draws < 1, "num_draws must be positive") ||
      refuse(cfg.num_psis_draws < 1, "num_psis_draws must be positive") ||
      refuse(cfg.num_elbo_draws < 1, "num_elbo_draws must be positive") ||
      refuse(cfg.max_lbfgs_iters < 1, "max_lbfgs_iters must be positive") ||
      refuse(cfg.history_size < 1, "history_size must be positive") ||
      refuse(!std::isfinite(cfg.init_radius) || cfg.init_radius < 0,
             "init_radius must be finite and nonnegative"))
    return out;

  const detail::ServiceHooks hooks{&host.names, host.row, host.poll};
  detail::ServiceModel model(ex, hooks);
  detail::SinkLogger logger(host.log);
  detail::PollInterrupt interrupt(model);
  detail::RowCollector rows(3);  // lp_approx__, lp__, path__

  const size_t paths = (size_t)cfg.num_paths;
  const int64_t n = ex.n_params();
  std::vector<std::unique_ptr<stan::io::var_context>> inits;
  for (size_t p = 0; p < paths; ++p)
    inits.push_back(detail::init_context(
        model, cfg.inits ? cfg.inits + (int64_t)p * n : nullptr));
  std::vector<stan::callbacks::writer> init_writers(paths);
  // Base writers are invalid, which is how the service knows not to write
  // each path's own draws as well.
  std::vector<stan::callbacks::writer> path_writers(paths);
  std::vector<stan::callbacks::structured_writer> path_diagnostics(paths);
  stan::callbacks::structured_writer diagnostics;

  detail::run_service(out, logger, "pathfinder", [&] {
    namespace pf = stan::services::pathfinder;
    // CmdStan's own choice: one path is the single-path service, with no
    // resampling step.
    if (cfg.num_paths == 1)
      return pf::pathfinder_lbfgs_single(
          model, *inits[0], cfg.seed, (unsigned int)cfg.chain_id,
          cfg.init_radius, cfg.history_size, cfg.init_alpha, cfg.tol_obj,
          cfg.tol_rel_obj, cfg.tol_grad, cfg.tol_rel_grad, cfg.tol_param,
          cfg.max_lbfgs_iters, cfg.num_elbo_draws, cfg.num_draws,
          /*save_iterations=*/false, cfg.refresh, interrupt, logger,
          init_writers[0], rows, diagnostics, cfg.calculate_lp);
    return pf::pathfinder_lbfgs_multi(
        model, inits, cfg.seed, (unsigned int)cfg.chain_id, cfg.init_radius,
        cfg.history_size, cfg.init_alpha, cfg.tol_obj, cfg.tol_rel_obj,
        cfg.tol_grad, cfg.tol_rel_grad, cfg.tol_param, cfg.max_lbfgs_iters,
        cfg.num_elbo_draws, cfg.num_draws, cfg.num_psis_draws, cfg.num_paths,
        /*save_iterations=*/false, cfg.refresh, interrupt, logger, init_writers,
        path_writers, path_diagnostics, rows, diagnostics, cfg.calculate_lp,
        cfg.psis_resample);
  });
  if (out.return_code != 0 || out.interrupted) return out;
  out.n_columns = (int64_t)model.n_columns();
  out.lp.resize(rows.rows());
  out.lp_approx.resize(rows.rows());
  out.path.resize(rows.rows());
  for (size_t i = 0; i < rows.rows(); ++i) {
    out.lp_approx[i] = rows.lead(i, 0);
    out.lp[i] = rows.lead(i, 1);
    out.path[i] = rows.lead(i, 2);
  }
  out.values = std::move(rows.values());
  return out;
}

}  // namespace stanli
