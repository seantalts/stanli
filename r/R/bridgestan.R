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
