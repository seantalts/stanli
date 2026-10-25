random_state <- function() {
  if (exists(".Random.seed", envir = globalenv(), inherits = FALSE))
    get(".Random.seed", envir = globalenv())
}

without_random_state <- function(code) {
  old <- random_state()
  on.exit(if (is.null(old)) {
    if (exists(".Random.seed", envir = globalenv(), inherits = FALSE))
      rm(".Random.seed", envir = globalenv())
  } else assign(".Random.seed", old, envir = globalenv()))
  if (!is.null(old)) rm(".Random.seed", envir = globalenv())
  force(code)
}
