# EXPERIMENTAL. Laplace whitening: a model over coordinates z with
# q = q_hat + P' L^-T z, where q is the original unconstrained vector.
# whiten() applies a given factor; laplace_whiten() finds q_hat and the factor
# for a model with random effects. The R builder follows the Python one in
# python/stanli/laplace.py and needs the Matrix package.

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

lw_general <- function(M)
  methods::as(methods::as(M, "CsparseMatrix"), "generalMatrix")

lw_triplets <- function(M)
  Matrix::mat2triplet(lw_general(M))

lw_sparse <- function(i, j, x, n, m = n)
  Matrix::sparseMatrix(i = i, j = j, x = x, dims = c(n, m))

lw_col_rows <- function(P, js) {
  start <- P@p[js]
  P@i[sequence(P@p[js + 1L] - start, start + 1L)] + 1L
}

lw_pattern <- function(H, tol) {
  n <- nrow(H)
  d <- sqrt(abs(diag(H)))
  fallback <- max(abs(H))
  ri <- list()
  rj <- list()
  for (start in seq.int(1L, n, by = 512L)) {
    rows <- start:min(n, start + 511L)
    scale <- outer(d[rows], d)
    scale[scale == 0] <- fallback
    hit <- which(abs(H[rows, , drop = FALSE]) > tol * scale, arr.ind = TRUE)
    ri[[length(ri) + 1L]] <- rows[hit[, 1L]]
    rj[[length(rj) + 1L]] <- hit[, 2L]
  }
  i <- c(unlist(ri), unlist(rj), seq_len(n))
  j <- c(unlist(rj), unlist(ri), seq_len(n))
  lw_sparse(i, j, rep(1, length(i)), n)
}

lw_colouring <- function(P) {
  n <- ncol(P)
  colour <- rep(-1L, n)
  for (j in order(-diff(P@p), seq_len(n))) {
    near <- lw_col_rows(P, lw_col_rows(P, j))
    used <- colour[near]
    used <- used[used >= 0L]
    colour[j] <- which(!(0:length(used) %in% used))[1L] - 1L
  }
  colour
}

lw_plan <- function(P, dense = integer(0)) {
  m <- ncol(P)
  degree <- diff(P@p)
  limit <- max(64L, as.integer(4 * sqrt(m)))
  dense <- sort(union(dense, which(degree > limit)))
  free <- setdiff(seq_len(m), dense)
  colour <- if (length(free)) lw_colouring(P[free, free, drop = FALSE]) else
    integer(0)
  n_colours <- if (length(free)) max(colour) + 1L else 0L
  list(pattern = P, dense = dense, free = free, colour = colour,
       n_colours = n_colours)
}

# Central-difference Hessian of the log density over the coordinates `cols`
# of q, one pair of gradients per colour of the pattern and per dense column.
lw_sparse_hessian <- function(grad, q, plan, cols) {
  m <- length(cols)
  P <- plan$pattern
  step <- 1e-4 * pmax(1, abs(q[cols]))
  in_free <- logical(m)
  in_free[plan$free] <- TRUE
  difference <- function(js) {
    e <- numeric(length(q))
    e[cols[js]] <- step[js]
    (grad(q + e)$grad - grad(q - e)$grad)[cols]
  }
  ti <- list()
  tj <- list()
  tx <- list()
  add <- function(i, j, x) {
    ti[[length(ti) + 1L]] <<- c(i, j)
    tj[[length(tj) + 1L]] <<- c(j, i)
    tx[[length(tx) + 1L]] <<- c(x, x)
  }
  for (colour in seq_len(plan$n_colours) - 1L) {
    js <- plan$free[plan$colour == colour]
    dg <- difference(js)
    rows <- lw_col_rows(P, js)
    cj <- rep(js, diff(P@p)[js])
    keep <- in_free[rows]
    rows <- rows[keep]
    cj <- cj[keep]
    add(rows, cj, 0.5 * dg[rows] / (2 * step[cj]))
  }
  for (h in plan$dense) {
    dg <- difference(h)
    rows <- seq_len(m)
    x <- dg / (2 * step[h])
    free_row <- in_free[rows]
    add(rows[free_row], rep(h, sum(free_row)), x[free_row])
    add(rows[!free_row], rep(h, sum(!free_row)), 0.5 * x[!free_row])
  }
  if (!length(ti)) return(Matrix::sparseMatrix(i = integer(0), j = integer(0),
                                                x = numeric(0), dims = c(m, m)))
  lw_sparse(unlist(ti), unlist(tj), unlist(tx), m)
}

