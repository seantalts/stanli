# Optional-package S3 methods are registered by .onLoad, without importing
# (or eagerly loading) the plotting and workflow packages.
register_ecosystem_methods <- function() {
  methods <- list(
    bayesplot = c("nuts_params", "log_posterior", "rhat", "neff_ratio"),
    loo = c("loo", "loo_compare"),
    tidybayes = "tidy_draws",
    bridgesampling = "bridge_sampler",
    shinystan = "launch_shinystan"
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
  if ("shinystan" %in% loadedNamespaces()) register_shinystan_method()
  setHook(packageEvent("shinystan", "onLoad"), function(...) register_shinystan_method())
}

# as.shinystan() is an S4 generic, so its method needs an S4 view of the
# S3 class. Like the stanfit classes, it lives in a package-owned registry.
.shinystan_registry <- new.env(parent = emptyenv())

register_shinystan_method <- function() {
  registry <- .shinystan_registry
  if (exists(".__C__stanli_fit", envir = registry, inherits = FALSE))
    return(invisible(NULL))
  parent.env(registry) <- asNamespace("stanli")
  registry$.packageName <- "stanli"
  methods::setOldClass("stanli_fit", where = registry)
  methods::setMethod(methods::getGeneric("as.shinystan", where = asNamespace("shinystan")),
                     signature = "stanli_fit", definition = as_shinystan_stanli_fit,
                     where = registry)
  invisible(NULL)
}

as_shinystan_stanli_fit <- function(X, ...) {
  shinystan::as.shinystan(as_stanfit(X), ...)
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
#' Optional methods for bayesplot, loo, tidybayes, bridgesampling, and
#' shinystan. Install the relevant package to use its generic; no plotting
#' package is needed to load stanli.
#' Saved warmup is excluded by default.
#'
#' @param object,x,model A `stanli_fit`.
#' @param pars Optional variable names (base names select all indexed elements).
#'   For `nuts_params`, exact sampler names such as `divergent__`.
#' @param inc_warmup Include saved warmup in sampler tables.
#' @param samples A `stanli_fit` for `bridge_sampler`.
#' @param repetitions,method,cores,use_neff,maxiter,silent,verbose Passed to
#'   `bridgesampling::bridge_sampler()`.
#' @param ... Further arguments passed to the underlying package where applicable.
#' @return `nuts_params` returns a data frame with `Chain`, `Iteration`,
#'   `Parameter` (a factor), and `Value`; `log_posterior` omits `Parameter`.
#'   `rhat` and `neff_ratio` return named numeric vectors. `tidy_draws` returns
#'   a tibble with draw indices. `loo` returns a `psis_loo` object and
#'   `loo_compare` compares fits via their LOO results. `bridge_sampler`
#'   returns a `bridge` object.
#' @details R-hat and effective sample size use [summary.stanli_fit()].
#'   The ESS ratio is bulk ESS divided by the number of retained post-warmup
#'   draws. Bayesplot's CmdStanMCMC method instead uses basic ESS.
#'
#'   `launch_shinystan` and `shinystan::as.shinystan()` convert the fit with
#'   [as_stanfit()] first, so they need rstan as well as shinystan.
#'
#'   `bridge_sampler` estimates the log marginal likelihood from the
#'   unconstrained post-warmup draws. Write the model with `target +=`
#'   statements: `~` drops normalizing constants, as in rstan, and the
#'   estimate is then wrong. It needs the fit's live model, so a fit restored
#'   with `readRDS()` must be sampled again from a new [stanli_model()].
#'   On Windows, `cores` is set to 1.
#' @name stanli-ecosystem
NULL

#' @rdname stanli-ecosystem
nuts_params.stanli_fit <- function(object, pars = NULL, inc_warmup = FALSE, ...) {
  require_nuts_fit(object, "nuts_params()")
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
    selected <- which(columns == par | startsWith(columns, paste0(par, "[")) |
                      startsWith(columns, paste0(par, ":")))
    if (!length(selected)) stop("unknown variable: ", par, call. = FALSE)
    selected
  })
  unique(unlist(indices, use.names = FALSE))
}

fit_diagnostic <- function(object, pars, column) {
  object$draws <- object$draws[post_warmup_rows(object), , , drop = FALSE]
  # A deserialized fit needs the runtime for stansummary, but no live model.
  load_runtime()
  s <- summary.stanli_fit(object)
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

# Other packages export a generic of this name too (rstantools, and brms and
# rstanarm through it). Whichever is attached last masks the rest, so an
# object this package has no method for is handed to that generic and not
# refused here.
#' @export
log_lik.default <- function(object, ...) {
  if (requireNamespace("rstantools", quietly = TRUE))
    return(rstantools::log_lik(object, ...))
  stop("no applicable method for 'log_lik' applied to an object of class \"",
       class(object)[1L], "\"", call. = FALSE)
}

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

#' @rdname stanli-ecosystem
bridge_sampler.stanli_fit <- function(samples, repetitions = 1, method = "normal",
                                      cores = 1, use_neff = TRUE, maxiter = 1000,
                                      silent = FALSE, verbose = FALSE, ...) {
  model <- samples$model
  if (!model_is_live(model))
    stop("This fit has no live stanli model (saveRDS does not preserve native ",
         "handles). Recreate stanli_model() with the original code and data ",
         "and sample again.", call. = FALSE)
  if (.Platform$OS.type != "unix" && cores != 1) {
    warning("cores > 1 is not supported on Windows; using cores = 1.",
            call. = FALSE)
    cores <- 1
  }
  u <- samples$unconstrained[post_warmup_rows(samples), , , drop = FALSE]
  n_par <- dim(u)[3L]
  p <- paste0("x", seq_len(n_par))
  chains <- coda::mcmc.list(lapply(seq_len(dim(u)[2L]), function(chain)
    coda::mcmc(matrix(u[, chain, ], ncol = n_par, dimnames = list(NULL, p)))))
  log_posterior <- function(pars, data) {
    lp <- tryCatch(.Call("stanli_r_log_prob", data$ptr, as.double(pars)),
                   error = function(e) -Inf)
    if (is.na(lp)) -Inf else lp
  }
  bridgesampling::bridge_sampler(
    chains, log_posterior = log_posterior, data = model,
    lb = stats::setNames(rep(-Inf, n_par), p),
    ub = stats::setNames(rep(Inf, n_par), p),
    repetitions = repetitions, method = method, cores = cores,
    use_neff = use_neff, maxiter = maxiter, silent = silent, verbose = verbose)
}

#' @rdname stanli-ecosystem
launch_shinystan.stanli_fit <- function(object, ...) {
  shinystan::launch_shinystan(as_stanfit(object), ...)
}
