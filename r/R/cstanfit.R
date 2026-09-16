#' Native CmdStanR-style fit interface
#'
#' Wrap stored Stanli draws in a native object with CmdStanR-style methods.
#' Neither cmdstanr nor rstan is needed. The original fit is unchanged.
#'
#' @param x A `stanli_fit` or an existing `stanli_cstanfit`.
#' @return A `stanli_cstanfit` with methods described below. This is a native
#'   list of functions, not an R6 CmdStanMCMC or RStan S4 object.
#' @details
#' * `$draws(variables = NULL, inc_warmup = FALSE, format = "draws_array")`
#'   returns posterior draws, including `lp__` and excluding sampler diagnostics.
#'   Base names select all indexed elements. Supported formats are
#'   `draws_array`, `draws_matrix`, `draws_df`, `draws_list`, and `draws_rvars`.
#' * `$summary(variables = NULL, ...)` delegates to
#'   `posterior::summarise_draws()`, including custom functions and formulas.
#'   It uses rank-normalized R-hat and bulk/tail ESS by default, as CmdStanR does.
#' * `$sampler_diagnostics(inc_warmup = FALSE, format = "draws_array")`
#'   returns the six sampler diagnostics in stable alphabetical order;
#'   CmdStanR's CSV order may differ.
#' * `$metadata()` includes iteration counts **before** thinning, `thin`,
#'   `save_warmup`, variable names, chain IDs, and sampler settings.
#' * `$num_chains()` returns the number of chains.
#' * `$time()` returns `total` and a `chains` data frame with `chain_id`,
#'   `warmup`, `sampling`, and `total` seconds. Unavailable timings are `NA`;
#'   overall wall time is unavailable, rather than inferred from chain timings.
#' * `$loo(variables = "log_lik", r_eff = TRUE, moment_match = FALSE, ...)`
#'   computes LOO with optional relative efficiency. Moment matching is unsupported.
#' * `$stanli_fit()` retrieves the original fit for native Stanli operations.
#'
#' Draws and summaries require posterior; LOO also requires loo. Stored-draw
#' methods work after serialization without a runtime or live model. The Stanli
#' R package must remain installed. Requests for unsaved warmup fail explicitly.
#' Model recompilation, CSV output, and CmdStan's model methods are not provided.
#' Consumers that dispatch on CmdStanR class names must explicitly accept this
#' class or call the methods directly.
#' @export
as_cstanfit <- function(x) {
  if (inherits(x, "stanli_cstanfit")) return(x)
  if (!inherits(x, "stanli_fit")) stop("x must be a stanli_fit", call. = FALSE)
  required <- c("warmup", "samples", "thin", "chains", "warmup_draws", "save_warmup")
  if (!all(required %in% names(x)))
    stop("fit lacks sampling metadata; sample with the current Stanli version", call. = FALSE)
  for (name in c("warmup", "samples", "thin", "chains", "warmup_draws"))
    cstan_integer(x[[name]], name, if (name %in% c("warmup", "warmup_draws")) 0 else 1)
  stanfit_flag(x$save_warmup, "save_warmup")
  warmup <- if (x$save_warmup) ceiling(x$warmup / x$thin) else 0L
  dims <- c(warmup + ceiling(x$samples / x$thin), x$chains, length(x$columns))
  diagnostics <- cstan_diagnostic_names()
  if (!identical(dim(x$draws), as.integer(dims)) ||
      length(dim(x$sampler)) != 3L ||
      !identical(dim(x$sampler)[1:2], as.integer(dims[1:2])) ||
      x$warmup_draws != warmup ||
      (length(x$columns) > 0L && !identical(dimnames(x$draws)[[3L]], x$columns)) ||
      !all(c("lp__", diagnostics) %in% dimnames(x$sampler)[[3L]]))
    stop("fit arrays and sampling metadata do not agree", call. = FALSE)
  stanfit_column_layout(c("lp__", x$columns))
  cstan_methods(x)
}

# Keep only the fit in serialized closures. Method behavior is resolved from
# the installed namespace, so fixes also apply to previously saved fits.
cstan_methods <- function(x) {
  force(x)
  structure(list(
    draws = function(variables = NULL, inc_warmup = FALSE, format = "draws_array")
      cstan_draws(x, variables, inc_warmup, format),
    summary = function(variables = NULL, ...) cstan_summary(x, variables, ...),
    sampler_diagnostics = function(inc_warmup = FALSE, format = "draws_array")
      cstan_diagnostics(x, inc_warmup, format),
    metadata = function() cstan_metadata(x),
    num_chains = function() x$chains,
    time = function() cstan_time(x),
    loo = function(variables = "log_lik", r_eff = TRUE, moment_match = FALSE, ...)
      cstan_loo(x, variables, r_eff, moment_match, ...),
    stanli_fit = function() x
  ), class = "stanli_cstanfit")
}

