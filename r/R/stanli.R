# The user-facing API: compile a model, sample it, summarize it.

json_scalar <- function(x) {
  if (is.logical(x)) return(if (isTRUE(x)) "1" else "0")
  if (is.character(x)) return(paste0("\"", x, "\""))
  if (!is.finite(x)) stop("Stan data cannot contain NA, NaN or Inf",
                          call. = FALSE)
  format(x, digits = 17, scientific = FALSE, trim = TRUE)
}

# Stan's JSON data format, written without a JSON dependency: the values
# are numbers, arrays of numbers, and nested arrays, which is a small
# enough grammar to emit directly. Matrices go out ROW-major, as Stan's
# JSON reader expects an array of rows.
to_json <- function(x) {
  if (is.list(x)) {
    parts <- vapply(seq_along(x), function(i) {
      paste0("\"", names(x)[i], "\": ", to_json(x[[i]]))
    }, character(1))
    return(paste0("{", paste(parts, collapse = ", "), "}"))
  }
  if (is.matrix(x)) {
    rows <- apply(x, 1, function(r) paste0("[", paste(vapply(r, json_scalar,
      character(1)), collapse = ", "), "]"))
    return(paste0("[", paste(rows, collapse = ", "), "]"))
  }
  if (is.array(x) && length(dim(x)) > 2)
    stop("arrays with more than two dimensions are not supported by this ",
         "data writer yet; pass the model a flattened form", call. = FALSE)
  if (length(x) == 1 && is.null(dim(x))) return(json_scalar(x))
  paste0("[", paste(vapply(x, json_scalar, character(1)), collapse = ", "),
         "]")
}

#' Compile a Stan model
#'
#' @param file Path to a `.stan` file.
#' @param code Model source, as an alternative to `file`.
#' @param data A named list of data, or a path to a JSON data file.
#' @param mir Transformed MIR text, for a build without the embedded
#'   compiler. Rarely needed.
#' @return An object of class `stanli_model`.
#' @export
stanli_model <- function(file = NULL, code = NULL, data = NULL, mir = NULL) {
  load_runtime()
  if (is.null(code) && is.null(mir)) {
    if (is.null(file)) stop("provide file, code or mir", call. = FALSE)
    code <- paste(readLines(file, warn = FALSE), collapse = "\n")
  }
  data_json <- if (is.null(data)) {
    "{}"
  } else if (is.character(data) && length(data) == 1 && file.exists(data)) {
    paste(readLines(data, warn = FALSE), collapse = "\n")
  } else if (is.character(data) && length(data) == 1) {
    data
  } else {
    to_json(as.list(data))
  }
  is_mir <- !is.null(mir)
  if (!is_mir && !.Call("stanli_r_has_embedded_stanc")) {
    # A runtime without the embedded compiler needs the MIR handed to it.
    # The release builds embed stanc3; a source build and the Windows
    # runtime do not, and ship a stanc binary beside the library instead.
    mir <- stanc_mir(code)
    is_mir <- TRUE
  }
  ptr <- .Call("stanli_r_model_new", if (is_mir) mir else code, data_json,
               is_mir)
  structure(list(ptr = ptr,
                 n_unconstrained = .Call("stanli_r_n_unconstrained", ptr),
                 columns = .Call("stanli_r_column_names", ptr)),
            class = "stanli_model")
}

#' @export
print.stanli_model <- function(x, ...) {
  cat("<stanli model:", x$n_unconstrained, "unconstrained parameters,",
      length(x$columns), "columns>\n")
  invisible(x)
}

#' Log density and gradient
#'
#' @param model A `stanli_model`.
#' @param q A point on the unconstrained scale.
#' @return A list with `lp` and `grad`.
#' @export
log_prob_grad <- function(model, q) {
  .Call("stanli_r_grad", model$ptr, as.double(q))
}

