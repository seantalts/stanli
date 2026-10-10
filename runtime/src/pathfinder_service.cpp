// Single-path Pathfinder as an algorithm service: constrained rows written
// through the service's own generator, like the other entry points of
// algorithms.hpp. (pathfinder.cpp is the older single-path entry point with
// unconstrained draws and the L-BFGS path.)
//
// MULTI-PATH IS HELD. stan::services::pathfinder::pathfinder_lbfgs_multi in
// Stan 2.40.0 reads one element past the end of its array of resampled draw
// indices (stan/services/pathfinder/multi.hpp, line 348: the bound on j is
// checked once before a loop that increments j and reads coeff(j + 1)),
// whenever the last resampled draws are duplicates. AddressSanitizer reports
// it as a heap-buffer-overflow. Until Stan has a fix this file must not
// include multi.hpp or call that service; run_pathfinder_paths refuses more
// than one path instead.
#include <stanli/algorithms.hpp>

#include "service_model.hpp"

#include <stan/callbacks/structured_writer.hpp>
#include <stan/services/pathfinder/single.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <type_traits>

namespace stanli {

namespace {

// A diagnostic writer that records nothing, as the base class does, and can
// be called with any integer. The service writes std::size_t values, and
// structured_writer's integer overloads are fixed-width: where std::size_t
// is `unsigned long` and uint64_t is `unsigned long long` (macOS, wasm) no
// overload matches exactly and the call is ambiguous. pathfinder.cpp's
// PathCollector answers the same way.
class QuietDiagnostics : public stan::callbacks::structured_writer {
 public:
  using stan::callbacks::structured_writer::write;
  template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
  void write(const std::string&, T) {}
};

}  // namespace

int64_t pathfinder_max_draws(const PathfinderRunConfig& cfg) {
  return cfg.num_paths == 1 && cfg.num_draws >= 1 ? (int64_t)cfg.num_draws : 0;
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
      refuse(cfg.num_paths > 1,
             "multi-path Pathfinder is not available in this version: Stan "
             "2.40's multi-path service reads past the end of an array while "
             "resampling. Use num_paths = 1 for single-path Pathfinder.") ||
      refuse(cfg.num_draws < 1, "num_draws must be positive") ||
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

  const auto init = detail::init_context(model, cfg.inits);
  stan::callbacks::writer init_writer;
  QuietDiagnostics diagnostics;

  detail::run_service(out, logger, "pathfinder", [&] {
    return stan::services::pathfinder::pathfinder_lbfgs_single(
        model, *init, cfg.seed, (unsigned int)cfg.chain_id, cfg.init_radius,
        cfg.history_size, cfg.init_alpha, cfg.tol_obj, cfg.tol_rel_obj,
        cfg.tol_grad, cfg.tol_rel_grad, cfg.tol_param, cfg.max_lbfgs_iters,
        cfg.num_elbo_draws, cfg.num_draws, /*save_iterations=*/false,
        cfg.refresh, interrupt, logger, init_writer, rows, diagnostics,
        cfg.calculate_lp);
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
