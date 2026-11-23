#' Sample a model with WALNUTS
#'
#' Runs WALNUTS, the within-orbit adaptive step-length No-U-Turn sampler
#' (arXiv:2506.18746), through the walnutpie implementation built into the
#' runtime. NUTS uses one step size
#' for a whole trajectory. WALNUTS halves the step inside a trajectory where
#' the energy error grows too large, which helps on funnels and other
#' posteriors whose curvature changes from place to place. [sample_model()]
#' stays the default sampler.
#'
#' The chains run one after another on the calling thread, because the
#' runtime samples one WALNUTS chain at a time on one model. Each chain uses
#' the model's `threads_per_chain`.
#'
#' Chain `c` uses the random stream for `(seed, chain_id + c - 1)`, as in
#' [sample_model()]. Without `pathfinder_init`, a run of chains 1 to 4 gives
#' the same draws, chain for chain, as four one-chain runs with
#' `chain_id = 1:4` and the same `seed`.
#' Without `init` or `pathfinder_init`, a chain starts where the NUTS chain
#' with the same `seed` and chain id starts. With `pathfinder_init`, all
#' chains' starts come from one Pathfinder run seeded by the first chain's
#' id, so splitting the run into separate calls can change the starts.
#' Generated quantities draw from the stream that [sample_model()] would
#' give that seed and chain id, but they do not match its draws, because
#' NUTS shares that stream with the sampler.
#'
#' The result is a `stanli_fit`, so [summary.stanli_fit()], [as_draws_array()],
#' [log_lik()], [as_stanfit()], [as_rfit()] and [as_cstanfit()] work on it.
#' The runtime reports less about a WALNUTS run than about a NUTS run:
#' * `fit$sampler` holds `lp__`, the log density at each draw, the same
#'   quantity as in [sample_model()], and `stepsize__`, the macro step size
#'   that warmup adapted. WALNUTS halves that step within a trajectory, so a
#'   transition can use smaller steps. There are no divergences, tree depths,
#'   leapfrog counts, energies or acceptance statistics.
#' * The adapted metric is not reported, so `fit$inv_metric` is `NULL` and
#'   `as_cstanfit(fit)$inv_metric()` is an error.
#' * The maximum tree depth is 10 and a macro step can be halved at most 5
#'   times. Neither can be changed.
#' * There is no thinning and no saved warmup.
#' * `report$warmup_seconds` is `NA`, and `report$sampling_seconds` is the
#'   wall time of each chain, warmup included.
#' * [sample_more()], [stanli_diagnose()], `nuts_params()` and
#'   [get_sampler_params()] need a NUTS fit and refuse this one.
#'
#' Transformed data that draws random numbers uses `seed`, as it does in
#' [sample_model()], and the fit's `model` is the model built under it.
#'
#' @param model A `stanli_model`.
#' @param chains,seed,warmup,samples Number of chains, seed, warmup
#'   iterations and post-warmup draws per chain.
#' @param max_error The largest change in the Hamiltonian allowed across one
#'   macro step before WALNUTS halves the step. Smaller values take smaller
#'   steps where the posterior is hard.
#' @param init Optional starting point on the unconstrained scale: one vector
#'   for every chain, or a matrix with one row per chain, as in
#'   [sample_model()]. See [unconstrain()].
#' @param init_radius Random starts are drawn uniform(-r, r); 0 starts at the
#'   origin.
#' @param chain_id Id of the first chain, a positive integer, as in
#'   [sample_model()].
#' @param refresh Print a progress update every `refresh` iterations within
#'   each phase, plus the first and last iteration of the phase. Set to 0 for
#'   no output.
#' @param pathfinder_init Optional named list enabling Pathfinder-generated
#'   starts, as in [sample_model()]. An empty list uses defaults; supported
#'   entries are `num_iterations` (1000), `num_elbo_draws` (25),
#'   `history_size` (5), and Pathfinder's own `init_radius` (2). Uses `seed`
#'   and the first `chain_id`, returns one start per chain, and cannot be
#'   combined with `init`. Single-path Pathfinder does not perform PSIS
#'   resampling.
#' @param ... Not used. Options that WALNUTS lacks, such as `thin` or
#'   `delta`, are an error.
#' @details Interrupting R (Ctrl-C, or the stop button in RStudio) stops the
#'   run when the current chain finishes, because the runtime cannot stop a
#'   WALNUTS chain partway. It then raises the usual interrupt and returns no
#'   fit.
#' @return An object of class `stanli_fit` with `algorithm` and
#'   `sampler_backend` set to `"walnuts"`, `max_error` recording the setting
#'   and `step_size` the adapted macro step size of each chain.
#' @export
sample_walnuts <- function(model, chains = 4, seed = 1, warmup = 1000,
                           samples = 1000, max_error = 0.5, init = NULL,
                           init_radius = 2, chain_id = 1L, refresh = 100,
                           pathfinder_init = NULL, ...) {
  if (!inherits(model, "stanli_model"))
    stop("model must be a stanli_model", call. = FALSE)
  extra <- names(list(...))
  if (length(list(...)))
    stop(if (is.null(extra) || !all(nzchar(extra))) "unnamed arguments are" else
           paste(paste(extra, collapse = ", "),
                 if (length(extra) > 1L) "are" else "is"),
         " not available in sample_walnuts(): supported arguments are ",
         "model, chains, seed, warmup, samples, max_error, init, init_radius, ",
         "chain_id, refresh and pathfinder_init", call. = FALSE)
  cstan_integer(seed, "seed", 0)
  chains <- algorithm_count(chains, "chains")
  check_chain_id(chain_id, chains)
  chain_id <- as.integer(chain_id)
  warmup <- algorithm_count(warmup, "warmup", 0L)
  samples <- algorithm_count(samples, "samples")
  max_error <- algorithm_number(max_error, "max_error")
  init_radius <- algorithm_number(init_radius, "init_radius", positive = FALSE)
  refresh <- algorithm_count(refresh, "refresh", 0L)
  if (!is.null(init) && !is.null(pathfinder_init))
    stop("init and pathfinder_init are mutually exclusive", call. = FALSE)
  if (!is.null(pathfinder_init))
    pathfinder_init <- .pathfinder_init_options(pathfinder_init)
  load_runtime()
  model <- with_run_seed(model, seed)
  ncol <- length(model$columns)
  n <- model$n_unconstrained
  if (!is.null(pathfinder_init)) {
    init <- .Call("stanli_r_pathfinder_inits", model$ptr, as.integer(seed),
                  as.integer(chains), unname(pathfinder_init), chain_id)
    algorithm_interrupt(init, "Pathfinder initialization")
  }
  starts <- algorithm_inits(model, init, chains)
  draws <- array(NA_real_, c(samples, chains, ncol),
                 dimnames = list(NULL, NULL, model$columns))
  unconstrained <- array(NA_real_, c(samples, chains, n))
  reported <- c("lp__", "stepsize__")
  sampler <- array(NA_real_, c(samples, chains, length(reported)),
                   dimnames = list(NULL, NULL, reported))
  stat_names <- .Call("stanli_r_sampler_columns")
  seconds <- numeric(chains)
  for (chain in seq_len(chains)) {
    start <- if (length(starts)) starts[(chain - 1L) * n + seq_len(n)] else
      numeric(0)
    started <- proc.time()[["elapsed"]]
    res <- .Call("stanli_r_walnuts", model$ptr,
                 list(as.integer(seed), warmup, samples, max_error, refresh,
                      chain_id + chain - 1L, init_radius), start)
    seconds[chain] <- proc.time()[["elapsed"]] - started
    algorithm_interrupt(res, "sampling")
    # C fills rows column-fastest; see sample_model().
    draws[, chain, ] <- matrix(res$values, samples, ncol, byrow = TRUE)
    unconstrained[, chain, ] <- matrix(res$unconstrained, samples, n,
                                       byrow = TRUE)
    stats <- matrix(res$stats, samples, length(stat_names), byrow = TRUE,
                    dimnames = list(NULL, stat_names))
    sampler[, chain, ] <- stats[, reported]
  }
  structure(list(
    draws = draws, sampler = sampler, unconstrained = unconstrained,
    columns = model$columns, max_depth = 10L, seed = seed, model = model,
    algorithm = "walnuts", sampler_backend = "walnuts",
    report = list(warmup_seconds = rep(NA_real_, chains),
                  sampling_seconds = seconds),
    warmup_draws = 0L, warmup = warmup, thin = 1L, samples = samples,
    save_warmup = FALSE, chains = chains, chain_id = chain_id,
    max_error = max_error, step_size = sampler[samples, , "stepsize__"]),
    class = "stanli_fit")
}

is_walnuts_fit <- function(x) identical(x$algorithm, "walnuts")
