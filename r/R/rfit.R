# A native, RStan-shaped view of a fit. It owns its class and methods; it does
# not register RStan's S4 classes or require that package to be installed.

#' Native R fit interface
#'
#' Give a Stanli fit an RStan-style summary and extraction interface without
#' loading RStan. The arrays, live model, and ecosystem integrations are kept.
#' Existing `stanli_fit` summary behavior is unchanged until explicitly converted.
#'
#' @param x,object A `stanli_fit` or `stanli_rfit`.
#' @param pars Parameter names or indexed element names. Defaults to all outputs,
#'   including `lp__`. Base names select the complete vector or array.
#' @param permuted Merge post-warmup chains using stored per-chain permutations.
#' @param inc_warmup Include saved warmup. Ignored for permuted extraction.
#' @param include Select `pars` when TRUE, or exclude them when FALSE.
#' @param probs Quantile probabilities.
#' @param ... Reserved; unsupported arguments produce an error.
#' @details `extract()` returns a named list of draw-first arrays when permuted,
#'   or an iteration-by-chain-by-parameter array otherwise. `as_rfit()` creates
#'   per-chain permutations using R's RNG; extraction from the converted fit
#'   does not advance that RNG.
#'   All parameters share the same permutations, preserving joint draws.
#'
#'   `summary()` returns `summary` and `c_summary` matrices/arrays in RStan's
#'   layout. `n_eff` and `Rhat` use posterior's basic ESS and split R-hat;
#'   these are distinct from Stanli's usual bulk ESS and rank-normalized R-hat.
#'   posterior's ESS algorithm also differs from RStan's legacy ESS estimator,
#'   so `n_eff` and `se_mean` are not numerically identical to RStan's summary.
#'   Summary requires optional posterior but no native runtime. Sampler and
#'   timing extraction require neither posterior nor a native runtime.
#'
#'   This is a native S3 class, not an RStan S4 object. Calls explicitly written
#'   as `rstan::extract()` still require RStan. Use `stanli::extract()` instead.
#'   Packages inspecting RStan slots or requiring its C++ instance need an
#'   integration or the optional [as_stanfit()] conversion. The live model pointer
#'   does not survive serialization; stored-draw operations do.
#' @return `as_rfit()` returns a `stanli_rfit` inheriting from `stanli_fit`.
#'   `get_sampler_params()` returns one matrix per chain, excluding `lp__`.
#'   `get_elapsed_time()` returns a chain-by-phase matrix in seconds.
#' @export
as_rfit <- function(x) {
  if (inherits(x, "stanli_rfit")) return(x)
  if (!inherits(x, "stanli_fit")) stop("x must be a stanli_fit", call. = FALSE)
  if (is.null(x$warmup_draws))
    stop("fit lacks warmup metadata; sample with the current Stanli version",
         call. = FALSE)
  d <- dim(x$draws)
  ds <- dim(x$sampler)
  warmup <- x$warmup_draws
  if (length(d) != 3L || length(ds) != 3L || !identical(d[1:2], ds[1:2]) ||
      !identical(dimnames(x$draws)[[3L]], x$columns) ||
      !"lp__" %in% dimnames(x$sampler)[[3L]] ||
      length(warmup) != 1L || !is.numeric(warmup) || !is.finite(warmup) ||
      warmup < 0 || warmup != floor(warmup) || warmup >= d[1L])
    stop("fit arrays and warmup metadata do not agree", call. = FALSE)
  n <- length(post_warmup_rows(x))
  if (n < 1L) stop("fit has no post-warmup draws", call. = FALSE)
  stanfit_column_layout(c(x$columns, "lp__"))
  x$rfit_permutation <- replicate(dim(x$draws)[2L], sample.int(n), simplify = FALSE)
  class(x) <- c("stanli_rfit", class(x))
  x
}

rfit_array <- function(object, inc_warmup = FALSE) {
  if (!inherits(object, "stanli_fit"))
    stop("object must be a stanli_fit", call. = FALSE)
  rows <- post_warmup_rows(object, inc_warmup)
  d <- dim(object$draws)
  arr <- array(c(object$draws, object$sampler[, , "lp__", drop = FALSE]),
               dim = c(d[1:2], d[3] + 1L),
               dimnames = list(iterations = NULL,
                               chains = paste0("chain:", seq_len(d[2])),
                               parameters = c(object$columns, "lp__")))
  arr[rows, , , drop = FALSE]
}

rfit_variables <- function(columns, pars, include = TRUE) {
  if (is.null(pars)) return(unique(sub("\\[.*$", "", columns)))
  if (!is.character(pars) || anyNA(pars) || anyDuplicated(pars))
    stop("pars must contain unique parameter names", call. = FALSE)
  selected <- select_draw_variables(columns, pars)
  if (include) pars else {
    # Preserve intact containers, returning indexed elements for partial exclusions.
    remaining <- setdiff(seq_along(columns), selected)
    bases <- unique(sub("\\[.*$", "", columns[remaining]))
    unlist(lapply(bases, function(base) {
      full <- select_draw_variables(columns, base)
      if (all(full %in% remaining)) base else columns[intersect(full, remaining)]
    }), use.names = FALSE)
  }
}

