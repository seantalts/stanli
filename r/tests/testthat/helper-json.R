# A reader for the nested-array JSON the data writer emits, so a test can
# compare structure without a JSON package. An array of numbers comes back as
# a numeric array in the writer's first-index-outermost layout.
jsonlite_free_parse <- function(text) {
  tokens <- regmatches(text, gregexpr("[][{}:,]|\"[^\"]*\"|[^][{}:,[:space:]]+",
                                      text))[[1L]]
  pos <- 1L
  value <- function() {
    token <- tokens[pos]
    pos <<- pos + 1L
    if (token == "{") {
      out <- list()
      while (tokens[pos] != "}") {
        key <- gsub("\"", "", tokens[pos])
        pos <<- pos + 2L
        out[key] <- list(value())
        if (tokens[pos] == ",") pos <<- pos + 1L
      }
      pos <<- pos + 1L
      out
    } else if (token == "[") {
      out <- list()
      while (tokens[pos] != "]") {
        out[[length(out) + 1L]] <- value()
        if (tokens[pos] == ",") pos <<- pos + 1L
      }
      pos <<- pos + 1L
      json_array(out)
    } else {
      as.numeric(token)
    }
  }
  value()
}

json_array <- function(items) {
  if (!length(items)) return(array(numeric(0), 0L))
  shapes <- lapply(items, dim)
  if (all(vapply(items, function(i) is.numeric(i) && is.null(dim(i)), logical(1))))
    return(array(unlist(items), length(items)))
  if (all(vapply(shapes, function(s) !is.null(s), logical(1))) &&
      length(unique(shapes)) == 1L) {
    shape <- shapes[[1L]]
    return(aperm(array(unlist(lapply(items, as.vector)), c(shape, length(items))),
                 c(length(shape) + 1L, seq_along(shape))))
  }
  items
}
