#' Build id of the stanli runtime
#'
#' The string a BridgeStan client's model payload must carry for the runtime
#' to accept it; see [bridgestan_data()].
#'
#' @return A single string.
#' @export
stanli_build_id <- function() {
  load_runtime()
  .Call("stanli_r_build_id")
}

json_string <- function(x) {
  x <- enc2utf8(x)
  x <- gsub("\\", "\\\\", x, fixed = TRUE)
  x <- gsub("\"", "\\\"", x, fixed = TRUE)
  x <- gsub("\n", "\\n", x, fixed = TRUE)
  x <- gsub("\r", "\\r", x, fixed = TRUE)
  x <- gsub("\t", "\\t", x, fixed = TRUE)
  if (grepl("[[:cntrl:]]", x)) {
    for (code in setdiff(1:31, c(9L, 10L, 13L))) {
      x <- gsub(rawToChar(as.raw(code)), sprintf("\\u%04x", code), x,
                fixed = TRUE)
    }
  }
  x <- paste0("\"", x, "\"")
  Encoding(x) <- "UTF-8"
  x
}

#' A model as BridgeStan data
#'
#' Stanli's runtime exposes BridgeStan's C interface, so a BridgeStan client
#' can sample a stanli model without a C++ toolchain. The client loads
#' [stanli_runtime_path()] as the model library and passes the string this
#' function returns as the model's data. The string is the model's data
#' object with one added key, `"__stanli"`, holding the runtime build id,
#' the compiled model and its name. The runtime refuses a payload whose
#' build id is not its own.
#'
#' @param model A `stanli_model`.
#' @return A single JSON string.
#' @seealso [sample_nutpie()] for a client that is wired up already.
#' @export
bridgestan_data <- function(model) {
  if (!inherits(model, "stanli_model"))
    stop("model must be a stanli_model", call. = FALSE)
  load_runtime()
  mir <- if (isTRUE(model$source$is_mir)) model$source$code else
    suppressWarnings(embedded_mir(model$source$code, character(),
                                  model$fast_math, NULL))
  manifest <- paste0("\"__stanli\": {\"build_id\": ",
                     json_string(stanli_build_id()), ", \"mir\": ",
                     json_string(mir), ", \"name\": ",
                     json_string(model$model_name), "}")
  data <- model$source$data_json
  open <- regexpr("^[[:space:]]*\\{", data)
  if (open < 0L)
    stop("the model's data is not a JSON object", call. = FALSE)
  head <- substr(data, 1L, attr(open, "match.length"))
  tail <- substring(data, attr(open, "match.length") + 1L)
  empty <- grepl("^[[:space:]]*\\}", tail)
  out <- paste0(head, manifest, if (!empty) ", ", tail)
  Encoding(out) <- "UTF-8"
  out
}

nutpie_installed <- function() requireNamespace("nutpieR", quietly = TRUE)

nutpie_reserved <- c("model", "data", "num_draws", "num_warmup", "num_chains",
                     "seed", "adaptation", "save_warmup", "store_unconstrained",
                     "pars", "include", "chain_format")

