# EXPERIMENTAL. Laplace whitening: a model over coordinates z with
# q = q_hat + P' L^-T z, where q is the original unconstrained vector.
# whiten() applies a given factor; laplace_whiten() finds q_hat and the factor
# for a model with random effects, with the runtime's builder
# (stanli_laplace_whiten), and needs the Matrix package.

require_matrix <- function() {
  if (!requireNamespace("Matrix", quietly = TRUE))
    stop("this needs the Matrix package: install.packages(\"Matrix\")",
         call. = FALSE)
}

apply_whitening <- function(ptr, w) {
  low <- w$lowrank
  .Call("stanli_r_model_whiten", ptr, w$q_hat, w$perm, w$col_ptr, w$row_idx,
        w$values, low$c, low$b, low$w)
}

# A lower triangular L as 0-based CSC arrays: a Matrix, or list(p, i, x).
csc_arrays <- function(L, n) {
  if (inherits(L, "Matrix")) {
    require_matrix()
    if (!identical(dim(L), c(n, n)))
      stop("L is ", nrow(L), " by ", ncol(L), ", expected ", n, " by ", n,
           call. = FALSE)
    L <- methods::as(methods::as(methods::as(L, "CsparseMatrix"),
                                 "generalMatrix"), "dMatrix")
    L <- Matrix::drop0(L)
    return(list(p = L@p, i = L@i, x = L@x))
  }
  if (!is.list(L) || !all(c("p", "i", "x") %in% names(L)))
    stop("L must be a Matrix or a list with CSC arrays p, i and x (0-based)",
         call. = FALSE)
  list(p = L$p, i = L$i, x = as.double(L$x))
}

lowrank_arrays <- function(lowrank, n) {
  if (is.null(lowrank)) return(NULL)
  ok <- is.list(lowrank) && all(c("c", "b", "w") %in% names(lowrank))
  if (ok) {
    b <- as.matrix(lowrank$b)
    w <- as.matrix(lowrank$w)
    ok <- length(lowrank$c) == n && nrow(b) == n && identical(dim(b), dim(w)) &&
      ncol(b) >= 1L
  }
  if (!ok)
    stop("lowrank must be list(c, b, w) with c of length ", n,
         " and b and w n by k matrices", call. = FALSE)
  list(c = as.double(lowrank$c), b = as.double(t(b)), w = as.double(t(w)))
}

# Each experimental feature says so once per session, through message() so
# suppressMessages() silences it.
experimental_state <- new.env(parent = emptyenv())

experimental_notice <- function(feature, advice) {
  if (isTRUE(experimental_state[[feature]])) return(invisible())
  assign(feature, TRUE, envir = experimental_state)
  message(feature, " is experimental. ", advice)
}

laplace_whitening_notice <- function() {
  experimental_notice("Laplace whitening", paste(
    "A whitened fit can be wrong without any sampler warning, so compare it",
    "against one fit with the default sampler. See docs/laplace-whitening.md."))
}

#' Whiten a model around a point (experimental)
#'
#' A new model over coordinates `z` with `q = q_hat + P' L^-T z`, where `q` is
#' the unconstrained vector of `model`, `(P v)[i] = v[perm[i]]`, and `L` is a
#' lower triangular factor with `P Q P' = L L'`. `Q` is usually the negative
#' Hessian of the log density at `q_hat`, so `z` is close to standard normal.
#' The log density is that of `model` at `q(z)` with the constant `log|det L|`
#' dropped. Every algorithm runs on `z` unchanged; sample with
#' `metric = "unit_e"`. Columns, constrained names and output rows are those of
#' `model`. [unconstrain()] returns `z`, and unconstrained inits, modes and
#' Hessians are in `z`. The result does not depend on `model` afterwards, and
#' it survives the rebuild a run with a new seed does.
#'
#' @param model A `stanli_model`.
#' @param q_hat The point to whiten around, on the unconstrained scale.
#' @param perm A permutation of `1:n`.
#' @param L A lower triangular factor with a nonzero diagonal: a `Matrix`
#'   (the Matrix package must be installed), or a list with the 0-based CSC
#'   arrays `p`, `i` and `x`.
#' @param lowrank `NULL`, or `list(c, b, w)` with `c` of length `n` and `b`
#'   and `w` `n` by `k` matrices. It inserts the factor
#'   `diag(c) + tril(w b', -1)`, so that with `L` unit lower triangular and
#'   `C C' = D + W W'` the precision is `P' L (D + W W') L' P`. See
#'   [laplace_whiten()].
#' @return A `stanli_model`.
#' @export
whiten <- function(model, q_hat, perm, L, lowrank = NULL) {
  laplace_whitening_notice()
  if (!inherits(model, "stanli_model"))
    stop("model must be a stanli_model", call. = FALSE)
  if (!is.null(model$whitening))
    stop("this model is already whitened; whiten the original model",
         call. = FALSE)
  n <- model$n_unconstrained
  if (!is.numeric(q_hat) || length(q_hat) != n)
    stop("q_hat must be a numeric vector of length ", n, call. = FALSE)
  if (!is.numeric(perm) || length(perm) != n || !setequal(perm, seq_len(n)))
    stop("perm must be a permutation of 1:", n, call. = FALSE)
  arrays <- csc_arrays(L, n)
  w <- list(q_hat = as.double(q_hat), perm = as.double(perm) - 1,
            col_ptr = as.double(arrays$p), row_idx = as.double(arrays$i),
            values = as.double(arrays$x),
            lowrank = lowrank_arrays(lowrank, n))
  model$ptr <- apply_whitening(model$ptr, w)
  model$whitening <- w
  model
}

