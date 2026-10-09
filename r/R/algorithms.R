# Stan's inference algorithms other than NUTS: fixed-parameter sampling,
# Laplace sampling, Pathfinder, and ADVI. Each runs Stan's own service in the
# runtime and comes back as a `stanli_fit`, so the draws interface and the
# conversions are the ones a NUTS fit has.

# The algorithm a fit came from. Fits made before the others existed carry no
# field and are NUTS.
fit_algorithm <- function(fit) {
  if (is.null(fit$algorithm)) "sampling" else fit$algorithm
}

# For what only a NUTS fit can answer.
require_nuts_fit <- function(fit, what) {
  algorithm <- fit_algorithm(fit)
  if (algorithm != "sampling")
    stop(what, " needs a NUTS fit; this one is from algorithm '", algorithm,
         "'", call. = FALSE)
  invisible(fit)
}

algorithm_count <- function(x, name, minimum = 1L) {
  if (length(x) != 1L || !is.numeric(x) || is.logical(x) || is.na(x) ||
      !is.finite(x) || x < minimum || x != floor(x) ||
      x > .Machine$integer.max)
    stop(name, " must be a single integer of at least ", minimum, call. = FALSE)
  as.integer(x)
}

algorithm_number <- function(x, name, positive = TRUE) {
  if (length(x) != 1L || !is.numeric(x) || is.logical(x) || is.na(x) ||
      !is.finite(x) || (positive && x <= 0) || (!positive && x < 0))
    stop(name, " must be a single ", if (positive) "positive" else "nonnegative",
         " number", call. = FALSE)
  as.double(x)
}

# Starting points as the runtime reads them: `count` rows of unconstrained
# values, row-major, or nothing for random starts.
algorithm_inits <- function(model, init, count) {
  if (is.null(init)) return(numeric(0))
  n <- as.integer(model$n_unconstrained)
  m <- if (is.matrix(init)) init else
    matrix(rep(as.double(init), count), nrow = count, byrow = TRUE)
  if (!is.numeric(m) || !identical(dim(m), c(as.integer(count), n)))
    stop("init must be a length-", n, " vector",
         if (count > 1L) paste0(" or a ", count, " x ", n, " matrix") else "",
         " on the unconstrained scale", call. = FALSE)
  as.double(t(m))
}

# A stopped run returns no fit: raise the interrupt the user sent.
algorithm_interrupt <- function(res, what) {
  if (!isTRUE(res$interrupted)) return(invisible(NULL))
  signalCondition(structure(class = c("interrupt", "condition"),
                            list(message = paste(what, "interrupted"),
                                 call = NULL)))
  invokeRestart("abort")
}

# What Stan said that is worth more than a console line.
algorithm_notes <- function(res) {
  if (is.character(res$notes) && nzchar(res$notes))
    warning(res$notes, call. = FALSE)
  invisible(NULL)
}

# One `stanli_fit` for every algorithm that returns independent draws from an
# approximation: one chain, no warmup, and the per-draw log densities where a
# NUTS fit has its sampler columns.
approximation_fit <- function(model, algorithm, seed, values, diagnostics,
                              elapsed, extra = list()) {
  ncol <- length(model$columns)
  ndraw <- if (ncol > 0L) length(values) %/% ncol else
    length(diagnostics[[1L]])
  # C fills rows column-fastest; see sample_model().
  arr <- aperm(array(values, dim = c(ncol, ndraw, 1L)), c(2, 3, 1))
  dimnames(arr) <- list(NULL, NULL, model$columns)
  sarr <- array(unlist(diagnostics, use.names = FALSE),
                dim = c(ndraw, 1L, length(diagnostics)),
                dimnames = list(NULL, NULL, names(diagnostics)))
  fit <- list(draws = arr, sampler = sarr,
              unconstrained = array(numeric(0),
                                    dim = c(0L, 1L, model$n_unconstrained)),
              columns = model$columns, seed = seed, model = model,
              algorithm = algorithm,
              # the wall time of the whole run; there is no warmup
              report = list(available = TRUE, warmup_seconds = 0,
                            sampling_seconds = elapsed),
              warmup_draws = 0L, warmup = 0L, thin = 1L,
              samples = as.integer(ndraw), save_warmup = FALSE, chains = 1L)
  structure(c(fit, extra), class = "stanli_fit")
}

