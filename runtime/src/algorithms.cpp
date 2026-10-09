#include <stanli/algorithms.hpp>

#include "service_model.hpp"

#include <stan/callbacks/structured_writer.hpp>
#include <stan/services/experimental/advi/fullrank.hpp>
#include <stan/services/experimental/advi/meanfield.hpp>
#include <stan/services/optimize/laplace_sample.hpp>
#include <stan/services/sample/fixed_param.hpp>

#include <cmath>

namespace stanli {

namespace {

bool refuse(AlgorithmResult& out, bool bad, const char* message) {
  if (!bad) return false;
  out.return_code = 1;
  out.message = message;
  return true;
}

bool bad_radius(double r) { return !std::isfinite(r) || r < 0; }

}  // namespace

AlgorithmResult run_fixed_param(Executor& ex, const AlgorithmHost& host,
                                const FixedParamConfig& cfg) {
  AlgorithmResult out;
  if (refuse(out, cfg.samples < 0, "samples must be nonnegative") ||
      refuse(out, cfg.thin < 1, "thin must be positive") ||
      refuse(out, bad_radius(cfg.init_radius),
             "init_radius must be finite and nonnegative"))
    return out;

  const detail::ServiceHooks hooks{&host.names, host.row, host.poll};
  detail::ServiceModel model(ex, hooks);
  detail::SinkLogger logger(host.log);
  detail::PollInterrupt interrupt(model);
  const auto init = detail::init_context(model, cfg.init);
  // lp__ and accept_stat__ lead each row; both are zero by definition.
  detail::RowCollector rows(2);
  stan::callbacks::writer init_writer;
  stan::callbacks::writer diagnostic_writer;

  detail::run_service(out, logger, "fixed_param", [&] {
    return stan::services::sample::fixed_param(
        model, *init, cfg.seed, (unsigned int)cfg.chain_id, cfg.init_radius,
        cfg.samples, cfg.thin, cfg.refresh, interrupt, logger, init_writer,
        rows, diagnostic_writer);
  });
  if (out.return_code != 0 || out.interrupted) return out;
  out.n_columns = (int64_t)model.n_columns();
  out.values = std::move(rows.values());
  return out;
}

AlgorithmResult run_laplace(Executor& ex, const AlgorithmHost& host,
                            const double* mode, const LaplaceConfig& cfg) {
  AlgorithmResult out;
  if (refuse(out, !cfg.jacobian,
             "laplace without the Jacobian is not available: stanli folds "
             "the Jacobian terms into the graph at lowering time. Pass "
             "jacobian = true with a mode found the same way.") ||
      refuse(out, mode == nullptr, "laplace needs a mode") ||
      refuse(out, cfg.draws < 1, "draws must be positive"))
    return out;

  const detail::ServiceHooks hooks{&host.names, host.row, host.poll};
  detail::ServiceModel model(ex, hooks);
  detail::SinkLogger logger(host.log);
  detail::PollInterrupt interrupt(model);
  detail::RowCollector rows(2);  // log_p__, log_q__
  stan::callbacks::structured_writer hessian_writer;
  const Eigen::VectorXd theta =
      Eigen::Map<const Eigen::VectorXd>(mode, (Eigen::Index)ex.n_params());

  detail::run_service(out, logger, "laplace", [&] {
    return stan::services::laplace_sample<true>(
        model, theta, cfg.draws, cfg.calculate_lp, cfg.seed, cfg.refresh,
        interrupt, logger, rows, hessian_writer);
  });
  if (out.return_code != 0 || out.interrupted) return out;
  out.n_columns = (int64_t)model.n_columns();
  out.lp.resize(rows.rows());
  out.lp_approx.resize(rows.rows());
  for (size_t i = 0; i < rows.rows(); ++i) {
    out.lp[i] = rows.lead(i, 0);
    out.lp_approx[i] = rows.lead(i, 1);
  }
  out.values = std::move(rows.values());
  return out;
}

AlgorithmResult run_variational(Executor& ex, const AlgorithmHost& host,
                                const VariationalConfig& cfg) {
  AlgorithmResult out;
  if (refuse(out, bad_radius(cfg.init_radius),
             "init_radius must be finite and nonnegative") ||
      refuse(out, cfg.adapt_engaged && cfg.adapt_iter < 1,
             "adapt_iter must be positive"))
    return out;

  const detail::ServiceHooks hooks{&host.names, host.row, host.poll};
  detail::ServiceModel model(ex, hooks);
  detail::SinkLogger logger(host.log);
  detail::PollInterrupt interrupt(model);
  const auto init = detail::init_context(model, cfg.init);
  detail::RowCollector rows(3);  // lp__ (always 0), log_p__, log_g__
  stan::callbacks::writer init_writer;
  stan::callbacks::writer diagnostic_writer;

  detail::run_service(out, logger, "variational", [&] {
    namespace advi = stan::services::experimental::advi;
    const auto run = [&](auto&& family) {
      return family(model, *init, cfg.seed, (unsigned int)cfg.chain_id,
                    cfg.init_radius, cfg.grad_samples, cfg.elbo_samples,
                    cfg.iter, cfg.tol_rel_obj, cfg.eta, cfg.adapt_engaged,
                    cfg.adapt_iter, cfg.eval_elbo, cfg.output_samples,
                    interrupt, logger, init_writer, rows, diagnostic_writer);
    };
    return cfg.fullrank ? run(advi::fullrank<detail::ServiceModel>)
                        : run(advi::meanfield<detail::ServiceModel>);
  });
  if (out.return_code != 0 || out.interrupted) return out;
  // Stan writes the mean of the approximation first, then the draws.
  if (rows.rows() < 1) {
    out.return_code = 1;
    out.message = "variational wrote no output";
    return out;
  }
  out.n_columns = (int64_t)model.n_columns();
  const size_t width = model.n_columns();
  const size_t draws = rows.rows() - 1;
  std::vector<double>& all = rows.values();
  out.mean.assign(all.begin(), all.begin() + (std::ptrdiff_t)width);
  out.values.assign(all.begin() + (std::ptrdiff_t)width, all.end());
  out.lp.resize(draws);
  out.lp_approx.resize(draws);
  for (size_t i = 0; i < draws; ++i) {
    out.lp[i] = rows.lead(i + 1, 1);
    out.lp_approx[i] = rows.lead(i + 1, 2);
  }
  return out;
}

}  // namespace stanli
