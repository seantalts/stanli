#include <stanli/estimate.hpp>

#include <stan/callbacks/interrupt.hpp>
#include <stan/callbacks/logger.hpp>
#include <stan/callbacks/writer.hpp>
#include <stan/optimization/bfgs.hpp>
#include <stan/services/util/create_rng.hpp>

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace stanli {

namespace {

// Draw a starting point the way the sampler does, so `optimize` and
// `sample` disagree about where to start only when asked to. Rejects a
// draw whose log density or gradient is not finite, as CmdStan's
// initialize does.
std::vector<double> initial_point(Executor& ex, uint32_t seed, int chain_id,
                                  double radius, const double* init) {
  const int64_t n = ex.n_params();
  std::vector<double> q((size_t)n, 0.0);
  if (init != nullptr) {
    for (int64_t i = 0; i < n; ++i) q[(size_t)i] = init[i];
    return q;
  }
  if (radius == 0.0) return q;

  stan::rng_t rng = stan::services::util::create_rng(seed, chain_id);
  boost::random::uniform_real_distribution<double> dist(-radius, radius);
  std::vector<double> grad((size_t)n);
  for (int attempt = 0; attempt < 100; ++attempt) {
    for (int64_t i = 0; i < n; ++i) q[(size_t)i] = dist(rng);
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
    try {
      const double lp = ex.forward_value_only();
      if (!std::isfinite(lp)) continue;
      const double g = ex.gradient(grad.data());
      if (!std::isfinite(g)) continue;
      bool ok = true;
      for (int64_t i = 0; ok && i < n; ++i) ok = std::isfinite(grad[(size_t)i]);
      if (ok) return q;
    } catch (const std::exception&) {
    }
  }
  throw std::runtime_error(
      "initialization failed: no draw in 100 attempts had finite log density "
      "and gradient");
}

// run_optimize refuses the Jacobian-free form before it gets here, so only
// the Jacobian form is ever instantiated.
int run_lbfgs(ExecutorModel& model, std::vector<double>& cont,
              const OptimizeConfig& cfg, double* lp_out, std::string* msg) {
  using Optimizer =
      stan::optimization::BFGSLineSearch<ExecutorModel,
                                         stan::optimization::LBFGSUpdate<>,
                                         double, Eigen::Dynamic, true>;
  std::vector<int> disc;
  std::stringstream ss;
  Optimizer opt(model, cont, disc, &ss);
  opt.get_qnupdate().set_history_size(cfg.history_size);
  opt._ls_opts.alpha0 = cfg.init_alpha;
  opt._conv_opts.tolAbsF = cfg.tol_obj;
  opt._conv_opts.tolRelF = cfg.tol_rel_obj;
  opt._conv_opts.tolAbsGrad = cfg.tol_grad;
  opt._conv_opts.tolRelGrad = cfg.tol_rel_grad;
  opt._conv_opts.tolAbsX = cfg.tol_param;
  opt._conv_opts.maxIts = cfg.iter;

  int ret = 0;
  while (ret == 0) {
    ret = opt.step();
    // The optimizer's own message stream carries the line-search
    // diagnostics; keep the last of them for a failure report.
    if (!ss.str().empty()) {
      *msg = ss.str();
      ss.str("");
    }
  }
  opt.params_r(cont);
  // BFGSLineSearch::logp() already returns -(curr_f()), so it IS the log
  // density; negating it again would report the objective the optimizer
  // minimizes rather than the lp the caller asked for.
  *lp_out = opt.logp();
  // A positive code is a convergence condition (a satisfied tolerance);
  // only a negative one is a failure.
  return ret < 0 ? ret : 0;
}

}  // namespace

OptimizeResult run_optimize(Executor& ex, const WriteArray* wa,
                            const OptimizeConfig& cfg) {
  OptimizeResult out;
  if (!cfg.jacobian) {
    out.return_code = 1;
    out.message =
        "optimize without the Jacobian (CmdStan's default, the penalized "
        "maximum likelihood) is not available: stanli folds the Jacobian "
        "terms into the graph at lowering time. Pass jacobian = true for "
        "the posterior mode.";
    return out;
  }
  ExecutorModel model(ex, wa);
  std::vector<double> cont =
      initial_point(ex, cfg.seed, cfg.chain_id, cfg.init_radius, cfg.init);

  out.return_code = run_lbfgs(model, cont, cfg, &out.lp, &out.message);
  out.unconstrained = cont;
  if (wa != nullptr) {
    out.names = wa->names;
    out.values.assign(wa->names.size(), 0.0);
    wa->row(cont.data(), out.values.data());
  }
  return out;
}

}  // namespace stanli