# sample_model(fixed_param = TRUE).
fixed_param_fit <- function(model, chains, seed, samples, thin, init,
                            init_radius, refresh) {
  chains <- algorithm_count(chains, "chains")
  samples <- algorithm_count(samples, "samples")
  thin <- algorithm_count(thin, "thin")
  opts <- list(as.integer(seed), chains, samples, thin,
               algorithm_number(init_radius, "init_radius", positive = FALSE),
               as.integer(refresh))
  res <- .Call("stanli_r_fixed_param", model$ptr, opts,
               algorithm_inits(model, init, chains))
  algorithm_interrupt(res, "sampling")
  algorithm_notes(res)
  ncol <- length(model$columns)
  arr <- aperm(array(res$values, dim = c(ncol, res$draws, res$chains)),
               c(2, 3, 1))
  dimnames(arr) <- list(NULL, NULL, model$columns)
  # CmdStan's two columns for this sampler, both zero by definition.
  sarr <- array(0, dim = c(res$draws, res$chains, 2L),
                dimnames = list(NULL, NULL, c("lp__", "accept_stat__")))
  structure(list(draws = arr, sampler = sarr,
                 unconstrained = array(numeric(0),
                                       dim = c(0L, res$chains,
                                               model$n_unconstrained)),
                 columns = model$columns, seed = seed, model = model,
                 algorithm = "fixed_param", report = list(available = FALSE),
                 warmup_draws = 0L, warmup = 0L, thin = thin,
                 samples = samples, save_warmup = FALSE,
                 chains = as.integer(res$chains)),
            class = "stanli_fit")
}

#' Variational inference with ADVI
#'
#' Fits a normal approximation on the unconstrained scale by stochastic
#' gradient ascent on the evidence lower bound, then draws from it. This is
#' Stan's own ADVI, the algorithm behind CmdStan's `variational` method and
#' `rstan::vb()`, with their defaults.
#'
#' @param model A `stanli_model`.
#' @param algorithm `"meanfield"` for independent normals, `"fullrank"` for a
#'   normal with a full covariance matrix.
#' @param seed Seed. Also the model-construction seed for this run when
#'   transformed data draws from it; see [stanli_model()].
#' @param iter Maximum number of iterations.
#' @param grad_samples,elbo_samples Monte Carlo draws for each gradient and
#'   for each evaluation of the lower bound.
#' @param eta Step size. Used as given when `adapt_engaged` is `FALSE`.
#' @param adapt_engaged Choose the step size by a short search first.
#' @param adapt_iter Iterations for each step size the search tries.
#' @param tol_rel_obj Relative change in the lower bound below which the fit
#'   is taken as converged.
#' @param eval_elbo Evaluate the lower bound every this many iterations.
#' @param draws Number of draws to take from the approximation.
#' @param init Optional starting point on the unconstrained scale; see
#'   [unconstrain()].
#' @param init_radius Random starts are drawn uniform(-r, r); 0 starts at the
#'   origin.
#' @param refresh Any positive value prints Stan's progress, including the
#'   table of lower-bound values. 0 prints nothing.
#' @details The approximation is not checked against the posterior. ADVI can
#'   stop early at a poor optimum, and a mean-field fit ignores correlation
#'   by construction, so compare with [sample_model()] before relying on the
#'   spread of the draws. Reaching `iter` without converging is reported as a
#'   warning and the fit reached so far is returned.
#'
#'   Stan measures the lower bound with every constant of the log density
#'   included. stanli evaluates the density `lp__` reports, which leaves out
#'   the constants of `~` statements, so for a model written with them the
#'   lower bound printed here sits a constant below CmdStan's and the
#'   relative stopping rule can end on a different iteration. A model that
#'   uses `target +=` throughout is unaffected.
#' @return A `stanli_fit` with one chain of `draws` draws. Its `sampler`
#'   element holds `lp__`, the log density at each draw, and `lp_approx__`,
#'   the approximation's unnormalized log density there. `mean` holds every
#'   column at the mean of the approximation, and `report$sampling_seconds`
#'   the wall time of the run.
#' @export
variational_model <- function(model, algorithm = c("meanfield", "fullrank"),
                              seed = 1, iter = 10000, grad_samples = 1,
                              elbo_samples = 100, eta = 1,
                              adapt_engaged = TRUE, adapt_iter = 50,
                              tol_rel_obj = 0.01, eval_elbo = 100,
                              draws = 1000, init = NULL, init_radius = 2,
                              refresh = 100) {
  algorithm <- match.arg(algorithm)
  opts <- list(
    as.integer(seed), algorithm == "fullrank",
    algorithm_count(iter, "iter"),
    algorithm_count(grad_samples, "grad_samples"),
    algorithm_count(elbo_samples, "elbo_samples"),
    algorithm_number(eta, "eta"), stanfit_flag(adapt_engaged, "adapt_engaged"),
    algorithm_count(adapt_iter, "adapt_iter"),
    algorithm_number(tol_rel_obj, "tol_rel_obj"),
    algorithm_count(eval_elbo, "eval_elbo"), algorithm_count(draws, "draws"),
    algorithm_number(init_radius, "init_radius", positive = FALSE),
    algorithm_count(refresh, "refresh", 0L))
  load_runtime()
  model <- with_run_seed(model, seed)
  started <- proc.time()[["elapsed"]]
  res <- .Call("stanli_r_variational", model$ptr, opts,
               algorithm_inits(model, init, 1L))
  elapsed <- proc.time()[["elapsed"]] - started
  algorithm_interrupt(res, "variational inference")
  algorithm_notes(res)
  mean <- res$mean
  names(mean) <- model$columns
  approximation_fit(model, algorithm, seed, res$values,
                    list(lp__ = res$lp, lp_approx__ = res$lp_approx), elapsed,
                    list(mean = mean))
}