lw_probe_groups <- 48L
lw_probe_rounds <- 4L
lw_probe_noise <- 1e3 * .Machine$double.eps

lw_with_seed <- function(seed, f) {
  old <- if (exists(".Random.seed", envir = globalenv()))
    get(".Random.seed", envir = globalenv())
  on.exit(if (is.null(old)) rm(".Random.seed", envir = globalenv()) else
    assign(".Random.seed", old, envir = globalenv()))
  set.seed(seed)
  f()
}

# Whether a forward difference of the gradient along random weights on the
# columns `js` moves each entry by more than rounding noise.
lw_probe_response <- function(grad, q, iu, g0, step, js) {
  w <- runif(length(js), 0.5, 1.5) * sample(c(-1, 1), length(js), TRUE)
  e <- numeric(length(q))
  e[iu[js]] <- step[js] * w
  gp <- grad(q + e)$grad[iu]
  abs(gp - g0) > lw_probe_noise * (abs(gp) + abs(g0))
}

# The (row, column) pairs that responded in every round, or NULL when there
# are more than `budget`.
lw_probe_intersect <- function(resps, labels, groups, budget) {
  rounds <- nrow(labels)
  by_group <- split(seq_len(ncol(labels)), factor(labels[1L, ], 0:(groups - 1L)))
  out_r <- list()
  out_c <- list()
  total <- 0
  for (g in seq_len(groups)) {
    cols <- by_group[[g]]
    rows <- which(resps[[1L]][g, ])
    if (!length(cols) || !length(rows)) next
    chunk <- max(1L, 4000000L %/% length(cols))
    for (start in seq(1L, length(rows), by = chunk)) {
      rr <- rows[start:min(length(rows), start + chunk - 1L)]
      keep <- matrix(TRUE, length(rr), length(cols))
      for (t in seq_len(rounds)[-1L]) {
        idx <- outer(rr, labels[t, cols] + 1L, function(r, l) l + groups * (r - 1))
        keep <- keep & matrix(resps[[t]][as.vector(idx)], length(rr))
      }
      hit <- which(keep, arr.ind = TRUE)
      total <- total + nrow(hit)
      if (!is.null(budget) && total > budget) return(NULL)
      out_r[[length(out_r) + 1L]] <- rr[hit[, 1L]]
      out_c[[length(out_c) + 1L]] <- cols[hit[, 2L]]
    }
  }
  list(i = unlist(out_r), j = unlist(out_c))
}

# A symmetric pattern with the diagonal that contains every nonzero of the
# Hessian block over `iu`: group probes with random weights, intersected over
# rounds, with groups four times as many whenever the candidates outgrow a
# budget and one column per probe (exact) once probes would outnumber columns.
lw_probe_candidates <- function(grad, q, iu, g0, step) {
  m <- length(iu)
  budget <- 32 * m + 100000
  groups <- lw_probe_groups
  repeat {
    if (groups * lw_probe_rounds >= m) {
      i <- list()
      j <- list()
      for (col in seq_len(m)) {
        rows <- which(lw_probe_response(grad, q, iu, g0, step, col))
        i[[col]] <- rows
        j[[col]] <- rep(col, length(rows))
      }
      pairs <- list(i = unlist(i), j = unlist(j))
      break
    }
    labels <- t(vapply(seq_len(lw_probe_rounds), function(r)
      sample(m) %% groups, numeric(m)))
    resps <- lapply(seq_len(lw_probe_rounds), function(r) {
      out <- matrix(FALSE, groups, m)
      for (g in seq_len(groups)) {
        js <- which(labels[r, ] == g - 1L)
        if (length(js)) out[g, ] <- lw_probe_response(grad, q, iu, g0, step, js)
      }
      out
    })
    pairs <- lw_probe_intersect(resps, labels, groups, budget)
    if (!is.null(pairs)) break
    groups <- groups * 4L
  }
  i <- c(pairs$i, pairs$j, seq_len(m))
  j <- c(pairs$j, pairs$i, seq_len(m))
  lw_sparse(i, j, rep(1, length(i)), m)
}

