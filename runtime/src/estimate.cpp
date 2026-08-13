#include <stanli/estimate.hpp>

#include "initialize.hpp"

#include <stan/optimization/bfgs.hpp>
#include <stan/services/util/create_rng.hpp>

#include <sstream>

namespace stanli {

namespace {

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
  stan::rng_t rng = stan::services::util::create_rng(cfg.seed, cfg.chain_id);
  std::vector<double> cont((size_t)ex.n_params());
  initialize_point(ex, rng, cfg.init_radius, cfg.init, cont.data(),
                   FixedInitPolicy::SkipValidation);

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
