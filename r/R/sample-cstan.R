#' Sample with CmdStanR-style arguments
#'
#' Prepare and sample a Stan model with Stanli, returning [as_cstanfit()].
#' This is a convenience interface; it does not install or call CmdStanR.
#'
#' @param model_code Stan program source.
#' @param data Named list of model data.
#' @param chains Number of chains.
#' @param parallel_chains Maximum concurrent chains.
#' @param threads_per_chain Threads per chain for eligible native `reduce_sum`
#'   calls. The model is built for this count; see [stanli_model()]. Defaults
#'   to 1.
#' @param iter_warmup,iter_sampling Iterations per chain before thinning.
#' @param seed Integer seed. If NULL, draw one from R's RNG.
#' @param init Constrained parameter list, one list per chain, a
#'   function returning a list (optionally accepting `chain_id`), a number,
#'   a `posterior` draws object, a fit from `sample_cstan()`, or NULL for
#'   random initialization, as in cmdstanr. A list may leave parameters out;
#'   those start at random within `init_radius`, as in CmdStan. A number is
#'   the radius of that draw, so 0 starts at zero on the unconstrained
#'   scale. Draws, or a fit's draws, give each chain one draw, recycled when
#'   there are fewer draws than chains and otherwise drawn without
#'   replacement. Files and fits from the approximate methods are
#'   unsupported. Extra non-parameter names (such as transformed parameters)
#'   are ignored, as in CmdStanR.
#' @param adapt_delta Target acceptance probability.
#' @param max_treedepth Maximum NUTS tree depth.
#' @param thin Retain every thin-th iteration, separately in warmup and sampling.
#' @param save_warmup Retain warmup draws.
#' @param refresh Progress interval; zero suppresses progress.
#' @param show_messages `FALSE` drops the sampler's notices and the model's own
#'   output during sampling; see [sample_model()].
#' @param init_radius Radius for random initialization on the unconstrained scale.
#' @param pathfinder_init Optional Pathfinder settings passed to [sample_model()].
#' @param metric,inv_metric,step_size,adapt_engaged,adapt_metric Metric kind
#'   (`"diag_e"`, `"dense_e"` or `"unit_e"`), starting inverse metric, initial
#'   step size, whether adaptation is engaged and whether it adapts the metric,
#'   passed to [sample_model()], which describes them. `inv_metric` may be one
#'   vector or matrix, or a list with one per chain.
#' @param include_paths Directories searched for Stan `#include` files; see
#'   [stanli_model()].
#' @param ... Reserved. Unsupported options produce an error.
#' @return A native `stanli_cstanfit`.
#' @details Model preparation uses the sampling seed, including RNG calls in
#'   transformed data. C++ compiler options are not supported. Existing Stanli
#'   models and unconstrained initial values can be
#'   used through [sample_model()] followed by [as_cstanfit()].
#' @export
sample_cstan <- function(model_code, data = list(), chains = 4, parallel_chains = chains,
                         iter_warmup = 1000, iter_sampling = 1000, seed = NULL,
                         init = NULL, adapt_delta = 0.8, max_treedepth = 10,
                         thin = 1, save_warmup = FALSE, refresh = 100,
                         init_radius = 2, pathfinder_init = NULL,
                         threads_per_chain = 1, include_paths = NULL,
                         metric = c("diag_e", "dense_e", "unit_e"),
                         inv_metric = NULL, step_size = NULL,
                         adapt_engaged = NULL, adapt_metric = TRUE,
                         show_messages = TRUE, ...) {
  if (length(list(...))) stop("unsupported sample_cstan arguments: ",
                             paste(names(list(...)), collapse = ", "), call. = FALSE)
  for (name in c("chains", "parallel_chains", "threads_per_chain", "iter_sampling", "max_treedepth", "thin"))
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
  # A number is a radius, as in cmdstanr.
  if (is.numeric(init) && length(init) == 1L && !is.object(init)) {
    init_radius <- init
    init <- NULL
    if (!is.finite(init_radius) || init_radius < 0)
      stop("init must be finite and nonnegative when it is a number",
           call. = FALSE)
  }
  draws_init <- inherits(init, c("draws", "stanli_cstanfit"))
  if (!is.null(init) && !draws_init && (is.object(init) ||
      (!is.list(init) && !is.function(init))))
    stop("init must be a constrained list, a function, a number, a draws ",
         "object or an MCMC fit", call. = FALSE)
  metric <- match.arg(metric)
  if (is.null(seed)) seed <- sample.int(.Machine$integer.max, 1)
  cstan_integer(seed, "seed", 0)
  model <- stanli_model(code = model_code, data = data, seed = seed,
                        threads_per_chain = threads_per_chain,
                        include_paths = include_paths)
  unconstrained <- NULL
  if (draws_init) init <- draws_inits(model, init, chains, seed)
  if (!is.null(init)) {
    declared <- parameter_names(model)
    per_chain <- is.list(init) && is.null(names(init))
    if (per_chain && (length(init) != chains || !all(vapply(init, is.list, logical(1)))))
      stop("init must supply one named list per chain", call. = FALSE)
    unconstrained <- do.call(rbind, lapply(seq_len(chains), function(chain) {
      values <- if (is.function(init)) {
        if ("chain_id" %in% names(formals(init))) init(chain_id = chain) else init()
      } else if (per_chain) init[[chain]] else init
      if (!is.list(values) || is.null(names(values)) || anyNA(names(values)) || any(!nzchar(names(values))) ||
          anyDuplicated(names(values)))
        stop("init must supply a named list of parameters", call. = FALSE)
      values <- values[intersect(names(values), declared)]
      # parameters left out start at random, as in CmdStan
      unconstrain(model, values, partial = TRUE, init_radius = init_radius,
                  seed = seed + chain)
    }))
  }
  as_cstanfit(sample_model(model, chains = chains, parallel_chains = min(chains, parallel_chains),
    warmup = iter_warmup, samples = iter_sampling, seed = seed, init = unconstrained,
    delta = adapt_delta, max_depth = max_treedepth, thin = thin, save_warmup = save_warmup,
    refresh = refresh, init_radius = init_radius, pathfinder_init = pathfinder_init,
    metric = metric,
    inv_metric = inv_metric, step_size = step_size,
    adapt_engaged = adapt_engaged, adapt_metric = adapt_metric,
    show_messages = show_messages))
}