# Whether the coloured Hessian H predicts central differences of the gradient
# along three random directions to within what a missing entry of the size
# the pattern keeps would change.
lw_probe_agrees <- function(grad, q, iu, H, step, tol) {
  dH <- Matrix::diag(H)
  for (k in 1:3) {
    v <- step * rnorm(length(iu))
    e <- numeric(length(q))
    e[iu] <- v
    gp <- grad(q + e)$grad[iu]
    gm <- grad(q - e)$grad[iu]
    miss <- abs((gp - gm) - 2 * as.numeric(H %*% v))
    noise <- lw_probe_noise * (abs(gp) + abs(gm))
    signal <- 0.25 * tol * abs(dH) * sqrt(mean(v * v))
    if (any(miss > pmax(noise, signal))) return(FALSE)
  }
  TRUE
}

# lw_pattern for a sparse H.
lw_threshold_pattern <- function(H, tol) {
  m <- nrow(H)
  tr <- lw_triplets(H)
  d <- sqrt(abs(Matrix::diag(H)))
  scale <- d[tr$i] * d[tr$j]
  if (length(tr$x)) scale[scale == 0] <- max(abs(tr$x))
  keep <- abs(tr$x) > tol * scale
  i <- c(tr$i[keep], tr$j[keep], seq_len(m))
  j <- c(tr$j[keep], tr$i[keep], seq_len(m))
  lw_sparse(i, j, rep(1, length(i)), m)
}

# The pattern lw_pattern would give for the Hessian block over `iu` at q
# without the dense Hessian, or NULL when the verification fails.
lw_probe_pattern <- function(model, q, iu, tol, counter, seed) {
  grad <- function(x) {
    counter$n <- counter$n + 1L
    log_prob_grad(model, x)
  }
  lw_with_seed(seed, function() {
    g0 <- grad(q)$grad[iu]
    if (!all(is.finite(g0))) return(NULL)
    step <- 1e-4 * pmax(1, abs(q[iu]))
    C <- lw_probe_candidates(grad, q, iu, g0, step)
    H <- lw_sparse_hessian(grad, q, lw_plan(C), iu)
    if (!all(is.finite(H@x))) return(NULL)
    if (!lw_probe_agrees(grad, q, iu, H, step, tol)) return(NULL)
    lw_threshold_pattern(H, tol)
  })
}

lw_start_pattern <- function(model, q, iu, tol, mode, seed, counter) {
  if (mode == "probe") {
    P <- lw_probe_pattern(model, q, iu, tol, counter, seed)
    if (!is.null(P)) return(P)
    warning("laplace_whiten: the sparsity probe disagreed with a gradient ",
            "difference (or met a non-finite value); using the dense Hessian ",
            "pattern instead", call. = FALSE)
  }
  counter$n <- counter$n + 1L + 2L * length(q)
  H <- log_prob_hessian(model, q)$hessian
  if (!all(is.finite(H)))
    stop("the Hessian at the starting point is not finite", call. = FALSE)
  lw_pattern(H, tol)[iu, iu, drop = FALSE]
}

# The n by n pattern with `pattern_uu` on the `iu` block and only the diagonal
# elsewhere.
lw_embed_pattern <- function(pattern_uu, iu, n) {
  tr <- lw_triplets(pattern_uu)
  rest <- setdiff(seq_len(n), iu)
  lw_sparse(c(iu[tr$i], rest), c(iu[tr$j], rest), rep(1, length(tr$i) + length(rest)), n)
}

# Sparse LDL' of the symmetric matrix whose lower triangle is that of A:
# P A P' = L diag(d) L' with L unit lower triangular and P v = v[perm + 1].
lw_ldlt <- function(A, perm = NULL) {
  n <- nrow(A)
  A <- methods::as(methods::as(Matrix::tril(A), "generalMatrix"),
                   "CsparseMatrix")
  ptr <- .Call("stanli_r_sparse_factor_new", n, A@p, A@i, A@x, perm)
  f <- .Call("stanli_r_sparse_factor_get", ptr, n)
  list(ptr = ptr, n = n, perm = f$perm,
       L = Matrix::sparseMatrix(i = f$row_idx + 1, p = f$col_ptr, x = f$values,
                                dims = c(n, n)),
       d = f$d, n_negative = f$n_negative, log_abs_det = f$log_abs_det)
}