#' Annotations on a model's parameters
#'
#' The declaration annotations on each parameter, without the `@`:
#' `@latent vector[J] a;` gives `list(a = "latent")`. Only parameters with at
#' least one annotation are listed, in source order. Transformed parameters and
#' generated quantities are not listed; an annotation on one has no effect.
#'
#' @param model A `stanli_model`.
#' @return A named list of character vectors.
#' @export
parameter_annotations <- function(model) {
  if (!inherits(model, "stanli_model"))
    stop("model must be a stanli_model", call. = FALSE)
  a <- .Call("stanli_r_parameter_annotations", model$ptr)
  keep <- lengths(a$annotations) > 0L
  stats::setNames(a$annotations[keep], a$name[keep])
}

# ---- builder ----------------------------------------------------------------

lw_latent_index <- function(model, latent) {
  if (!is.character(latent) || !length(latent) || anyNA(latent))
    stop("latent must be a character vector of parameter names",
         call. = FALSE)
  if (anyDuplicated(latent))
    stop("latent names '", latent[anyDuplicated(latent)], "' twice",
         call. = FALSE)
  blocks <- .Call("stanli_r_param_blocks", model$ptr)
  if (!length(blocks$name))
    stop("this model is already whitened; build the whitened model from the ",
         "original one", call. = FALSE)
  stop_at <- cumsum(blocks$size)
  start <- stop_at - blocks$size + 1L
  for (name in latent) {
    if (name %in% blocks$name) next
    what <- if (name %in% sub("\\[.*", "", model$columns))
      "is a transformed parameter or generated quantity, not a parameter"
    else "is not a parameter of this model (data and undeclared names are not parameters)"
    stop("latent '", name, "' ", what, "; the parameters are ",
         paste0("'", blocks$name, "'", collapse = ", "), call. = FALSE)
  }
  k <- match(latent, blocks$name)
  sort(unlist(lapply(k, function(i) seq.int(start[i], length.out = blocks$size[i]))))
}

lw_pathfinder_mean <- function(model, seed, n) {
  num_draws <- max(1000L, min(20L * n, 20000L, 25000000L %/% n))
  opts <- unname(.pathfinder_init_options(list()))
  draws <- .Call("stanli_r_pathfinder_inits", model$ptr, as.integer(seed),
                 as.integer(num_draws), opts, 1L)
  algorithm_interrupt(draws, "Pathfinder")
  draws <- draws[apply(is.finite(draws), 1L, all), , drop = FALSE]
  if (!nrow(draws))
    stop("Pathfinder returned no finite draws; pass init to choose the ",
         "starting point", call. = FALSE)
  colMeans(draws)
}