cstan_draws <- function(x, variables = NULL, inc_warmup = FALSE, format = "draws_array") {
  if (!is.null(variables) && (!is.character(variables) || anyNA(variables)))
    stop("variables must be character names", call. = FALSE)
  rows <- cstan_rows(x, inc_warmup)
  columns <- c("lp__", x$columns)
  selected <- columns[select_draw_variables(columns, variables)]
  # Allocate only requested variables, avoiding copies of all generated
  # quantities for a small summary or trace plot.
  arr <- array(NA_real_, dim = c(length(rows), x$chains, length(selected)),
               dimnames = list(NULL, NULL, selected))
  parameters <- which(selected != "lp__")
  if (length(parameters))
    arr[, , parameters] <- x$draws[rows, , selected[parameters], drop = FALSE]
  lp <- which(selected == "lp__")
  if (length(lp)) arr[, , lp] <- x$sampler[rows, , "lp__", drop = FALSE]
  cstan_format(arr, format)
}

cstan_summary <- function(x, variables = NULL, ...) {
  posterior::summarise_draws(cstan_draws(x, variables), ...)
}

cstan_diagnostic_names <- function() {
  c("accept_stat__", "divergent__", "energy__", "n_leapfrog__", "stepsize__", "treedepth__")
}

cstan_diagnostics <- function(x, inc_warmup = FALSE, format = "draws_array") {
  cstan_format(x$sampler[cstan_rows(x, inc_warmup), , cstan_diagnostic_names(), drop = FALSE], format)
}

cstan_metadata <- function(x) {
  layout <- stanfit_column_layout(c("lp__", x$columns))
  list(method = "sample", algorithm = "hmc", engine = "nuts",
    model_name = x$model$model_name, iter_warmup = x$warmup, iter_sampling = x$samples,
    thin = x$thin, save_warmup = x$save_warmup, chains = x$chains,
    id = seq_len(x$chains), variables = c("lp__", x$columns),
    stan_variables = layout$parameters,
    stan_variable_sizes = lapply(layout$dimensions, function(d) if (length(d)) d else 1L),
    sampler_diagnostics = cstan_diagnostic_names(), adapt_delta = x$delta,
    max_treedepth = x$max_depth, seed = x$seed)
}

cstan_time <- function(x) {
  elapsed <- get_elapsed_time(x)
  chains <- data.frame(chain_id = seq_len(x$chains), warmup = elapsed[, "warmup"],
                        sampling = elapsed[, "sample"], row.names = NULL)
  chains$total <- chains$warmup + chains$sampling
  list(total = NA_real_, chains = chains)
}

cstan_loo <- function(x, variables = "log_lik", r_eff = TRUE, moment_match = FALSE, ...) {
  stanfit_flag(moment_match, "moment_match")
  if (moment_match) stop("moment matching is not supported by as_cstanfit()", call. = FALSE)
  if (!requireNamespace("loo", quietly = TRUE)) stop("LOO needs the loo package", call. = FALSE)
  if (!is.character(variables) || length(variables) != 1L || is.na(variables))
    stop("variables must be one log-likelihood variable", call. = FALSE)
  ll <- cstan_draws(x, variables)
  if (is.logical(r_eff)) {
    stanfit_flag(r_eff, "r_eff")
    r_eff <- if (r_eff) {
      # Scaling cancels in relative efficiency and prevents exp underflow.
      likelihood <- exp(sweep(ll, 3L, apply(ll, 3L, max), "-"))
      cores <- list(...)$cores
      loo::relative_eff(likelihood, cores = if (is.null(cores)) 1L else cores)
    } else NULL
  }
  loo::loo(ll, r_eff = r_eff, ...)
}

cstan_rows <- function(x, inc_warmup) {
  stanfit_flag(inc_warmup, "inc_warmup")
  if (inc_warmup && !x$save_warmup)
    stop("warmup draws were not saved; sample with save_warmup = TRUE", call. = FALSE)
  post_warmup_rows(x, inc_warmup)
}

cstan_format <- function(arr, format) {
  formats <- c("draws_array", "draws_matrix", "draws_df", "draws_list", "draws_rvars")
  if (!is.character(format) || length(format) != 1L || is.na(format) || !format %in% formats)
    stop("unsupported draws format", call. = FALSE)
  if (!requireNamespace("posterior", quietly = TRUE)) stop("draws need the posterior package", call. = FALSE)
  # Use numeric chain IDs and posterior's standard dimension labels.
  dimnames(arr) <- list(NULL, NULL, dimnames(arr)[[3L]])
  getExportedValue("posterior", paste0("as_", format))(arr)
}

cstan_integer <- function(x, name, minimum = 1) {
  if (length(x) != 1L || !is.numeric(x) || !is.finite(x) ||
      x < minimum || x != floor(x) || x > .Machine$integer.max)
    stop(name, " must be an integer >= ", minimum, call. = FALSE)
}

#' @rdname as_cstanfit
#' @param object A `stanli_cstanfit`.
#' @param variables Variables to summarize.
#' @param ... Arguments passed to `posterior::summarise_draws()`.
#' @export
summary.stanli_cstanfit <- function(object, variables = NULL, ...) {
  object$summary(variables, ...)
}

#' @rdname as_cstanfit
#' @param n Maximum number of summary rows to print (default 10).
#' @export
print.stanli_cstanfit <- function(x, variables = NULL, n = 10L, ...) {
  cstan_integer(n, "n")
  cat("Stanli fit with", x$num_chains(), "chains\n")
  if (!requireNamespace("posterior", quietly = TRUE)) {
    cat("Install posterior for a draw summary.\n")
  } else {
    if (is.null(variables)) variables <- utils::head(x$metadata()$variables, n)
    print(utils::head(x$summary(variables, ...), n))
  }
  invisible(x)
}