# (c, b, w) for diag(d) + W0 W0' with W0 = L^-1 P U, the form whiten(lowrank =)
# takes. U is n by k in the matrix's own ordering.
lw_lowrank <- function(f, U) {
  r <- .Call("stanli_r_sparse_factor_lowrank", f$ptr, U, f$n)
  k <- ncol(U)
  list(c = r$c, b = matrix(r$b, f$n, k, byrow = TRUE),
       w = matrix(r$w, f$n, k, byrow = TRUE))
}

lw_normal <- function(n, seed) {
  old <- if (exists(".Random.seed", envir = globalenv()))
    get(".Random.seed", envir = globalenv())
  on.exit(if (is.null(old)) rm(".Random.seed", envir = globalenv()) else
    assign(".Random.seed", old, envir = globalenv()))
  set.seed(seed)
  stats::rnorm(n)
}

# Ritz values and vectors of the symmetric operator `op` from m Lanczos steps
# with full reorthogonalization, and the residual norm of each.
lw_lanczos <- function(op, n, m, v0) {
  V <- matrix(0, n, m)
  alpha <- numeric(m)
  beta <- numeric(m)
  v <- v0 / sqrt(sum(v0^2))
  steps <- m
  for (j in seq_len(m)) {
    V[, j] <- v
    w <- op(v)
    alpha[j] <- sum(w * v)
    Vj <- V[, seq_len(j), drop = FALSE]
    for (pass in 1:2) w <- w - Vj %*% crossprod(Vj, w)
    beta[j] <- sqrt(sum(w^2))
    if (beta[j] <= 1e-13 * max(abs(alpha[seq_len(j)]), 1e-300)) {
      steps <- j
      beta[j] <- 0
      break
    }
    v <- as.numeric(w) / beta[j]
  }
  Tm <- diag(alpha[seq_len(steps)], steps)
  if (steps > 1L) {
    off <- cbind(seq_len(steps - 1L), seq_len(steps - 1L) + 1L)
    Tm[off] <- beta[seq_len(steps - 1L)]
    Tm[off[, 2:1, drop = FALSE]] <- beta[seq_len(steps - 1L)]
  }
  e <- eigen(Tm, symmetric = TRUE)
  list(values = e$values,
       vectors = V[, seq_len(steps), drop = FALSE] %*% e$vectors,
       residual = abs(beta[steps] * e$vectors[steps, ]), steps = steps)
}

lw_top_eigenvalue <- function(Q) {
  n <- nrow(Q)
  op <- function(v) as.numeric(Q %*% v)
  v0 <- lw_normal(n, 0L)
  for (m in c(40L, 160L, n)) {
    m <- min(m, n)
    r <- lw_lanczos(op, n, m, v0)
    if (r$residual[1L] <= 1e-4 * abs(r$values[1L]) || m == n)
      return(r$values[1L])
  }
}