# One constrained start per chain from draws, as cmdstanr 1.0 takes them:
# recycled when there are fewer draws than chains, otherwise a sample
# without replacement. A parameter whose columns are all there is given;
# the rest start at random.
draws_inits <- function(model, init, chains, seed) {
  if (!requireNamespace("posterior", quietly = TRUE))
    stop("draws as init need the posterior package", call. = FALSE)
  if (inherits(init, "stanli_cstanfit")) init <- init$draws()
  draws <- as.data.frame(posterior::as_draws_df(init))
  if (!nrow(draws)) stop("init has no draws", call. = FALSE)
  rows <- if (nrow(draws) < chains) rep_len(seq_len(nrow(draws)), chains) else
    preserve_random_seed({
      set.seed(seed)
      sample.int(nrow(draws), chains)
    })
  variables <- runtime_variables(model$ptr, parameters = TRUE)
  columns <- lapply(names(variables), function(name)
    variable_columns(variables[name]))
  names(columns) <- names(variables)
  # A zero-size parameter has no columns. It is left out, except as a part
  # of a tuple, which is given only when every part is.
  bases <- variable_base(names(variables))
  given <- vapply(seq_along(columns), function(k)
    (length(columns[[k]]) > 0L || bases[k] != names(variables)[k]) &&
      all(columns[[k]] %in% names(draws)), logical(1))
  for (base in unique(bases)) given[bases == base] <- all(given[bases == base])
  used <- unlist(columns[given], use.names = FALSE)
  bad <- vapply(used, function(col) any(!is.finite(draws[[col]][rows])),
                logical(1))
  if (any(bad)) {
    names <- unique(variable_base(sub("\\[.*$", "", used[bad])))
    stop(if (length(names) > 1) "Variables: " else "Variable: ",
         paste(names, collapse = ", "), " contains NA or Inf values!",
         call. = FALSE)
  }
  lapply(rows, function(row) {
    values <- lapply(names(variables)[given], function(name) {
      value <- vapply(columns[[name]], function(col) draws[[col]][row],
                      numeric(1), USE.NAMES = FALSE)
      if (!length(value)) value <- numeric(0)
      dims <- variables[[name]]
      if (length(dims)) array(value, dim = dims) else value
    })
    names(values) <- names(variables)[given]
    bases <- variable_base(names(values))
    lapply(stats::setNames(nm = unique(bases)), function(base) {
      slots <- values[bases == base]
      if (identical(names(slots), base)) slots[[1L]] else
        tuple_init_value(slots, natural = TRUE)
    })
  })
}
