# Recover rectangular shapes from indexed column names, never from model source.
# Each group's coordinates must enumerate a complete array with its first
# index varying fastest. Zero-length declarations leave no columns to recover.
stanfit_column_layout <- function(columns) {
  if (!is.character(columns) || anyNA(columns) || anyDuplicated(columns))
    stop("columns must be unique, nonmissing Stan names", call. = FALSE)
  pattern <- "^([A-Za-z][A-Za-z0-9_]*)(\\[([1-9][0-9]*(,[1-9][0-9]*)*)\\])?$"
  if (any(!grepl(pattern, columns)))
    stop("invalid Stan column name", call. = FALSE)
  base <- sub("\\[.*$", "", columns)
  parameters <- unique(base)
  dims <- vector("list", length(parameters))
  names(dims) <- parameters
  cursor <- 1L
  for (parameter in parameters) {
    positions <- which(base == parameter)
    if (!identical(positions, seq.int(cursor, length.out = length(positions))))
      stop("columns for '", parameter, "' must be contiguous", call. = FALSE)
    cursor <- cursor + length(positions)
    names <- columns[positions]
    if (identical(names, parameter)) {
      dims[[parameter]] <- integer(0)
      next
    }
    if (parameter %in% names)
      stop("mixed scalar and indexed columns for '", parameter, "'", call. = FALSE)
    indices <- lapply(names, function(name) {
      text <- substr(name, nchar(parameter) + 2L, nchar(name) - 1L)
      suppressWarnings(as.integer(strsplit(text, ",", fixed = TRUE)[[1L]]))
    })
    rank <- lengths(indices)
    if (length(unique(rank)) != 1L || anyNA(unlist(indices)))
      stop("invalid index dimensions for '", parameter, "'", call. = FALSE)
    coordinates <- matrix(unlist(indices), nrow = length(positions), byrow = TRUE)
    shape <- as.integer(apply(coordinates, 2L, max))
    if (prod(shape) != length(positions))
      stop("incomplete rectangular columns for '", parameter, "'", call. = FALSE)
    stride <- 1
    for (axis in seq_along(shape)) {
      expected <- (seq_along(positions) - 1L) %/% stride %% shape[axis] + 1L
      if (!all(coordinates[, axis] == expected))
        stop("columns for '", parameter, "' are not in Stan column order",
             call. = FALSE)
      stride <- stride * shape[axis]
    }
    dims[[parameter]] <- shape
  }
  list(parameters = parameters, dimensions = dims)
}

# Keep this function in the namespace rather than capturing the conversion's
# evaluation frame, which contains the source fit and its runtime pointer.
stanfit_no_cppmodule <- function(...) {
  stop("as_stanfit() has no compiled RStan model", call. = FALSE)
}

