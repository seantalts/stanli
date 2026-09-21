#' Native CmdStanR-style model interface
#'
#' Retain Stan source in a native model object. Its `$sample()` method accepts
#' the arguments of [sample_cstan()] (except `model_code`) and returns the same
#' native fit. Model preparation happens when sampling, using the supplied data
#' and seed, including RNG calls in transformed data.
#'
#' @param model_code A single string of Stan program source.
#' @param include_paths Directories searched for Stan `#include` files; see
#'   [stanli_model()]. Resolved when the model object is created; files are read
#'   when `$sample()` prepares the model.
#' @return A `stanli_cstanmodel` with `$sample(...)`, `$code()`, and
#'   `$model_name()` methods. `$sample()` accepts `threads_per_chain` for native
#'   within-chain parallelism and `parallel_chains` for concurrent chains.
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
cstan_model <- function(model_code, include_paths = NULL) {
  if (!is.character(model_code) || length(model_code) != 1L ||
      is.na(model_code) || !nzchar(trimws(model_code)))
    stop("model_code must be a single nonempty string of Stan source", call. = FALSE)
  paths <- if (!is.null(include_paths) || grepl("#include", model_code, fixed = TRUE))
    stan_include_paths(include_paths) else NULL
  structure(list(
    code = function() model_code,
    model_name = function() "stanli_model",
    sample = function(..., include_paths = paths) {
      if (is.null(include_paths)) sample_cstan(model_code = model_code, ...) else
        sample_cstan(model_code = model_code, include_paths = include_paths, ...)
    }
  ), class = "stanli_cstanmodel")
}

#' @export
print.stanli_cstanmodel <- function(x, ...) {
  cat("<stanli CmdStanR-style model:", x$model_name(), ">\n")
  invisible(x)
}
