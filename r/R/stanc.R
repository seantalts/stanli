# Getting transformed MIR out of stanc3, three ways.
#
# The runtime usually embeds stanc3 -- the released library links the
# OCaml compiler in, so `stanli_model(code = ...)` hands the source
# straight to it and none of this runs.
#
# When it does not (a source build, or the Windows runtime), the fallback
# is stanc3 compiled to JavaScript and run through V8. That is the same
# trick rstan uses to ship a Stan compiler on CRAN: js_of_ocaml turns the
# OCaml into one 2.8 MB file with no toolchain and no platform binaries,
# which is a thing CRAN can carry and a native `stanc` per platform is
# not. tests/test_stancjs.cjs already checks that this build emits the
# same MIR as the native binary, byte for byte.
#
# A native stanc on PATH or in STANLI_STANC still wins over the JS one
# when it is there, because it is faster.

stanc_js_ctx <- new.env(parent = emptyenv())

stanc_js_path <- function() {
  system.file("js", "stanc.js", package = "stanli")
}

# One V8 context per session: loading 2.8 MB of JavaScript takes a moment
# and the compiler is stateless afterwards.
stanc_js <- function() {
  if (!is.null(stanc_js_ctx$ctx)) return(stanc_js_ctx$ctx)
  if (!requireNamespace("V8", quietly = TRUE))
    stop("compiling Stan source needs either a runtime with the embedded ",
         "compiler, a stanc3 binary (STANLI_STANC), or the V8 package for ",
         "the bundled JavaScript compiler.", call. = FALSE)
  js <- stanc_js_path()
  if (!nzchar(js) || !file.exists(js))
    stop("the bundled stanc.js is missing from the installed package",
         call. = FALSE)
  ctx <- V8::v8()
  ctx$source(js)
  stanc_js_ctx$ctx <- ctx
  ctx
}

mir_from_js <- function(code, name = "stanli_model") {
  ctx <- stanc_js()
  ctx$assign("stanli_src", code)
  ctx$assign("stanli_name", name)
  # js_of_ocaml exports stanc() on globalThis under V8. It returns an
  # object with `result` on success and `errors` otherwise.
  out <- ctx$eval(
    "(function () {
       var f = (typeof stanc === 'function') ? stanc
             : (globalThis.stanc || (globalThis.module &&
                globalThis.module.exports && globalThis.module.exports.stanc));
       if (typeof f !== 'function') return JSON.stringify({e: 'no stanc()'});
       var r = f(stanli_name, stanli_src, ['debug-transformed-mir']);
       if (r.errors) return JSON.stringify({e: String(r.errors)});
       return JSON.stringify({r: r.result});
     })()")
  parsed <- jsonlite::fromJSON(out, simplifyVector = TRUE)
  if (!is.null(parsed$e))
    stop("stanc: ", paste(parsed$e, collapse = "\n"), call. = FALSE)
  parsed$r
}

# A native stanc, when one is configured or on the PATH.
find_stanc <- function() {
  from_env <- Sys.getenv("STANLI_STANC", "")
  if (nzchar(from_env)) return(from_env)
  exe <- if (identical(Sys.info()[["sysname"]], "Windows")) "stanc.exe" else
    "stanc"
  beside <- file.path(dirname(stanli_runtime_path()), exe)
  if (file.exists(beside)) return(beside)
  found <- Sys.which(exe)
  if (nzchar(found)) return(unname(found))
  ""
}

mir_from_binary <- function(stanc, code) {
  f <- tempfile(fileext = ".stan")
  on.exit(unlink(f), add = TRUE)
  writeLines(code, f)
  out <- suppressWarnings(
    system2(stanc, c("--debug-transformed-mir", shQuote(f)),
            stdout = TRUE, stderr = TRUE))
  status <- attr(out, "status")
  if (!is.null(status) && status != 0)
    stop("stanc failed:\n", paste(out, collapse = "\n"), call. = FALSE)
  paste(out, collapse = "\n")
}

stanc_mir <- function(code) {
  stanc <- find_stanc()
  if (nzchar(stanc)) return(mir_from_binary(stanc, code))
  mir_from_js(code)
}