#' Pathfinder variational inference
#'
#' Runs `num_paths` single-path Pathfinders, each a normal approximation
#' chosen along an L-BFGS path, and resamples their draws by Pareto smoothed
#' importance sampling. This is Stan's own multi-path Pathfinder, the
#' algorithm behind CmdStan's `pathfinder` method, with its defaults.
#'
#' @param model A `stanli_model`.
#' @param seed Seed; path `p` uses the stream of chain id `p`.
#' @param num_paths Number of single-path runs. With 1, the single-path
#'   algorithm runs alone and nothing is resampled.
#' @param draws Number of draws returned after resampling.
#' @param single_path_draws Number of draws taken from each path's
#'   approximation. Without resampling, all `num_paths * single_path_draws`
#'   are returned and `draws` is not used.
#' @param max_lbfgs_iters Maximum L-BFGS iterations per path.
#' @param num_elbo_draws Draws used to compare the approximations along a
#'   path.
#' @param history_size,init_alpha,tol_obj,tol_rel_obj,tol_grad,tol_rel_grad,tol_param
#'   L-BFGS settings, as in CmdStan.
#' @param psis_resample Resample across paths. `FALSE` returns every path's
#'   draws unweighted.
#' @param calculate_lp Evaluate the log density at each draw. `FALSE` saves
#'   those evaluations, leaves `lp__` as `NaN`, and turns resampling off.
#' @param init Optional starting points on the unconstrained scale: one
#'   vector shared by every path or a matrix with one row per path.
#' @param init_radius Random starts are drawn uniform(-r, r).
#' @param refresh Any positive value prints Stan's progress every that many
#'   L-BFGS iterations. 0 prints nothing.
#' @details The paths run one after another. A warning repeats what Stan
#'   reports about the run, such as a Pareto k above 0.7, which means the
#'   importance weights are unreliable and the draws should not be trusted,
#'   or a path that failed. Resampling is with replacement, so the draws
#'   repeat; the number of distinct draws is what carries information.
#' @return A `stanli_fit` with one chain. Its `sampler` element holds `lp__`,
#'   `lp_approx__`, and `path__`, the path each draw came from.
#' @export
pathfinder_model <- function(model, seed = 1, num_paths = 4, draws = 1000,
                             single_path_draws = 1000, max_lbfgs_iters = 1000,
                             num_elbo_draws = 25, history_size = 5,
                             init_alpha = 0.001, tol_obj = 1e-12,
                             tol_rel_obj = 1e4, tol_grad = 1e-8,
                             tol_rel_grad = 1e7, tol_param = 1e-8,
                             psis_resample = TRUE, calculate_lp = TRUE,
                             init = NULL, init_radius = 2, refresh = 0) {
  num_paths <- algorithm_count(num_paths, "num_paths")
  opts <- list(
    as.integer(seed), num_paths,
    algorithm_count(single_path_draws, "single_path_draws"),
    algorithm_count(draws, "draws"),
    algorithm_count(num_elbo_draws, "num_elbo_draws"),
    algorithm_count(max_lbfgs_iters, "max_lbfgs_iters"),
    algorithm_count(history_size, "history_size"),
    algorithm_number(init_alpha, "init_alpha"),
    algorithm_number(tol_obj, "tol_obj", positive = FALSE),
    algorithm_number(tol_rel_obj, "tol_rel_obj", positive = FALSE),
    algorithm_number(tol_grad, "tol_grad", positive = FALSE),
    algorithm_number(tol_rel_grad, "tol_rel_grad", positive = FALSE),
    algorithm_number(tol_param, "tol_param", positive = FALSE),
    algorithm_number(init_radius, "init_radius", positive = FALSE),
    stanfit_flag(psis_resample, "psis_resample"),
    stanfit_flag(calculate_lp, "calculate_lp"),
    algorithm_count(refresh, "refresh", 0L))
  load_runtime()
  model <- with_run_seed(model, seed)
  started <- proc.time()[["elapsed"]]
  res <- .Call("stanli_r_pathfinder", model$ptr, opts,
               algorithm_inits(model, init, num_paths))
  elapsed <- proc.time()[["elapsed"]] - started
  algorithm_interrupt(res, "Pathfinder")
  algorithm_notes(res)
  approximation_fit(model, "pathfinder", seed, res$values,
                    list(lp__ = res$lp, lp_approx__ = res$lp_approx,
                         path__ = res$path), elapsed)
}