# The eigenvalues of the symmetric sparse Q below `floor` times the largest
# absolute eigenvalue, with their eigenvectors, and the extreme eigenvalues.
# n_negative is the exact count of negative eigenvalues, which the search
# must reach.
lw_bad_eigenpairs <- function(Q, floor, n_negative) {
  n <- nrow(Q)
  if (n <= 64L) {
    e <- eigen(as.matrix(Q), symmetric = TRUE)
    w <- rev(e$values)
    V <- e$vectors[, n:1, drop = FALSE]
    bad <- w < floor * max(abs(w))
    return(list(w = w[bad], V = V[, bad, drop = FALSE], min = w[1L],
                max = w[n]))
  }
  top <- lw_top_eigenvalue(Q)
  sigma <- -floor * abs(top)
  shifted <- Q - sigma * Matrix::Diagonal(n)
  lu <- Matrix::lu(lw_general(shifted))
  op <- function(v) as.numeric(Matrix::solve(lu, v))
  k <- min(max(2L * n_negative, n_negative + 4L), n - 2L)
  m <- min(n, 2L * k + 20L)
  for (attempt in 1:8) {
    repeat {
      r <- lw_lanczos(op, n, m, lw_normal(n, attempt))
      by_size <- order(-abs(r$values))
      converged <- r$residual[by_size] <= 1e-9 * abs(r$values[by_size[1L]])
      ready <- if (all(converged)) length(converged) else
        which(!converged)[1L] - 1L
      if (ready >= k || m >= n) break
      m <- min(n, 2L * m)
    }
    take <- by_size[seq_len(min(k, ready))]
    w <- sigma + 1 / r$values[take]
    V <- r$vectors[, take, drop = FALSE]
    o <- order(w)
    w <- w[o]
    V <- V[, o, drop = FALSE]
    reach <- max(abs(1 / r$values[take]))
    lo <- floor * max(abs(top), abs(w[1L]))
    if (sum(w < 0) >= n_negative && (sigma + reach >= lo || k >= n - 2L)) {
      bad <- w < lo
      return(list(w = w[bad], V = V[, bad, drop = FALSE], min = w[1L],
                  max = top))
    }
    k <- min(2L * k, n - 2L)
    m <- min(n, max(m, 2L * k + 20L))
  }
  stop("could not find all ", n_negative, " negative eigenvalues of the ",
       "negative Hessian; use repair = \"abs_dense\"", call. = FALSE)
}

lw_try_grad <- function(model, q)
  tryCatch(log_prob_grad(model, q), error = function(e) NULL)

# Maximize the marginal log p(theta, u_hat(theta)) - log det(-H_uu) / 2 over
# theta: u_hat from an inner L-BFGS, H_uu from the coloured Hessian, theta from
# BFGS with forward-difference gradients.
lw_marginal_mode <- function(model, q_start, iu, it, pattern_uu, max_outer,
                             inner_iter, counter) {
  n <- length(q_start)
  grad <- function(q) {
    counter$n <- counter$n + 1L
    log_prob_grad(model, q)
  }
  plan <- lw_plan(pattern_uu)
  u_cache <- q_start[iu]
  inner <- function(theta) {
    q <- numeric(n)
    q[it] <- theta
    last <- list(u = NULL, value = NULL)
    at <- function(u) {
      if (!is.null(last$u) && identical(u, last$u)) return(last$value)
      qq <- q
      qq[iu] <- u
      counter$n <- counter$n + 1L
      r <- lw_try_grad(model, qq)
      value <- if (is.null(r) || !is.finite(r$lp) || abs(r$lp) > 1e100 ||
                    !all(is.finite(r$grad)) || max(abs(r$grad)) > 1e100)
        NULL else r
      last <<- list(u = u, value = value)
      value
    }
    fn <- function(u) {
      r <- at(u)
      if (is.null(r)) 1e300 else -r$lp
    }
    gr <- function(u) {
      r <- at(u)
      if (is.null(r)) numeric(length(u)) else -r$grad[iu]
    }
    r <- stats::optim(u_cache, fn, gr, method = "L-BFGS-B",
                      control = list(maxit = inner_iter, lmm = 20L,
                                     pgtol = 1e-8,
                                     factr = 1e-14 / .Machine$double.eps))
    q[iu] <- r$par
    list(q = q, lp = -r$value)
  }
  neg_marginal <- function(theta) {
    r <- inner(theta)
    if (!is.finite(r$lp) || r$lp < -1e299) return(1e300)
    u_cache <<- r$q[iu]
    H <- lw_sparse_hessian(grad, r$q, plan, iu)
    f <- tryCatch(lw_ldlt(-H), error = function(e) NULL)
    if (is.null(f) || f$n_negative > 0L) return(1e300)
    -(r$lp - 0.5 * f$log_abs_det)
  }
  theta0 <- q_start[it]
  outer <- list()
  theta <- theta0
  if (length(it)) {
    cache <- list(theta = NULL, value = NULL)
    fn <- function(theta) {
      if (!is.null(cache$theta) && identical(theta, cache$theta))
        return(cache$value)
      value <- neg_marginal(theta)
      cache <<- list(theta = theta, value = value)
      value
    }
    gr <- function(theta) {
      f0 <- fn(theta)
      vapply(seq_along(theta), function(i) {
        e <- numeric(length(theta))
        e[i] <- 1e-5
        (fn(theta + e) - f0) / 1e-5
      }, numeric(1))
    }
    r <- stats::optim(theta0, fn, gr, method = "BFGS",
                      control = list(maxit = max_outer, reltol = 1e-10))
    theta <- r$par
    outer <- list(iterations = unname(r$counts[2L]),
                  evaluations = unname(r$counts[1L]),
                  message = if (is.null(r$message)) "" else r$message)
  }
  list(q = inner(theta)$q, theta_start = theta0, theta = theta, outer = outer)
}