#' Convert draws to an RStan fit without compilation
#'
#' Build a `stanli_stanfit`, an S4 subclass of `rstan::stanfit`, directly from
#' stored arrays. Requires optional rstan but never compiles a C++ model.
#'
#' @param x A fitted object. A `stanli_fit` must contain the sampling metadata
#'   recorded by the current [sample_model()]. A `stanli_stanfit` is returned
#'   unchanged unless a replacement `model` is supplied.
#' @param model A `stanli_model` made with the original code and data. Defaults
#'   to the model retained by a `stanli_fit`. Use `NULL` for a draws-only result.
#'   A replacement must match the fit's output columns and parameter count.
#' @param ... Reserved for methods.
#' @return An S4 object inheriting from `stanfit`, with samples, sampler
#'   statistics, timings, and parameter dimensions. Initial values and
#'   adaptation text are empty: the first saved draw is not the initial state.
#' @details Extraction, summaries, plots, and ordinary LOO inherit RStan's
#'   methods. With a live model, `rstan::log_prob`, `grad_log_prob`,
#'   `constrain_pars`, and `unconstrain_pars` call stanli directly.
#'   `get_num_upars` uses retained metadata and also works without a live model.
#'   LOO moment matching works with `cores = 1` and a live model.
#'
#'   Density calls support `adjust_transform = TRUE` (the default) and require
#'   an exact-lp runtime. `adjust_transform = FALSE` is rejected because the
#'   graph includes the parameter-transform Jacobian. Native RStan resampling
#'   and operations requiring its C++ model instance remain unsupported.
#'   `constrain_pars` includes transformed parameters and generated quantities;
#'   RNG-based quantities advance the retained model's RNG stream.
#'
#'   `saveRDS()` preserves draws, but external pointers do not survive loading.
#'   Recreate the model with the same code and data and call
#'   `as_stanfit(saved_fit, model = model)` to restore model-dependent methods.
#'   Serialized workers likewise cannot use live handles. No model recompiles
#'   automatically on loading or conversion.
#'
#'   Per-chain permutations use R's current RNG, as CSV import does. Set the
#'   same R seed before both conversions to compare permuted extraction.
#'   Stored draw order is unchanged. Empty variables cannot be inferred from
#'   column names and are omitted, as in CSV import. Packages that insist on
#'   exact class equality instead of checking inheritance may need adaptation.
#'
#'   Install rstan from a CRAN binary on supported macOS/Windows R versions to
#'   avoid a toolchain. On Linux, provision a compatible binary installation.
#' @examples
#' \dontrun{
#' m <- stanli_model(code = "parameters { real mu; } model { mu ~ normal(0, 1); }")
#' fit <- sample_model(m, refresh = 0)
#' sf <- as_stanfit(fit)
#' rstan::extract(sf, pars = "mu")
#' rstan::log_prob(sf, upars = 0, gradient = TRUE)
#' rstan::constrain_pars(sf, upars = 0)
#' }
#' @export
as_stanfit <- function(x, ...) UseMethod("as_stanfit")