#' Draws from a Laplace approximation
#'
#' Draws from the normal distribution, on the unconstrained scale, centred at
#' the posterior mode with the curvature of the log density there. This is
#' Stan's own Laplace sampling, the algorithm behind CmdStan's `laplace`
#' method.
#'
#' @param model A `stanli_model`.
#' @param seed Seed.
#' @param draws Number of draws.
#' @param mode The mode to expand around: the result of [optimize_model()],
#'   or a point on the unconstrained scale (see [unconstrain()]). `NULL` runs
#'   [optimize_model()] with `seed`, `iter`, `init` and `init_radius`.
#' @param jacobian Must be `TRUE`. As for [optimize_model()], the
#'   change-of-variables Jacobian is part of the compiled model, so the mode
#'   and the curvature are those of the posterior on the unconstrained scale.
#'   CmdStan's `laplace` has the same default.
#' @param iter,init,init_radius Settings for the optimizer when `mode` is
#'   `NULL`.
#' @param calculate_lp Evaluate the log density at each draw. `FALSE` leaves
#'   `lp__` as `NaN`.
#' @param refresh Any positive value prints Stan's progress every that many
#'   draws. 0 prints nothing.
#' @details The curvature is Stan's finite-difference Hessian of the
#'   gradient. A point that is not a mode, or a mode with a direction of zero
#'   or positive curvature, has no normal approximation; Stan does not check
#'   for it and the draws are then meaningless or not finite.
#' @return A `stanli_fit` with one chain of `draws` draws. Its `sampler`
#'   element holds `lp__`, the log density at each draw, and `lp_approx__`,
#'   the approximation's unnormalized log density there. `mode` holds the
#'   unconstrained point that was expanded around.
#' @export
laplace_model <- function(model, seed = 1, draws = 1000, mode = NULL,
                          jacobian = TRUE, iter = 2000, init = NULL,
                          init_radius = 2, calculate_lp = TRUE, refresh = 0) {
  if (!stanfit_flag(jacobian, "jacobian"))
    stop("jacobian = FALSE is unsupported: stanli's compiled graph includes ",
         "the parameter-transform Jacobian", call. = FALSE)
  draws <- algorithm_count(draws, "draws")
  load_runtime()
  model <- with_run_seed(model, seed)
  if (is.null(mode))
    mode <- optimize_model(model, seed = seed, iter = iter, init = init,
                           init_radius = init_radius)
  if (is.list(mode)) mode <- mode$unconstrained
  if (!is.numeric(mode) || length(mode) != model$n_unconstrained ||
      any(!is.finite(mode)))
    stop("mode must be the result of optimize_model() or ",
         model$n_unconstrained, " finite unconstrained values", call. = FALSE)
  opts <- list(as.integer(seed), draws, TRUE,
               stanfit_flag(calculate_lp, "calculate_lp"),
               algorithm_count(refresh, "refresh", 0L))
  started <- proc.time()[["elapsed"]]
  res <- .Call("stanli_r_laplace", model$ptr, opts, as.double(mode))
  elapsed <- proc.time()[["elapsed"]] - started
  algorithm_interrupt(res, "Laplace sampling")
  algorithm_notes(res)
  approximation_fit(model, "laplace", seed, res$values,
                    list(lp__ = res$lp, lp_approx__ = res$lp_approx), elapsed,
                    list(mode = as.double(mode)))
}
