# Declaration-aware Stan JSON writer, shared by corpus tooling.
# Derived from this repository's gen_brms_models.R (BSD-3-Clause).
decl_ndim <- function(code) {
  lines <- strsplit(code, "\n", fixed = TRUE)[[1]]
  i0 <- grep("^\\s*data\\s*\\{", lines)[1]
  if (is.na(i0)) return(list())
  depth <- 0; out <- list()
  for (i in seq(i0, length(lines))) {
    l <- lines[i]
    depth <- depth + lengths(regmatches(l, gregexpr("{", l, fixed = TRUE))) -
                     lengths(regmatches(l, gregexpr("}", l, fixed = TRUE)))
    if (i > i0 && depth <= 0) break
    l <- sub("//.*$", "", l)
    if (!grepl(";", l)) next
    d <- trimws(sub(";.*$", "", l))
    if (d == "" || grepl("^(data|transformed|parameters|model)", d)) next
    # name is the last identifier before ; (ignoring any = default)
    d2 <- sub("=.*$", "", gsub("<[^>]*>", "", d))
    nm <- regmatches(d2, regexpr("[A-Za-z_][A-Za-z0-9_]*\\s*$", d2))
    if (length(nm) == 0) next
    nm <- trimws(nm)
    ty <- substr(d2, 1, nchar(d2) - nchar(nm))
    n <- 0
    arr <- regmatches(ty, regexpr("^\\s*array\\s*\\[[^]]*\\]", ty))
    if (length(arr) == 1) {
      n <- n + 1 + lengths(regmatches(arr, gregexpr(",", arr, fixed = TRUE)))
      ty <- sub("^\\s*array\\s*\\[[^]]*\\]", "", ty)
    }
    if (grepl("matrix|cov_matrix|corr_matrix|cholesky_factor", ty)) n <- n + 2
    else if (grepl("vector|simplex|ordered|unit_vector", ty)) n <- n + 1
    out[[nm]] <- n
  }
  out
}

num <- function(x) {
  if (is.logical(x)) x <- as.integer(x)
  if (length(x) == 0) return("null")
  if (is.na(x) && !is.nan(x)) return("NaN")          # NA -> NaN token
  if (is.nan(x)) return("NaN")
  if (is.infinite(x)) return(if (x > 0) "Infinity" else "-Infinity")
  if (is.integer(x)) return(format(x, scientific = FALSE, trim = TRUE))
  if (x == round(x) && abs(x) < 1e15)
    return(format(x, digits = 17, scientific = FALSE, trim = TRUE))
  format(x, digits = 17, scientific = TRUE, trim = TRUE)
}

emit <- function(x, ndim) {
  d <- dim(x)
  if (!is.null(d) && length(d) >= 2) {
    # row-major nesting over the leading index
    rows <- lapply(seq_len(d[1]), function(i) {
      slice <- do.call("[", c(list(x, i), rep(list(TRUE), length(d) - 1L),
                             list(drop = FALSE)))
      sub <- array(slice, dim = d[-1])
      emit(sub, length(d) - 1)
    })
    return(paste0("[", paste(unlist(rows), collapse = ","), "]"))
  }
  x <- as.vector(x)
  if (ndim <= 0 && length(x) == 1) return(num(x))
  paste0("[", paste(vapply(x, num, character(1)), collapse = ","), "]")
}

write_json <- function(sd, code, path) {
  nd <- decl_ndim(code)
  keep <- names(sd)[!vapply(sd, function(v) is.character(v) || is.factor(v) ||
                                            is.list(v), logical(1))]
  parts <- vapply(keep, function(k) {
    v <- sd[[k]]
    dd <- dim(v)
    attributes(v) <- NULL
    if (!is.null(dd) && length(dd) >= 2) dim(v) <- dd
    n <- if (!is.null(nd[[k]])) nd[[k]] else
         if (!is.null(dd)) length(dd) else if (length(v) > 1) 1 else 0
    paste0("\"", k, "\":", emit(v, n))
  }, character(1))
  writeLines(paste0("{", paste(parts, collapse = ","), "}"), path)
}