#' Laplace-whiten a model (experimental)
#'
#' Finds the Laplace-approximate marginal mode of the hyperparameters and
#' whitens `model` around it. `latent` names the parameters that make up the
#' random effects `u`; every other parameter is a hyperparameter `theta`. In
#' the unconstrained vector the latent block is those parameters' entries, so a
#' simplex of K entries contributes K - 1. With `latent = NULL` the latent
#' parameters are the ones declared `@latent` (see [parameter_annotations()]),
#' and a model with none is an error. A `latent` argument wins when both are
#' present.
#'
#' The runtime's builder, which Python's `laplace_whiten` also calls, does the
#' work. The steps, with the defaults its benchmark used: start at the
#' Pathfinder mean (or `init`) and find the sparsity pattern of the Hessian
#' there; maximize over `theta` the marginal
#' `log p(theta, u_hat(theta)) - log det(-H_uu) / 2` by L-BFGS, with `H_uu`
#' from central differences of the gradient along the pattern; at the result
#' take `Q = -H`, drop entries below `threshold` relative to
#' `sqrt(Q_ii Q_jj)` and factor it with a sparse LDL'. If `Q` is not
#' positive definite, `repair = "abs"` replaces each eigenvalue below `clip`
#' (default 1e-8) times the largest absolute eigenvalue by its absolute value,
#' floored at that level, as a low-rank correction to the sparse factor, and
#' warns. `"abs_dense"` does it with a dense eigendecomposition and Cholesky,
#' and `"clip"` raises eigenvalues below `clip` (default 1e-3) times the
#' largest to that value; both are O(n^3).
#'
#' Run the whitened model with `metric = "unit_e"` and `adapt_metric = FALSE`
#' (step-size adaptation only); a short warmup is enough. Needs the Matrix
#' package.
#'
#' @param model A `stanli_model`.
#' @param latent Names of the latent parameters, or `NULL`.
#' @param init An unconstrained starting point that replaces Pathfinder.
#' @param seed Seed for Pathfinder, for the model and for the pattern probes.
#' @param max_outer,inner_iter L-BFGS iteration limits for `theta` and `u`.
#' @param pattern_tolerance,threshold Relative thresholds for the Hessian
#'   pattern and for the entries of `Q`.
#' @param repair,clip How a `Q` that is not positive definite is repaired.
#' @param hessian `"coloured"` (default) or `"dense"`, which calls
#'   [log_prob_hessian()] at the mode.
#' @param pattern `"probe"` (default) finds the pattern from a few dozen
#'   gradients and checks it against three random gradient differences,
#'   falling back to the dense Hessian with a warning if the check fails;
#'   `"dense"` calls [log_prob_hessian()] at the start point.
#' @return A list with `model`, the whitened model, and `report`: counts of
#'   gradient evaluations by stage, `theta_hat`, the fill of the factor
#'   (`nnz_L`), the repaired eigenvalue count (`n_clipped`) and `rank`, and
#'   timings. `perm` and `latent_index` are 1-based.
#' @export
laplace_whiten <- function(model, latent = NULL, init = NULL, seed = 1,
                           max_outer = 60L, inner_iter = 2000L,
                           pattern_tolerance = 1e-6, threshold = 1e-6,
                           repair = c("abs", "abs_dense", "clip"), clip = NULL,
                           hessian = c("coloured", "dense"),
                           pattern = c("probe", "dense")) {
  laplace_whitening_notice()
  require_matrix()
  if (!inherits(model, "stanli_model"))
    stop("model must be a stanli_model", call. = FALSE)
  repair <- match.arg(repair)
  hessian <- match.arg(hessian)
  pattern <- match.arg(pattern)
  if (is.null(clip)) clip <- if (repair == "clip") 1e-3 else 1e-8
  if (is.null(latent)) {
    notes <- parameter_annotations(model)
    latent <- names(notes)[vapply(notes, function(a) "latent" %in% a,
                                  logical(1))]
    if (!length(latent))
      stop("no parameter is annotated @latent; pass latent = c(...) or ",
           "declare the random effects with @latent, for example ",
           "'@latent vector[J] a;'", call. = FALSE)
  }
  t0 <- proc.time()[["elapsed"]]
  iu <- lw_latent_index(model, latent)
  model <- with_run_seed(model, seed)
  n <- model$n_unconstrained
  pathfinder_seconds <- 0
  if (is.null(init)) {
    t <- proc.time()[["elapsed"]]
    q_start <- lw_pathfinder_mean(model, seed, n)
    pathfinder_seconds <- proc.time()[["elapsed"]] - t
  } else {
    if (!is.numeric(init) || length(init) != n)
      stop("init must be a numeric vector of length ", n, call. = FALSE)
    q_start <- as.double(init)
  }
  options <- list(as.integer(seed), as.integer(max_outer),
                  as.integer(inner_iter), as.double(pattern_tolerance),
                  as.double(threshold),
                  match(repair, c("abs", "abs_dense", "clip")) - 1L,
                  as.double(clip), pattern == "dense", hessian == "dense")
  r <- .Call("stanli_r_laplace_whiten", model$ptr, q_start, iu - 1, options)
  algorithm_interrupt(r, "laplace_whiten")
  for (w in r$warnings) warning(w, call. = FALSE)
  L <- Matrix::sparseMatrix(i = r$row_idx + 1, p = r$col_ptr, x = r$values,
                            dims = c(n, n))
  lowrank <- if (r$rank > 0L)
    list(c = r$c, b = matrix(r$b, n, r$rank, byrow = TRUE),
         w = matrix(r$w, n, r$rank, byrow = TRUE))
  gradients <- list(pattern_hessian = r$pattern_gradients,
                    marginal_mode = r$marginal_gradients,
                    final_hessian = r$final_gradients)
  gradients$total <- sum(unlist(gradients))
  outer <- if (length(r$theta_hat))
    list(iterations = r$outer_iterations, evaluations = r$outer_evaluations,
         message = r$outer_message) else list()
  spectrum <- r$has_spectrum
  list(model = whiten(model, r$q_hat, r$perm + 1, L, lowrank),
       report = list(n = n, n_latent = length(iu), n_theta = n - length(iu),
                     latent_index = iu, q_hat = r$q_hat,
                     theta_hat = r$theta_hat, theta_start = r$theta_start,
                     n_clipped = r$n_clipped,
                     min_eigenvalue = if (spectrum) r$min_eigenvalue,
                     max_eigenvalue = if (spectrum) r$max_eigenvalue,
                     nnz_L = length(L@x), ordering = "amd", perm = r$perm + 1,
                     L = L, gradients = gradients,
                     pathfinder_seconds = pathfinder_seconds,
                     wall_seconds = proc.time()[["elapsed"]] - t0,
                     outer = outer, rank = r$rank, n_colours = r$n_colours,
                     hessian = hessian, repair = repair))
}
