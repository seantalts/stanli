# The draws interface brms and the rest of the Stan R stack consume.

#' Convert a fit to a draws array
#'
#' Returns an object the posterior package understands: a
#' `draws_array` with dimensions (iteration, chain, variable). Without
#' posterior installed this is still a plain array with the same
#' dimensions and dimnames, which is what most consumers actually read.
#'
#' @param x A `stanli_fit`.
#' @param include_sampler Append the seven sampler columns (`lp__`,
#'   `accept_stat__`, ...), which is what a CSV reader would see.
#' @return A `draws_array`, or a plain array when posterior is absent.
#' @export
as_draws_array <- function(x, include_sampler = FALSE) {
  arr <- x$draws
  if (include_sampler) {
    arr <- array(c(x$sampler, x$draws),
                 dim = c(dim(x$draws)[1], dim(x$draws)[2],
                         dim(x$sampler)[3] + dim(x$draws)[3]))
    dimnames(arr) <- list(NULL, NULL,
                          c(dimnames(x$sampler)[[3]], x$columns))
  }
  # Naming the dim attribute DROPS dimnames, so the variable names have
  # to be put back afterwards -- otherwise every consumer sees an
  # unlabelled array and the columns become positional.
  vars <- dimnames(arr)[[3]]
  names(dim(arr)) <- c("iteration", "chain", "variable")
  dimnames(arr) <- list(iteration = NULL, chain = NULL, variable = vars)
  if (requireNamespace("posterior", quietly = TRUE)) {
    return(posterior::as_draws_array(arr))
  }
  arr
}