#' Sample with nutpie
#'
#' Runs the nutpie sampler (through the nutpieR package) on a stanli model.
#' nutpie drives the stanli runtime over its BridgeStan interface, so the
#' log density and gradient are stanli's. [sample_model()] stays the default
#' sampler; this is the alternative to reach for when nutpie's adaptation
#' suits a model better, for instance the low-rank mass matrix on a
#' posterior with strong correlations.
#'
#' nutpieR is not installed with stanli. Install it with
#' `install.packages("nutpieR", repos = c("https://andytimm.r-universe.dev",
#' "https://cloud.r-project.org"))`.
#'
#' The result is a `stanli_fit`, so [summary.stanli_fit()],
#' [stanli_diagnose()], [as_draws_array()], [log_lik()] and the conversions
#' to other fit classes work on it. Three things differ from a fit made by
#' [sample_model()]. `metric` and `inv_metric` are `NULL`, because nutpie's
#' mass matrix is not a Stan metric that another run can start from, and
#' `as_cstanfit(fit)$inv_metric()` says so. `report$warmup_seconds` is `NA`,
#' and `report$sampling_seconds` is the wall time of the whole run, warmup
#' included, since nutpie times the two together. `step_size` holds the
#' adapted step size of each chain, before nutpie's per-draw jitter.
#'
#' Transformed data that draws random numbers uses `seed`, as it does in
#' [sample_model()], and the fit's `model` is the model built under it.
#'
#' @param model A `stanli_model`.
#' @param chains,seed,samples Number of chains, seed, and post-warmup draws
#'   per chain.
#' @param warmup Warmup draws per chain. `NULL` uses nutpie's default.
#' @param adaptation `"diag"` for a diagonal mass matrix or `"low_rank"` for
#'   nutpie's low-rank-plus-diagonal one.
#' @param save_warmup Keep the warmup draws, placed before the post-warmup
#'   draws as in [sample_model()].
#' @param ... Further arguments for `nutpieR::nutpie_sample()`, such as
#'   `max_treedepth`, `target_accept` or `progress`. Progress output is off
#'   unless `progress` is given.
#' @return An object of class `stanli_fit` with `sampler_backend` set to
#'   `"nutpie"` and `adaptation` recording the choice.
#' @seealso [bridgestan_data()]
#' @export
sample_nutpie <- function(model, chains = 4, seed = 1, warmup = NULL,
                          samples = 1000, adaptation = c("diag", "low_rank"),
                          save_warmup = FALSE, ...) {
  if (!nutpie_installed())
    stop("sample_nutpie() needs the nutpieR package. Install it with ",
         "install.packages(\"nutpieR\", repos = c(",
         "\"https://andytimm.r-universe.dev\", ",
         "\"https://cloud.r-project.org\"))", call. = FALSE)
  if (!inherits(model, "stanli_model"))
    stop("model must be a stanli_model", call. = FALSE)
  extra <- list(...)
  clash <- intersect(names(extra), nutpie_reserved)
  if (length(clash))
    stop("sample_nutpie() sets ", paste(clash, collapse = ", "),
         " itself; remove ", if (length(clash) > 1L) "them" else "it",
         " from the call", call. = FALSE)
  adaptation <- match.arg(adaptation)
  cstan_integer(seed, "seed", 0)
  chains <- sampler_integer(chains, "chains")
  samples <- sampler_integer(samples, "samples")
  if (!is.null(warmup)) warmup <- sampler_integer(warmup, "warmup")
  stanfit_flag(save_warmup, "save_warmup")
  if (is.null(extra$progress)) extra$progress <- "none"
  load_runtime()
  model <- with_run_seed(model, seed)
  raw <- do.call(nutpieR::nutpie_sample,
                 c(list(stanli_runtime_path(), data = bridgestan_data(model),
                        num_draws = samples, num_warmup = warmup,
                        num_chains = chains, seed = seed,
                        adaptation = adaptation, save_warmup = save_warmup,
                        store_unconstrained = TRUE), extra))
  nutpie_fit_from(raw, model, seed, adaptation, save_warmup)
}

nutpie_array <- function(d, field, n, chains) {
  v <- d[[field]]
  if (is.null(v)) stop("nutpie did not report ", field, call. = FALSE)
  i <- order(d$chain, d$draw)
  if (is.matrix(v)) return(array(as.double(v[i, , drop = FALSE]),
                                 c(n, chains, ncol(v))))
  array(as.double(v[i]), c(n, chains, 1L))
}

nutpie_join <- function(pre, post) {
  if (is.null(pre)) return(post)
  out <- array(NA_real_, c(dim(pre)[1L] + dim(post)[1L], dim(post)[-1L]))
  out[seq_len(dim(pre)[1L]), , ] <- pre
  out[dim(pre)[1L] + seq_len(dim(post)[1L]), , ] <- post
  out
}

