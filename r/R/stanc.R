# Getting transformed MIR out of stanc3, four ways.
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
# Under webR there is no V8 package and no process to run a binary in,
# but the host already IS a JavaScript engine: the webr support
# package's eval_js() evaluates in the worker's global scope, and the
# same bundled stanc.js defines stanc() there once per session.
#
# A native stanc on PATH or in STANLI_STANC still wins over the JS ones
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

# A native stanc, when one is configured or on the PATH. Not under webR:
# there are no processes to run one in, and Sys.which warns about the
# missing `which` on every call there.
find_stanc <- function() {
  if (identical(Sys.info()[["sysname"]], "Emscripten")) return("")
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

# The webR path. Source and MIR travel through the shared Emscripten
# filesystem rather than through eval_js values: the MIR of a real model
# is megabytes, a file path survives any marshalling limit, and neither
# string ever needs escaping into a JavaScript literal.
# The support package is reached by computed name, and deliberately not
# declared in Suggests: CRAN has an unrelated package also named `webr`,
# so a declaration resolves to the wrong one (its 17-dependency install
# is what broke the macOS and Windows check runners), and webR's own
# support package exists nowhere a checker could fetch it from. The
# sysname guard keeps CRAN's webr from ever being touched on a native
# machine that happens to have it installed.
webr_eval_js <- function() {
  if (!identical(Sys.info()[["sysname"]], "Emscripten")) return(NULL)
  pkg <- "webr"
  if (!requireNamespace(pkg, quietly = TRUE)) return(NULL)
  f <- get0("eval_js", envir = asNamespace(pkg))
  if (is.function(f)) f else NULL
}

mir_from_webr <- function(eval_js, code, name = "stanli_model") {
  if (is.null(stanc_js_ctx$webr_loaded)) {
    js <- stanc_js_path()
    if (!nzchar(js) || !file.exists(js))
      stop("the bundled stanc.js is missing from the installed package",
           call. = FALSE)
    eval_js(paste(readLines(js, warn = FALSE), collapse = "\n"))
    stanc_js_ctx$webr_loaded <- TRUE
  }
  src <- tempfile(fileext = ".stan")
  mirf <- tempfile(fileext = ".mir")
  on.exit(unlink(c(src, mirf)), add = TRUE)
  writeLines(code, src)
  status <- eval_js(sprintf("(() => {
    const src = Module.FS.readFile('%s', {encoding: 'utf8'});
    const r = globalThis.stanc('%s', src, ['debug-transformed-mir']);
    if (r.errors) return 'ERR: ' + String(r.errors);
    Module.FS.writeFile('%s', r.result);
    return 'ok';
  })()", src, name, mirf))
  if (!identical(status, "ok"))
    stop("stanc: ", sub("^ERR: ", "", status), call. = FALSE)
  paste(readLines(mirf, warn = FALSE), collapse = "\n")
}

stanc_mir <- function(code) {
  stanc <- find_stanc()
  if (nzchar(stanc)) return(mir_from_binary(stanc, code))
  ejs <- webr_eval_js()
  if (!is.null(ejs)) return(mir_from_webr(ejs, code))
  mir_from_js(code)
}