# The eigenvalues of Q below floor times the largest are raised ("clip") or
# replaced by their absolute values ("abs"), floored at that level. Dense.
lw_repair <- function(Q, floor, mode) {
  if (!inherits(try(chol(Q), silent = TRUE), "try-error"))
    return(list(Q = Q, count = 0L))
  e <- eigen(Q, symmetric = TRUE)
  w <- e$values
  if (mode == "abs") {
    lo <- floor * max(abs(w))
    w2 <- pmax(abs(w), lo)
    count <- sum(w2 != w)
  } else {
    lo <- floor * max(w)
    if (!(lo > 0))
      stop("the negative Hessian has no positive eigenvalue; the starting ",
           "point is not near a mode", call. = FALSE)
    w2 <- pmax(w, lo)
    count <- sum(w < lo)
  }
  R <- (e$vectors %*% (w2 * t(e$vectors)))
  list(Q = (R + t(R)) / 2, count = count, min = min(w), max = max(w))
}

lw_dense_whiten <- function(model, q_hat, Q, repair, clip, warn) {
  n <- nrow(Q)
  Qd <- as.matrix(Q)
  r <- lw_repair(Qd, clip, repair)
  if (r$count) warn(r$count, r$min, r$max)
  shifted <- (Qd != 0) * 1 + n * diag(n)
  perm <- lw_ldlt(methods::as(shifted, "CsparseMatrix"))$perm
  R <- chol(r$Q[perm + 1, perm + 1])
  L <- t(R)
  keep <- abs(L) > 1e-13 * max(abs(L))
  diag(keep) <- TRUE
  L <- methods::as(methods::as(L * keep, "CsparseMatrix"), "generalMatrix")
  list(model = whiten(model, q_hat, perm + 1, L), L = L, perm = perm + 1,
       ordering = "amd", n_clipped = r$count, min = r$min, max = r$max,
       rank = 0L)
}

