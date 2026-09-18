# The user-facing API: compile a model, sample it, summarize it.

json_scalar <- function(x) {
  if (is.character(x)) return(paste0("\"", x, "\""))
  if (is.logical(x) && !is.na(x)) return(if (x) "1" else "0")
  if (is.na(x) && !is.double(x))
    stop("Stan integer data cannot contain NA", call. = FALSE)
  if (is.na(x)) return("\"NaN\"")
  if (is.infinite(x)) return(if (x > 0) "\"Infinity\"" else "\"-Infinity\"")
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

read_utf8_file <- function(path) {
  size <- file.info(path)$size
  if (is.na(size))
    stop("could not read UTF-8 file: ", path, call. = FALSE)
  value <- rawToChar(readBin(path, "raw", n = size))
  Encoding(value) <- "UTF-8"
  value
}

#' Compile a Stan model
#'
#' @param file Path to a `.stan` file.
#' @param code Model source, as an alternative to `file`.
#' @param data A named list of data, or a path to a JSON data file.
#' @param mir Transformed MIR text, for a build without the embedded
#'   compiler. Rarely needed.
#' @param threads_per_chain Positive thread count for eligible native `reduce_sum`
#'   calls. Defaults to 1. Sampling uses its own setting; direct gradients use
#'   the model's setting. Requires a thread-safe runtime for values above 1.
#' @param seed The model-construction seed: RNG calls in transformed data
#'   draw from it once, here, the way CmdStan's generated constructor does.
#'   [sample_model()] and [optimize_model()] forward their own `seed` here
#'   when transformed data drew from it, rebuilding the model for that run,
#'   so one seed governs a whole run as it does in CmdStan. The rebuilt
#'   model is the one the fit carries as `fit$model`; the object passed in
#'   is unchanged. Models are also rebuilt when sampling changes
#'   `threads_per_chain`.
#' @return An object of class `stanli_model` whose `columns` name every
#'   output the way the posterior package reads them, `theta[1,2]` for an
#'   indexed value. Warns, naming the part and the
#'   reason, when part of the model has no compiled path and runs through
#'   the much slower MIR interpreter; with the environment variable
#'   `STANLI_NO_INTERPRETER` set that is an error instead. Either message is
#'   what to include in a bug report.
#' @export
stanli_model <- function(file = NULL, code = NULL, data = NULL, mir = NULL,
                         seed = 1, threads_per_chain = 1) {
  cstan_integer(threads_per_chain, "threads_per_chain")
  load_runtime()
  if (is.null(code) && is.null(mir)) {
    if (is.null(file)) stop("provide file, code or mir", call. = FALSE)
    code <- read_utf8_file(file)
  }
  data_json <- if (is.null(data)) {
    "{}"
  } else if (is.character(data) && length(data) == 1 && file.exists(data)) {
    read_utf8_file(data)
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
  model <- build_model(if (is_mir) mir else code, data_json, is_mir, seed,
    model_name = if (is.null(file)) "stanli_model" else
      tools::file_path_sans_ext(basename(file)),
    model_code = if (is.null(code)) character(0) else code,
    threads_per_chain = threads_per_chain)
  note <- .Call("stanli_r_warnings", model$ptr)
  if (nzchar(note)) warning(note, call. = FALSE)
  model
}

# The whole object, from its source: everything a `stanli_model` carries
# besides the source is derived from the handle, so a rebuild under another
# seed must recompute all of it. Transformed data can size a parameter
# (`int k = poisson_rng(3);` then `vector[k] mu;`), which changes the free
# vector length and the columns along with the draws.
build_model <- function(code, data_json, is_mir, seed,
                        model_name = "stanli_model", model_code = character(0),
                        threads_per_chain = 1) {
  ptr <- .Call("stanli_r_model_new", code, data_json, is_mir,
               as.numeric(seed), as.integer(threads_per_chain))
  structure(list(ptr = ptr,
                 n_unconstrained = .Call("stanli_r_n_unconstrained", ptr),
                 columns = stan_variable_names(
                   .Call("stanli_r_column_names", ptr)),
                 model_name = model_name,
                 model_code = model_code,
                 source = list(code = code, data_json = data_json,
                               is_mir = is_mir),
                 seed = seed, threads_per_chain = threads_per_chain,
                 reduce_sum_count = .Call("stanli_r_reduce_sum_count", ptr),
                 reduce_sum_fallbacks = strsplit(
                   .Call("stanli_r_reduce_sum_fallbacks", ptr), "\n", fixed = TRUE)[[1L]]),
            class = "stanli_model")
}

# CmdStan builds the model under the run seed, so transformed data that
# draws from it follows that seed. Rebuild for this run only when a draw
# happened and the seed differs, or when the thread setting changes.
# The rebuilt model is what the run uses and what the fit carries as
# `fit$model`; the caller's own object is a value and keeps its seed.
with_run_seed <- function(model, seed, threads_per_chain = model$threads_per_chain) {
  if (!.Call("stanli_r_transformed_data_rng", model$ptr)) seed <- model$seed
  if (seed == model$seed && threads_per_chain == model$threads_per_chain)
    return(model)
  build_model(model$source$code, model$source$data_json,
              model$source$is_mir, seed, model$model_name, model$model_code,
              threads_per_chain)
}

stan_variable_names <- function(x) {
  vapply(strsplit(x, ".", fixed = TRUE), function(parts) {
    if (length(parts) > 1L && all(grepl("^[0-9]+$", parts[-1L])))
      paste0(parts[1L], "[", paste(parts[-1L], collapse = ","), "]")
    else
      paste(parts, collapse = ".")
  }, character(1))
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

#' Starting values on the constrained scale, as the free vector
#'
#' Every declared parameter must appear, at its declared size. A missing,
#' unknown, wrong-length, or out-of-support value is an error naming the
#' parameter. Containers are listed in Stan's own serialization order (the
#' first index fastest, the order a CSV column carries) and may be nested or
#' flat -- the declaration owns the shape either way.
#'
#' The result is what `sample_model(init = )` and `optimize_model(init = )`
#' take, so unconstraining is a step per starting point rather than a second
#' kind of argument.
#'
#' @param model A `stanli_model`.
#' @param values A named list of constrained starting values, or a JSON
#'   string in CmdStan's data format.
#' @return A numeric vector of length `model$n_unconstrained`.
#' @export
unconstrain <- function(model, values) {
  json <- if (is.character(values) && length(values) == 1) {
    values
  } else {
    to_json(values)
  }
  .Call("stanli_r_unconstrain_inits", model$ptr, json)
}

#' Sample a model with NUTS
#'
#' Four chains by default, run in parallel: R-hat needs more than one
#' chain, and a single-chain run cannot be checked for convergence at
#' all. Chain `c` uses CmdStan's stream for `(seed, chain id c + 1)`.
#'
#' @param model A `stanli_model`.
#' @param chains,seed,warmup,samples,thin Sampler configuration. `seed` is
#'   also the model-construction seed for this run when transformed data
#'   draws from it; see [stanli_model()].
#' @param delta Target acceptance statistic.
#' @param max_depth Maximum treedepth.
#' @param save_warmup Keep the warmup draws.
#' @param init Optional starting point on the UNCONSTRAINED scale: one
#'   vector shared by every chain, or a matrix with one row per chain.
#'   Start from constrained values by passing them through
#'   [unconstrain()] first -- one scale here means one contract for what
#'   a start is.
#' @param init_radius Random inits are drawn uniform(-r, r); 0 starts at
#'   the origin.
#' @param pathfinder_init Optional named list enabling Pathfinder-generated
#'   starts. An empty list uses defaults; supported entries are
#'   `num_iterations` (1000), `num_elbo_draws` (25), `history_size` (5), and
#'   Pathfinder's own `init_radius` (2). Uses `seed`, returns one start per
#'   chain, and cannot be combined with `init`. Single-path Pathfinder does
#'   not perform PSIS resampling.
#' @param threads_per_chain Positive thread count per chain for eligible native
#'   `reduce_sum` calls, default 1. Small or unsupported calls stay serial.
#'   Changing this setting prepares a new graph; reduction rounding and NUTS
#'   draws can differ across thread counts. Active chains may each use this
#'   many threads. `fit$model$reduce_sum_count` and `reduce_sum_fallbacks`
#'   report retained reductions and graph-lowering refusals (not opaque regions).
#' @param parallel_chains Chains to run at once. Defaults to all of them.
#' @param refresh Print a progress update every `refresh` transitions within
#'   each phase, plus the first and last transition of the phase. Set to 0 to
#'   suppress all automatic sampling output.
#' @details Interrupting R (Ctrl-C, or the stop button in RStudio) stops
#'   every chain after its current transition and raises the usual
#'   interrupt; no fit is returned.
#' @return An object of class `stanli_fit`. Its `report` element contains
#'   per-chain warmup and sampling times plus exact divergence and
#'   maximum-treedepth counts. With a compatible older runtime that predates
#'   progress reporting, `report$available` is `FALSE` and those values are
#'   `NA`. Sampling metadata (`warmup`, `samples`, `thin`, `save_warmup`,
#'   `chains`, and `delta`) records the configuration for downstream conversion;
#'   iteration counts are before thinning.
#' @export
sample_model <- function(model, chains = 4, seed = 1, warmup = 1000,
                         samples = 1000, thin = 1, delta = 0.8,
                         max_depth = 10, save_warmup = FALSE, init = NULL,
                         init_radius = 2, pathfinder_init = NULL,
                         parallel_chains = NULL, refresh = 100,
                         threads_per_chain = 1) {
  cstan_integer(threads_per_chain, "threads_per_chain")
  if (length(refresh) != 1L || !is.numeric(refresh) || is.na(refresh) ||
      !is.finite(refresh) || refresh < 0 || refresh != floor(refresh) ||
      refresh > .Machine$integer.max)
    stop("refresh must be a single nonnegative integer", call. = FALSE)
  refresh <- as.integer(refresh)
  if (!is.null(init) && !is.null(pathfinder_init))
    stop("init and pathfinder_init are mutually exclusive", call. = FALSE)
  if (!is.null(pathfinder_init))
    pathfinder_init <- .pathfinder_init_options(pathfinder_init)
  if (!is.null(pathfinder_init) &&
      (length(chains) != 1L || !is.numeric(chains) || is.logical(chains) ||
       is.na(chains) || !is.finite(chains) || chains <= 0 ||
       chains != floor(chains) || chains > .Machine$integer.max))
    stop("chains must be a positive integer with Pathfinder initialization",
         call. = FALSE)
  load_runtime()
  model <- with_run_seed(model, seed, threads_per_chain)
  if (is.null(parallel_chains)) parallel_chains <- chains
  if (!is.null(pathfinder_init)) {
    init <- .Call("stanli_r_pathfinder_inits", model$ptr, as.integer(seed),
                  as.integer(chains), unname(pathfinder_init))
  }
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
  res <- .Call("stanli_r_sample", model$ptr, opts, init_vec, refresh)
  if (isTRUE(res$interrupted)) {
    signalCondition(structure(class = c("interrupt", "condition"),
                              list(message = "sampling interrupted",
                                   call = NULL)))
    invokeRestart("abort")
  }

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

  report <- list(available = res$report_available,
                 warmup_seconds = res$warmup_seconds,
                 sampling_seconds = res$sampling_seconds,
                 n_divergent = res$n_divergent,
                 n_max_treedepth = res$n_max_treedepth)
  structure(list(draws = arr, sampler = sarr, unconstrained = uarr,
                 columns = model$columns, max_depth = max_depth, seed = seed,
                 model = model, report = report,
                 warmup_draws = if (isTRUE(save_warmup))
                   as.integer(ceiling(warmup / max(1, thin))) else 0L,
                 warmup = as.integer(warmup), thin = as.integer(max(1, thin)),
                 samples = as.integer(samples), save_warmup = isTRUE(save_warmup),
                 chains = as.integer(nchain), delta = as.double(delta)),
            class = "stanli_fit")
}

.pathfinder_init_options <- function(x) {
  if (!is.list(x))
    stop("pathfinder_init must be a named list of options", call. = FALSE)
  defaults <- list(num_iterations = 1000L, num_elbo_draws = 25L,
                   history_size = 5L, init_radius = 2)
  if (length(x) > 0L) {
    if (is.null(names(x)) || any(!nzchar(names(x))) || anyDuplicated(names(x)))
      stop("pathfinder_init must be a named list of options", call. = FALSE)
    unknown <- setdiff(names(x), names(defaults))
    if (length(unknown) > 0L)
      stop("unknown pathfinder_init option", if (length(unknown) > 1L) "s" else "",
           ": ", paste(unknown, collapse = ", "), call. = FALSE)
  }
  out <- utils::modifyList(defaults, x)
  for (name in c("num_iterations", "num_elbo_draws", "history_size")) {
    value <- out[[name]]
    if (length(value) != 1L || !is.numeric(value) || is.logical(value) ||
        is.na(value) || !is.finite(value) || value <= 0 ||
        value != floor(value) || value > .Machine$integer.max)
      stop("pathfinder_init ", name, " must be a positive integer",
           call. = FALSE)
    out[[name]] <- as.integer(value)
  }
  radius <- out$init_radius
  if (length(radius) != 1L || !is.numeric(radius) || is.logical(radius) ||
      is.na(radius) || !is.finite(radius) || radius < 0)
    stop("pathfinder_init init_radius must be finite and nonnegative",
         call. = FALSE)
  out$init_radius <- as.double(radius)
  out
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
#' @param init Optional start on the unconstrained scale; see
#'   [unconstrain()] to build one from constrained values.
#' @param init_radius Random start radius.
#' @return A list with `values` (named), `unconstrained`, and `lp`.
#' @export
optimize_model <- function(model, seed = 1, iter = 2000, init = NULL,
                           init_radius = 2) {
  load_runtime()
  model <- with_run_seed(model, seed)
  opts <- list(as.integer(seed), as.integer(iter), TRUE,
               as.double(init_radius))
  init_vec <- if (is.null(init)) numeric(0) else as.double(init)
  r <- .Call("stanli_r_optimize", model$ptr, opts, init_vec)
  names(r$values) <- model$columns
  r
}