#' @rdname as_rfit
#' @export
extract <- function(object, pars = NULL, permuted = TRUE, inc_warmup = FALSE,
                    include = TRUE, ...) {
  if (length(list(...))) stop("unsupported extract arguments", call. = FALSE)
  for (flag in c("permuted", "inc_warmup", "include")) stanfit_flag(get(flag), flag)
  if (permuted && !inherits(object, "stanli_rfit")) object <- as_rfit(object)
  arr <- rfit_array(object, inc_warmup = !permuted && inc_warmup)
  columns <- dimnames(arr)[[3L]]
  pars <- rfit_variables(columns, pars, include)
  if (!permuted)
    return(arr[, , select_draw_variables(columns, pars), drop = FALSE])
  layout <- stanfit_column_layout(columns)
  n <- dim(arr)[1L]
  chain_rows <- unlist(lapply(seq_len(dim(arr)[2L]), function(chain)
    object$rfit_permutation[[chain]] + (chain - 1L) * n), use.names = FALSE)
  flat <- matrix(arr, nrow = n * dim(arr)[2L])
  out <- lapply(pars, function(par) {
    index <- select_draw_variables(columns, par)
    shape <- if (par %in% names(layout$dimensions)) layout$dimensions[[par]] else integer(0)
    value <- array(flat[chain_rows, index, drop = FALSE], dim = c(length(chain_rows), shape))
    dimnames(value) <- c(list(iterations = NULL), rep(list(NULL), length(shape)))
    value
  })
  names(out) <- pars
  out
}

#' @rdname as_rfit
#' @export
summary.stanli_rfit <- function(object, pars = NULL,
                                probs = c(0.025, 0.25, 0.5, 0.75, 0.975), ...) {
  if (length(list(...))) stop("unsupported summary arguments", call. = FALSE)
  if (!requireNamespace("posterior", quietly = TRUE))
    stop("summary needs the posterior package", call. = FALSE)
  if (!is.numeric(probs) || anyNA(probs) || any(probs < 0 | probs > 1))
    stop("probs must be probabilities between 0 and 1", call. = FALSE)
  arr <- extract(object, pars = pars, permuted = FALSE)
  # RStan prints matrix/array elements in row-major order in summaries,
  # while extraction retains Stan's column-major serialization order.
  columns <- dimnames(arr)[[3L]]
  layout <- stanfit_column_layout(c(object$columns, "lp__"))
  indices <- unlist(lapply(unique(sub("\\[.*$", "", columns)), function(par) {
    selected <- select_draw_variables(columns, par)
    shape <- layout$dimensions[[par]]
    if (length(shape) > 1L) {
      full <- c(object$columns, "lp__")
      names <- full[select_draw_variables(full, par)]
      printed <- as.vector(aperm(array(names, shape), rev(seq_along(shape))))
      selected <- selected[order(match(columns[selected], printed))]
    }
    selected
  }), use.names = FALSE)
  arr <- arr[, , indices, drop = FALSE]
  labels <- names(stats::quantile(0, probs = probs))
  single <- function(x) c(mean = mean(x), sd = stats::sd(x), stats::quantile(x, probs))
  result <- matrix(NA_real_, nrow = dim(arr)[3L], ncol = 5L + length(probs),
                   dimnames = list(dimnames(arr)[[3L]],
                     c("mean", "se_mean", "sd", labels, "n_eff", "Rhat")))
  chain_result <- array(NA_real_, dim = c(dim(arr)[3L], 2L + length(probs), dim(arr)[2L]),
                        dimnames = list(dimnames(arr)[[3L]], c("mean", "sd", labels),
                                        dimnames(arr)[[2L]]))
  for (j in seq_len(dim(arr)[3L])) {
    draws <- matrix(arr[, , j], nrow = dim(arr)[1L])
    ess <- posterior::ess_basic(draws)
    sd <- stats::sd(as.numeric(draws))
    result[j, ] <- c(mean(draws), sd / sqrt(ess), sd,
                     stats::quantile(draws, probs), ess, posterior::rhat_basic(draws))
    for (chain in seq_len(dim(arr)[2L])) chain_result[j, , chain] <- single(draws[, chain])
  }
  list(summary = result, c_summary = chain_result)
}

#' @rdname as_rfit
#' @export
get_sampler_params <- function(object, inc_warmup = TRUE) {
  stanfit_flag(inc_warmup, "inc_warmup")
  rows <- post_warmup_rows(object, inc_warmup)
  pars <- c("accept_stat__", "treedepth__", "stepsize__", "divergent__",
            "n_leapfrog__", "energy__")
  lapply(seq_len(dim(object$sampler)[2L]), function(chain) {
    matrix(object$sampler[rows, chain, pars, drop = FALSE], nrow = length(rows),
           dimnames = list(NULL, pars))
  })
}

#' @rdname as_rfit
#' @export
get_elapsed_time <- function(object) {
  out <- matrix(NA_real_, nrow = dim(object$draws)[2L], ncol = 2L,
                 dimnames = list(paste0("chain:", seq_len(dim(object$draws)[2L])),
                                 c("warmup", "sample")))
  if (isTRUE(object$report$available)) {
    out[, "warmup"] <- object$report$warmup_seconds
    out[, "sample"] <- object$report$sampling_seconds
  }
  out
}
