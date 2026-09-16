#' Sample with CmdStanR-style arguments
#'
#' Prepare and sample a Stan model with Stanli, returning [as_cstanfit()].
#' This is a convenience interface; it does not install or call CmdStanR.
#'
#' @param model_code Stan program source.
#' @param data Named list of model data.
#' @param chains Number of chains.
#' @param parallel_chains Maximum concurrent chains.
#' @param iter_warmup,iter_sampling Iterations per chain before thinning.
#' @param seed Integer seed. If NULL, draw one from R's RNG.
#' @param init Complete constrained parameter list, one list per chain, a
#'   function returning a list (optionally accepting `chain_id`), or NULL for
#'   random initialization. Partial lists, numeric radii, and files are unsupported;
#'   use `init_radius` for random initialization.
#' @param adapt_delta Target acceptance probability.
#' @param max_treedepth Maximum NUTS tree depth.
#' @param thin Retain every thin-th iteration, separately in warmup and sampling.
#' @param save_warmup Retain warmup draws.
#' @param refresh Progress interval; zero suppresses progress.
#' @param init_radius Radius for random initialization on the unconstrained scale.
#' @param pathfinder_init Optional Pathfinder settings passed to [sample_model()].
#' @param ... Reserved. Unsupported options produce an error.
#' @return A native `stanli_cstanfit`.
#' @details Model preparation uses the sampling seed, including RNG calls in
#'   transformed data. Within-chain threading and C++ compiler options are not
#'   supported. Existing Stanli models and unconstrained initial values can be
#'   used through [sample_model()] followed by [as_cstanfit()].
#' @export
sample_cstan <- function(model_code, data = list(), chains = 4, parallel_chains = chains,
                         iter_warmup = 1000, iter_sampling = 1000, seed = NULL,
                         init = NULL, adapt_delta = 0.8, max_treedepth = 10,
                         thin = 1, save_warmup = FALSE, refresh = 100,
                         init_radius = 2, pathfinder_init = NULL, ...) {
  if (length(list(...))) stop("unsupported sample_cstan arguments: ",
                             paste(names(list(...)), collapse = ", "), call. = FALSE)
  for (name in c("chains", "parallel_chains", "iter_sampling", "max_treedepth", "thin"))
    cstan_integer(get(name), name)
  cstan_integer(iter_warmup, "iter_warmup", 0)
  cstan_integer(refresh, "refresh", 0)
  stanfit_flag(save_warmup, "save_warmup")
  if (length(adapt_delta) != 1L || !is.numeric(adapt_delta) || !is.finite(adapt_delta) ||
      adapt_delta <= 0 || adapt_delta >= 1)
    stop("adapt_delta must be between 0 and 1", call. = FALSE)
  if (length(init_radius) != 1L || !is.numeric(init_radius) || !is.finite(init_radius) || init_radius < 0)
    stop("init_radius must be finite and nonnegative", call. = FALSE)
  if (!is.null(init) && !is.null(pathfinder_init))
    stop("init and pathfinder_init cannot be combined", call. = FALSE)
  if (!is.null(init) && !is.list(init) && !is.function(init))
    stop("init must be a complete constrained list or function", call. = FALSE)
  if (is.null(seed)) seed <- sample.int(.Machine$integer.max, 1)
  cstan_integer(seed, "seed", 0)
  model <- stanli_model(code = model_code, data = data, seed = seed)
  unconstrained <- NULL
  if (!is.null(init)) {
    per_chain <- is.list(init) && is.null(names(init))
    if (per_chain && (length(init) != chains || !all(vapply(init, is.list, logical(1)))))
      stop("init must supply one complete named list per chain", call. = FALSE)
    unconstrained <- do.call(rbind, lapply(seq_len(chains), function(chain) {
      values <- if (is.function(init)) {
        if ("chain_id" %in% names(formals(init))) init(chain_id = chain) else init()
      } else if (per_chain) init[[chain]] else init
      if (!is.list(values) || is.null(names(values)) || any(!nzchar(names(values))) ||
          anyDuplicated(names(values)))
        stop("init must supply a complete named list of parameters", call. = FALSE)
      unconstrain(model, values)
    }))
  }
  as_cstanfit(sample_model(model, chains = chains, parallel_chains = min(chains, parallel_chains),
    warmup = iter_warmup, samples = iter_sampling, seed = seed, init = unconstrained,
    delta = adapt_delta, max_depth = max_treedepth, thin = thin, save_warmup = save_warmup,
    refresh = refresh, init_radius = init_radius, pathfinder_init = pathfinder_init))
}