nutpie_config_number <- function(config, key, default) {
  hit <- regmatches(config, regexec(paste0("\"", key, "\":([0-9.eE+-]+)"),
                                    config))[[1L]]
  if (length(hit) == 2L) as.numeric(hit[2L]) else default
}

nutpie_fit_from <- function(raw, model, seed, adaptation, save_warmup) {
  nchain <- attr(raw, "num_chains")
  nsample <- attr(raw, "num_draws")
  columns <- model$columns
  names_in <- dimnames(raw)[[3L]]
  if (anyDuplicated(names_in) || !setequal(names_in, columns))
    stop("nutpie returned variables that differ from the model's columns: ",
         paste(setdiff(union(names_in, columns), intersect(names_in, columns)),
               collapse = ", "), call. = FALSE)
  pick <- match(columns, names_in)
  diag <- attr(raw, "diagnostics")
  warm <- warm_diag <- NULL
  nwarm <- 0L
  if (save_warmup) {
    warm <- attr(raw, "warmup_draws")
    warm_diag <- attr(raw, "warmup_diagnostics")
    nwarm <- dim(warm)[1L]
  }
  post <- function(field) nutpie_array(diag, field, nsample, nchain)
  post_matrix <- function(field) matrix(post(field), nsample, nchain)
  both <- function(field) nutpie_join(
    if (nwarm) nutpie_array(warm_diag, field, nwarm, nchain), post(field))

  draws <- nutpie_join(
    if (nwarm) unclass(warm)[, , pick, drop = FALSE],
    unclass(raw)[, , pick, drop = FALSE])
  dimnames(draws) <- list(NULL, NULL, columns)

  from <- c(lp__ = "logp", accept_stat__ = "mean_tree_accept",
            stepsize__ = "step_size", treedepth__ = "depth",
            n_leapfrog__ = "n_steps", divergent__ = "diverging",
            energy__ = "energy")
  names_out <- .Call("stanli_r_sampler_columns")
  if (!setequal(names_out, names(from)))
    stop("the runtime reports sampler columns this function does not map: ",
         paste(setdiff(names_out, names(from)), collapse = ", "), call. = FALSE)
  sampler <- array(NA_real_, c(nwarm + nsample, nchain, length(names_out)),
                   dimnames = list(NULL, NULL, names_out))
  for (name in names_out) sampler[, , name] <- both(from[[name]])[, , 1L]

  unconstrained <- both("unconstrained_draw")
  if (dim(unconstrained)[3L] != model$n_unconstrained)
    stop("nutpie returned ", dim(unconstrained)[3L], " unconstrained values ",
         "per draw but the model has ", model$n_unconstrained, call. = FALSE)

  config <- attr(raw, "sampler_config")
  step_size <- if (is.null(diag$step_size_bar))
    colMeans(post_matrix("step_size")) else post("step_size_bar")[nsample, , 1L]

  structure(list(
    draws = draws, sampler = sampler, unconstrained = unconstrained,
    columns = columns,
    max_depth = as.integer(nutpie_config_number(config, "maxdepth", 10)),
    seed = seed, model = model,
    report = list(
      warmup_seconds = rep(NA_real_, nchain),
      sampling_seconds = rep(as.numeric(attr(raw, "sampling_time")), nchain),
      n_divergent = colSums(post_matrix("diverging")),
      n_max_treedepth = colSums(post_matrix("maxdepth_reached"))),
    warmup_draws = as.integer(nwarm), warmup = as.integer(attr(raw, "num_warmup")),
    thin = 1L, samples = as.integer(nsample), save_warmup = save_warmup,
    chains = as.integer(nchain),
    delta = nutpie_config_number(config, "target_accept", 0.8),
    metric = NULL, step_size = as.numeric(step_size), inv_metric = NULL,
    adapt_engaged = TRUE, chain_id = 1L, sampler_backend = "nutpie",
    adaptation = adaptation),
    class = "stanli_fit")
}