#' Sample a model with NUTS
#'
#' Four chains by default, run in parallel: R-hat needs more than one
#' chain, and a single-chain run cannot be checked for convergence at
#' all. Chain `c` uses CmdStan's stream for `(seed, chain id c + 1)`.
#'
#' @param model A `stanli_model`.
#' @param chains,seed,warmup,samples,thin Sampler configuration.
#' @param delta Target acceptance statistic.
#' @param max_depth Maximum treedepth.
#' @param save_warmup Keep the warmup draws.
#' @param init Optional starting point on the UNCONSTRAINED scale: one
#'   vector shared by every chain, or a matrix with one row per chain.
#'   The unconstrained scale is what stanli can read -- a constrained
#'   init would need the inverse parameter transforms, which do not
#'   exist.
#' @param init_radius Random inits are drawn uniform(-r, r); 0 starts at
#'   the origin.
#' @param parallel_chains Chains to run at once. Defaults to all of them.
#' @return An object of class `stanli_fit`.
#' @export
sample_model <- function(model, chains = 4, seed = 1, warmup = 1000,
                         samples = 1000, thin = 1, delta = 0.8,
                         max_depth = 10, save_warmup = FALSE, init = NULL,
                         init_radius = 2, parallel_chains = NULL) {
  load_runtime()
  if (is.null(parallel_chains)) parallel_chains <- chains
  init_vec <- numeric(0)
  if (!is.null(init)) {
    m <- if (is.matrix(init)) init else
      matrix(rep(as.double(init), chains), nrow = chains, byrow = TRUE)
    if (!identical(dim(m), c(as.integer(chains),
                             as.integer(model$n_unconstrained))))
      stop("init must be a length-", model$n_unconstrained, " vector or a ",
           chains, " x ", model$n_unconstrained, " matrix", call. = FALSE)
    # The C side reads chain-major rows, so transpose before flattening.
    init_vec <- as.double(t(m))
  }
  opts <- list(as.integer(seed), as.integer(chains), as.integer(warmup),
               as.integer(samples), as.integer(thin), as.double(delta),
               as.integer(max_depth), isTRUE(save_warmup),
               as.double(init_radius), as.integer(parallel_chains))
  res <- .Call("stanli_r_sample", model$ptr, opts, init_vec)

  nchain <- res$chains
  ndraw <- res$draws
  ncol <- length(model$columns)
  # C fills chain-major, column-fastest; R arrays are column-major, so
  # the dims go (col, draw, chain) and are permuted to the (draw, chain,
  # variable) layout the posterior package uses.
  arr <- aperm(array(res$values, dim = c(ncol, ndraw, nchain)), c(2, 3, 1))
  dimnames(arr) <- list(NULL, NULL, model$columns)
  sc <- .Call("stanli_r_sampler_columns")
  sarr <- aperm(array(res$stats, dim = c(7L, ndraw, nchain)), c(2, 3, 1))
  dimnames(sarr) <- list(NULL, NULL, sc)
  uarr <- aperm(array(res$unconstrained,
                      dim = c(model$n_unconstrained, ndraw, nchain)),
                c(2, 3, 1))

  structure(list(draws = arr, sampler = sarr, unconstrained = uarr,
                 columns = model$columns, max_depth = max_depth, seed = seed,
                 model = model),
            class = "stanli_fit")
}

#' @export
print.stanli_fit <- function(x, ...) {
  d <- dim(x$draws)
  cat("<stanli fit:", d[2], "chains x", d[1], "draws,", d[3], "columns>\n")
  invisible(x)
}

#' @export
as.array.stanli_fit <- function(x, ...) x$draws

#' Posterior summary
#'
#' Mean, MCSE, sd, quantiles, bulk and tail effective sample size, and
#' rank-normalized split R-hat -- stan's own estimators, so the numbers
#' agree with `stansummary` rather than approximating it.
#'
#' @param object A `stanli_fit`.
#' @param ... Unused.
#' @return A data frame, one row per column.
#' @export
summary.stanli_fit <- function(object, ...) {
  d <- dim(object$draws)
  # Back to the packed (col, draw, chain) order the C side reads.
  flat <- as.double(aperm(object$draws, c(3, 1, 2)))
  st <- .Call("stanli_r_summary", flat,
              as.integer(c(d[2], d[1], d[3])))
  m <- matrix(st, nrow = d[3], byrow = TRUE)
  out <- data.frame(variable = object$columns, mean = m[, 1],
                    mcse_mean = m[, 2], sd = m[, 3], mcse_sd = m[, 4],
                    q5 = m[, 5], q50 = m[, 6], q95 = m[, 7],
                    ess_bulk = m[, 8], ess_tail = m[, 9], rhat = m[, 10],
                    stringsAsFactors = FALSE)
  rownames(out) <- NULL
  out
}

#' Convergence diagnostics
#'
#' Divergent transitions, treedepth saturation, E-BFMI, R-hat and
#' bulk/tail effective sample size -- each either confirmed or reported
#' with the number that failed and what to do about it.
#'
#' @param fit A `stanli_fit`.
#' @return The report, invisibly, after printing it.
#' @export
stanli_diagnose <- function(fit) {
  d <- dim(fit$draws)
  flat <- as.double(aperm(fit$draws, c(3, 1, 2)))
  stats <- as.double(aperm(fit$sampler, c(3, 1, 2)))
  txt <- .Call("stanli_r_diagnose", flat, as.integer(c(d[2], d[1], d[3])),
               fit$columns, stats, as.integer(fit$max_depth))
  cat(txt)
  invisible(txt)
}

#' Find the posterior mode by L-BFGS
#'
#' Returns the posterior MODE. CmdStan's `optimize` defaults to
#' `jacobian=0`, the penalized maximum likelihood, which stanli cannot
#' offer: the change-of-variables Jacobian is folded into the graph when
#' the model is lowered.
#'
#' @param model A `stanli_model`.
#' @param seed,iter Optimizer configuration.
#' @param init Optional start on the unconstrained scale.
#' @param init_radius Random start radius.
#' @return A list with `values` (named), `unconstrained`, and `lp`.
#' @export
optimize_model <- function(model, seed = 1, iter = 2000, init = NULL,
                           init_radius = 2) {
  load_runtime()
  opts <- list(as.integer(seed), as.integer(iter), TRUE,
               as.double(init_radius))
  init_vec <- if (is.null(init)) numeric(0) else as.double(init)
  r <- .Call("stanli_r_optimize", model$ptr, opts, init_vec)
  names(r$values) <- model$columns
  r
}
