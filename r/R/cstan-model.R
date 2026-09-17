#' Native CmdStanR-style model interface
#'
#' Retain Stan source in a native model object. Its `$sample()` method accepts
#' the arguments of [sample_cstan()] (except `model_code`) and returns the same
#' native fit. Model preparation happens when sampling, using the supplied data
#' and seed, including RNG calls in transformed data.
#'
#' @param model_code A single string of Stan program source.
#' @return A `stanli_cstanmodel` with `$sample(...)`, `$code()`, and
#'   `$model_name()` methods. `$sample()` also accepts `threads_per_chain = 1`;
#'   larger values are unsupported. Use `parallel_chains` for concurrency.
#' @details Defaults and validation are delegated to [sample_cstan()]. Neither
#'   cmdstanr, rstan, CmdStan, nor a C++ toolchain is needed. This object provides
#'   the sampling subset of the CmdStanR model interface, with its own class;
#'   it does not provide compilation methods, compiler options, or CSV output.
#'   Unsupported sampling arguments produce an error before model preparation.
#' @examples
#' model <- cstan_model("parameters { real a; } model { a ~ normal(0, 1); }")
#' model$code()
#' if (stanli_available()) model$sample(chains = 1, iter_warmup = 20,
#'                                    iter_sampling = 20, seed = 1, refresh = 0)
#' @md
#' @export
cstan_model <- function(model_code) {
  if (!is.character(model_code) || length(model_code) != 1L ||
      is.na(model_code) || !nzchar(trimws(model_code)))
    stop("model_code must be a single nonempty string of Stan source", call. = FALSE)
  structure(list(
    code = function() model_code,
    model_name = function() "stanli_model",
    sample = function(..., threads_per_chain = 1) {
      cstan_integer(threads_per_chain, "threads_per_chain")
      if (threads_per_chain != 1)
        stop("within-chain threading is not supported; use parallel_chains", call. = FALSE)
      sample_cstan(model_code = model_code, ...)
    }
  ), class = "stanli_cstanmodel")
}

#' @export
print.stanli_cstanmodel <- function(x, ...) {
  cat("<stanli CmdStanR-style model:", x$model_name(), ">\n")
  invisible(x)
}