lw_sparse_whiten <- function(model, q_hat, Q, floor, warn) {
  f <- tryCatch(lw_ldlt(Q), error = function(e)
    stop(conditionMessage(e), "; use repair = \"abs_dense\"", call. = FALSE))
  n <- nrow(Q)
  ratio <- if (floor > 0) floor else 1e-8
  fine <- f$n_negative == 0L && min(f$d) > ratio * max(f$d)
  n_clipped <- 0L
  min_w <- NULL
  max_w <- NULL
  rank <- 0L
  lowrank <- NULL
  scaled <- function() f$L %*% Matrix::Diagonal(x = sqrt(f$d))
  if (fine) {
    L <- scaled()
  } else {
    bad <- lw_bad_eigenpairs(Q, floor, f$n_negative)
    min_w <- bad$min
    max_w <- bad$max
    n_clipped <- length(bad$w)
    if (n_clipped) {
      lo <- floor * max(abs(min_w), abs(max_w))
      delta <- pmax(abs(bad$w), lo) - bad$w
      U <- bad$V %*% diag(sqrt(delta), n_clipped)
      warn(n_clipped, min_w, max_w)
      lowrank <- tryCatch(lw_lowrank(f, U), error = function(e)
        stop(conditionMessage(e), "; the unpivoted factorization of the ",
             "indefinite negative Hessian lost accuracy: use repair = ",
             "\"abs_dense\"", call. = FALSE))
      L <- f$L
      rank <- n_clipped
    } else {
      L <- scaled()
    }
  }
  L <- methods::as(methods::as(L, "CsparseMatrix"), "generalMatrix")
  list(model = whiten(model, q_hat, f$perm + 1, L, lowrank), L = L,
       perm = f$perm + 1, ordering = "amd", n_clipped = n_clipped,
       min = min_w, max = max_w, rank = rank)
}

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
#' The steps, with the defaults the Python builder and its benchmark use: start
#' at the Pathfinder mean (or `init`) and find the sparsity pattern of the
#' Hessian there; maximize over `theta` the marginal
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
#' @param seed Seed for Pathfinder and for the model.
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
  it <- setdiff(seq_len(n), iu)
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
  counter <- new.env()
  counter$n <- 0L
  pattern_uu <- lw_start_pattern(model, q_start, iu, pattern_tolerance, pattern,
                                 seed, counter)
  pattern_grads <- counter$n
  pattern_full <- lw_embed_pattern(pattern_uu, iu, n)
  counter$n <- 0L
  mode <- lw_marginal_mode(model, q_start, iu, it, pattern_uu, max_outer,
                           inner_iter, counter)
  q_hat <- mode$q
  marginal_grads <- counter$n

  if (hessian == "dense") {
    H <- log_prob_hessian(model, q_hat)$hessian
    final_grads <- 1L + 2L * n
    n_colours <- n
    if (!all(is.finite(H)))
      stop("the Hessian at the marginal mode is not finite", call. = FALSE)
    Q <- -(H + t(H)) / 2
    hit <- which(Q != 0, arr.ind = TRUE)
    tr <- list(i = hit[, 1L], j = hit[, 2L], x = Q[hit])
  } else {
    counter$n <- 0L
    grad <- function(q) {
      counter$n <- counter$n + 1L
      log_prob_grad(model, q)
    }
    plan <- lw_plan(pattern_full, it)
    H <- lw_sparse_hessian(grad, q_hat, plan, seq_len(n))
    final_grads <- counter$n
    n_colours <- plan$n_colours
    if (!all(is.finite(H@x)))
      stop("the Hessian at the marginal mode is not finite", call. = FALSE)
    tr <- lw_triplets(-(H + Matrix::t(H)) / 2)
  }
  on_diagonal <- tr$i == tr$j
  dn <- numeric(n)
  dn[tr$i[on_diagonal]] <- sqrt(abs(tr$x[on_diagonal]))
  keep <- abs(tr$x) >= threshold * dn[tr$i] * dn[tr$j]
  Q <- lw_sparse(tr$i[keep], tr$j[keep], tr$x[keep], n)

  warn <- function(count, min_w, max_w) {
    how <- if (repair == "clip") "clipped" else "replaced by absolute values"
    warning(sprintf(paste0(
      "laplace_whiten: %d of %d eigenvalues of the negative Hessian were %s ",
      "(floor %g times the largest; smallest %.3g, largest %.3g); the ",
      "whitened coordinates are only approximately standard normal"),
      count, n, how, clip, min_w, max_w), call. = FALSE)
  }
  w <- if (repair == "abs")
    lw_sparse_whiten(model, q_hat, Q, clip, warn) else
    lw_dense_whiten(model, q_hat, Q, if (repair == "clip") "clip" else "abs",
                    clip, warn)
  gradients <- list(pattern_hessian = pattern_grads,
                    marginal_mode = marginal_grads,
                    final_hessian = final_grads)
  gradients$total <- sum(unlist(gradients))
  list(model = w$model,
       report = list(n = n, n_latent = length(iu), n_theta = length(it),
                     latent_index = iu, q_hat = q_hat,
                     theta_hat = mode$theta, theta_start = mode$theta_start,
                     n_clipped = w$n_clipped, min_eigenvalue = w$min,
                     max_eigenvalue = w$max, nnz_L = length(w$L@x),
                     ordering = w$ordering, perm = w$perm, L = w$L,
                     gradients = gradients,
                     pathfinder_seconds = pathfinder_seconds,
                     wall_seconds = proc.time()[["elapsed"]] - t0,
                     outer = mode$outer, rank = w$rank,
                     n_colours = n_colours, hessian = hessian,
                     repair = repair))
}
