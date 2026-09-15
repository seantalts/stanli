# Optional-package S3 methods are registered by .onLoad, without importing
# (or eagerly loading) the plotting and workflow packages.
register_ecosystem_methods <- function() {
  methods <- list(
    bayesplot = c("nuts_params", "log_posterior", "rhat", "neff_ratio"),
    loo = c("loo", "loo_compare"),
    tidybayes = "tidy_draws"
  )
  for (package in names(methods)) {
    local({
      pkg <- package
      generics <- methods[[pkg]]
      register <- function(...) {
        for (generic in generics)
          registerS3method(generic, "stanli_fit",
                           get(paste0(generic, ".stanli_fit"),
                               envir = asNamespace("stanli")),
                           envir = asNamespace(pkg))
      }
      if (pkg %in% loadedNamespaces()) register()
      setHook(packageEvent(pkg, "onLoad"), register)
    })
  }
}

post_warmup_rows <- function(fit, inc_warmup = FALSE) {
  n <- dim(fit$draws)[1L]
  warmup <- if (inc_warmup || is.null(fit$warmup_draws)) 0L else fit$warmup_draws
  seq_len(n)[seq_len(n) > warmup]
}

# The exact CmdStanMCMC table contract: chain, iteration, then parameter;
# lp has no Parameter column. Iteration varies fastest within each chain.
sampler_long <- function(object, pars, inc_warmup = FALSE, parameter = TRUE) {
  rows <- post_warmup_rows(object, inc_warmup)
  arr <- object$sampler[rows, , pars, drop = FALSE]
  d <- dim(arr)
  out <- data.frame(
    Chain = rep(rep(seq_len(d[2L]), each = d[1L]), times = d[3L]),
    Iteration = rep(seq_len(d[1L]), times = d[2L] * d[3L])
  )
  if (parameter)
    out$Parameter <- factor(rep(dimnames(arr)[[3L]], each = d[1L] * d[2L]),
                            levels = dimnames(arr)[[3L]])
  out$Value <- as.numeric(arr)
  out
}

#' R workflow integrations
#'
#' Optional methods for bayesplot, loo, and tidybayes. Install the relevant
#' package to use its generic; no plotting package is needed to load stanli.
#' Saved warmup is excluded by default.
#'
#' @param object,x,model A `stanli_fit`.
#' @param pars Optional variable names (base names select all indexed elements).
#'   For `nuts_params`, exact sampler names such as `divergent__`.
#' @param inc_warmup Include saved warmup in sampler tables.
#' @param ... Further arguments passed to the underlying package where applicable.
#' @return `nuts_params` returns a data frame with `Chain`, `Iteration`,
#'   `Parameter` (a factor), and `Value`; `log_posterior` omits `Parameter`.
#'   `rhat` and `neff_ratio` return named numeric vectors. `tidy_draws` returns
#'   a tibble with draw indices. `loo` returns a `psis_loo` object and
#'   `loo_compare` compares fits via their LOO results.
#' @details R-hat and effective sample size use [summary.stanli_fit()].
#'   The ESS ratio is bulk ESS divided by the number of retained post-warmup
#'   draws. Bayesplot's CmdStanMCMC method instead uses basic ESS.
#' @name stanli-ecosystem
NULL

#' @rdname stanli-ecosystem
nuts_params.stanli_fit <- function(object, pars = NULL, inc_warmup = FALSE, ...) {
  if (is.null(pars))
    pars <- setdiff(dimnames(object$sampler)[[3L]], "lp__")
  sampler_long(object, pars, inc_warmup)
}

#' @rdname stanli-ecosystem
log_posterior.stanli_fit <- function(object, inc_warmup = FALSE, ...) {
  sampler_long(object, "lp__", inc_warmup, parameter = FALSE)
}

select_draw_variables <- function(columns, pars) {
  if (is.null(pars)) return(seq_along(columns))
  indices <- lapply(pars, function(par) {
    selected <- which(columns == par | startsWith(columns, paste0(par, "[")))
    if (!length(selected)) stop("unknown variable: ", par, call. = FALSE)
    selected
  })
  unique(unlist(indices, use.names = FALSE))
}

fit_diagnostic <- function(object, pars, column) {
  object$draws <- object$draws[post_warmup_rows(object), , , drop = FALSE]
  # A deserialized fit needs the runtime for stansummary, but no live model.
  load_runtime()
  s <- summary(object)
  values <- s[[column]]
  if (column == "ess_bulk") values <- values / prod(dim(object$draws)[1:2])
  names(values) <- s$variable
  values[select_draw_variables(s$variable, pars)]
}

#' @rdname stanli-ecosystem
rhat.stanli_fit <- function(object, pars = NULL, ...) {
  fit_diagnostic(object, pars, "rhat")
}

#' @rdname stanli-ecosystem
neff_ratio.stanli_fit <- function(object, pars = NULL, ...) {
  fit_diagnostic(object, pars, "ess_bulk")
}

#' Extract pointwise log likelihoods
#'
#' Declare the pointwise log likelihood in Stan's generated quantities block,
#' for example `vector[N] log_lik` with one log density per observation.
#'
#' @param object A fitted model.
#' @param variable Name of the pointwise log-likelihood variable.
#' @param ... Arguments passed to methods.
#' @return A numeric array (iteration, chain, observation), in Stan column order,
#'   excluding saved warmup. A scalar variable retains a singleton third axis.
#' @export
log_lik <- function(object, ...) UseMethod("log_lik")

#' @rdname log_lik
#' @export
log_lik.stanli_fit <- function(object, variable = "log_lik", ...) {
  if (!is.character(variable) || length(variable) != 1L ||
      is.na(variable) || !nzchar(variable))
    stop("variable must be a single nonempty name", call. = FALSE)
  columns <- object$columns
  selected <- which(columns == variable |
                    startsWith(columns, paste0(variable, "[")))
  if (!length(selected))
    stop("No variable '", variable, "' found. Declare pointwise log likelihoods ",
         "in the Stan generated quantities block (for example, vector[N] ",
         variable, ").", call. = FALSE)
  unclass(as_draws_array(object))[, , selected, drop = FALSE]
}

#' @rdname stanli-ecosystem
#' @param variable Pointwise log-likelihood variable passed to [log_lik()].
#' @param r_eff Optional relative effective sample sizes. By default computed
#'   from the likelihood with `loo::relative_eff()` using the chain axis.
loo.stanli_fit <- function(x, ..., variable = "log_lik", r_eff = NULL) {
  ll <- log_lik(x, variable = variable)
  if (is.null(r_eff)) r_eff <- loo::relative_eff(exp(ll))
  loo::loo(ll, r_eff = r_eff, ...)
}

#' @rdname stanli-ecosystem
loo_compare.stanli_fit <- function(x, ...) {
  fits <- list(x, ...)
  if (!all(vapply(fits, inherits, logical(1), "stanli_fit")))
    stop("supply stanli_fit objects, or compare their loo() results", call. = FALSE)
  loo::loo_compare(lapply(fits, loo::loo))
}

#' @rdname stanli-ecosystem
tidy_draws.stanli_fit <- function(model, ...) {
  tidybayes::tidy_draws(as_draws_array(model, include_sampler = TRUE), ...)
}