#' @rdname as_stanfit
#' @export
as_stanfit.stanli_fit <- function(x, model = x$model, ...) {
  if (!requireNamespace("rstan", quietly = TRUE))
    stop("as_stanfit() needs rstan. Install a compatible binary with ",
         "install.packages('rstan', type = 'binary') on macOS/Windows; ",
         "use a provisioned binary on Linux.", call. = FALSE)
  register_stanfit_class()
  live_model <- model
  stanfit_check_model(live_model, x$columns, dim(x$unconstrained)[3L])
  required <- c("warmup", "thin", "samples", "save_warmup", "chains", "delta")
  if (!all(required %in% names(x)))
    stop("this fit lacks sampling metadata; create it with the current ",
         "sample_model() before using as_stanfit()", call. = FALSE)
  shape <- dim(x$draws)
  saved_warmup <- if (x$save_warmup) ceiling(x$warmup / x$thin) else 0L
  kept <- ceiling(x$samples / x$thin)
  if (length(shape) != 3L || shape[1L] != kept + saved_warmup ||
      shape[2L] != x$chains || shape[3L] != length(x$columns) ||
      !identical(dim(x$sampler)[1:2], shape[1:2]) || kept < 1L)
    stop("sampling metadata does not match the stored draws", call. = FALSE)
  layout <- stanfit_column_layout(c(x$columns, "lp__"))
  flat_names <- c(x$columns, "lp__")
  posterior_rows <- seq.int(saved_warmup + 1L, shape[1L])

  # Slot semantics checked against installed CRAN rstan 2.32.7 and
  # AllClass.R / stan_csv.R in the development source (no code reused):
  # https://github.com/stan-dev/rstan/tree/f070e44ec447b5caf1657f70d6c6e629ab43442c/rstan/rstan/R
  # stanfit-class.R reads each chain's sampler_params, elapsed_time, args,
  # mean_pars and mean_lp__ attributes. Samples use CSV dot names internally;
  # sim$fnames_oi uses bracket names. Diagnostics follow the CSV reader's order.
  sampler_names <- c("accept_stat__", "treedepth__", "stepsize__",
                     "divergent__", "n_leapfrog__", "energy__")
  chains <- vector("list", x$chains)
  arguments <- vector("list", x$chains)
  for (chain in seq_len(x$chains)) {
    values <- cbind(matrix(x$draws[, chain, ], nrow = shape[1L]),
                    x$sampler[, chain, "lp__"])
    colnames(values) <- flat_names
    draws <- as.data.frame(values, optional = TRUE)
    names(draws) <- gsub("\\[|,", ".", sub("\\]$", "", flat_names))
    diagnostics <- matrix(x$sampler[, chain, sampler_names], nrow = shape[1L])
    colnames(diagnostics) <- sampler_names
    attr(draws, "sampler_params") <- as.data.frame(diagnostics)
    attr(draws, "args") <- list(chain_id = chain, sampler_t = "NUTS(diag_e)")
    attr(draws, "adaptation_info") <- ""
    elapsed <- c(warmup = NA_real_, sample = NA_real_)
    if (isTRUE(x$report$available))
      elapsed[] <- c(x$report$warmup_seconds[chain], x$report$sampling_seconds[chain])
    attr(draws, "elapsed_time") <- elapsed
    averages <- colMeans(values[posterior_rows, , drop = FALSE])
    names(averages) <- names(draws)
    attr(draws, "mean_pars") <- utils::head(averages, -1L)
    attr(draws, "mean_lp__") <- averages["lp__"]
    chains[[chain]] <- draws
    arguments[[chain]] <- list(
      chain_id = chain, seed = x$seed, iter = x$warmup + x$samples,
      warmup = x$warmup, thin = x$thin, save_warmup = x$save_warmup,
      control = list(adapt_delta = x$delta, max_treedepth = x$max_depth),
      method = "sampling", algorithm = "NUTS", sampler_t = "NUTS(diag_e)"
    )
  }
  simulation <- list(
    samples = chains, chains = x$chains, iter = x$warmup + x$samples,
    warmup = x$warmup, thin = x$thin,
    n_save = rep(shape[1L], x$chains), warmup2 = rep(saved_warmup, x$chains),
    permutation = replicate(x$chains, sample.int(kept), simplify = FALSE),
    pars_oi = layout$parameters, dims_oi = layout$dimensions,
    fnames_oi = flat_names, n_flatnames = length(flat_names)
  )
  description <- if (is.null(live_model)) x$model else live_model
  name <- if (is.null(description$model_name)) "stanli_model" else description$model_name
  code <- if (is.null(description$model_code)) character(0) else description$model_code
  # cxxdso validity requires a nonempty signature list and the host system.
  # RStan handles remain absent, including .MISC$stan_fit_instance. Subclass
  # methods use the separately retained stanli model, never a fake Rcpp object.
  dso <- methods::new("cxxdso", sig = list(character(0)), dso_saved = FALSE,
                      system = R.version$system,
                      .CXXDSOMISC = new.env(parent = emptyenv()))
  model <- methods::new("stanmodel", model_name = name, model_code = code,
                        dso = dso, mk_cppmodule = stanfit_no_cppmodule)
  # A namespace parent makes serialized fits load RStan (and its registration
  # hook) when restored, without loading RStan during ordinary stanli startup.
  misc <- new.env(parent = asNamespace("rstan"))
  misc$stanli_model <- live_model
  misc$stanli_n_unconstrained <- dim(x$unconstrained)[3L]
  methods::new("stanli_stanfit", model_name = name, model_pars = layout$parameters,
               par_dims = layout$dimensions, sim = simulation, stan_args = arguments,
               stanmodel = model, mode = 0L, date = date(), inits = list(),
               .MISC = misc)
}

#' @rdname as_stanfit
#' @export
as_stanfit.stanli_stanfit <- function(x, model, ...) {
  if (missing(model)) return(x)
  columns <- x@sim$fnames_oi[x@sim$fnames_oi != "lp__"]
  stanfit_check_model(model, columns, x@.MISC$stanli_n_unconstrained)
  # Clone the environment: attaching a model must not change other copies of
  # the saved fit that still share its original .MISC environment.
  misc <- list2env(as.list(x@.MISC, all.names = TRUE), parent = asNamespace("rstan"))
  misc$stanli_model <- model
  x@.MISC <- misc
  x
}
