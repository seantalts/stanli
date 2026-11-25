# Getting transformed MIR out of stanc3, four ways.
#
# The runtime usually embeds stanc3 -- the released library links the
# OCaml compiler in, so `stanli_model(code = ...)` hands the source
# straight to it and none of this runs.
#
# When it does not (a source build, or the Windows runtime), native hosts
# first use the stanli-compile executable beside the runtime, which emits
# portable MIR through the shared OCaml pipeline. Without it, the fallback is
# the same compiler built to JavaScript and run through V8. js_of_ocaml turns
# the OCaml into one
# 3.0 MB file with no toolchain or platform-specific binary, and the same
# artifact can execute inside webR. CI checks that this generated file comes
# from the exact configured stanc3 source revision, and the R tests compile
# both valid and invalid models through it on every supported host.
#
# Under webR there is no V8 package and no process to run a binary in,
# but the host already IS a JavaScript engine: the webr support
# package's eval_js() evaluates in the worker's global scope, and the
# same bundled stanc.js defines stanli_compile() there once per session.
#
# Every path reports the compiler's warnings as one R warning.

stanc_js_ctx <- new.env(parent = emptyenv())

stan_include_paths <- function(include_paths = NULL, file = NULL) {
  if (is.null(include_paths)) include_paths <- character()
  if (!is.character(include_paths) || anyNA(include_paths) ||
      any(!nzchar(include_paths)))
    stop("include_paths must be a character vector of nonempty directory paths",
         call. = FALSE)
  if (any(!dir.exists(path.expand(include_paths))))
    stop("include_paths directory does not exist: ",
         paste(include_paths[!dir.exists(path.expand(include_paths))], collapse = ", "),
         call. = FALSE)
  fallback <- if (is.null(file)) getwd() else
    dirname(normalizePath(file, winslash = "/", mustWork = TRUE))
  unique(normalizePath(c(include_paths, fallback), winslash = "/", mustWork = TRUE))
}

# stanc.js accepts an in-memory filename -> source map. Discover candidate
# dependencies without expanding or rewriting source; Stan's own preprocessor
# still handles nesting, repeated includes, cycles, and source locations.
# Skip comments and string literals using the pinned Stan lexer's token forms.
# Missing files stay absent so the compiler reports the include's location.
stan_include_sources <- function(code, include_paths) {
  if (!grepl("#include", code, fixed = TRUE)) return(list())
  # OCamllex chooses the longest token. A quoted/bracketed name can beat the
  # bare form only if it contains whitespace; otherwise match the entire bare
  # token first and remove delimiters only when they enclose that whole token.
  tokens <- paste0('(?s)/\\*.*?(?:\\*/|$)|//[^\\r\\n]*|"[^"\\r\\n]*"|',
    '#include[ \\t\\f\\r\\n]+(?:"[^"\\r\\n]*[ \\t\\f][^"\\r\\n]*"|',
    '<[^>\\r\\n]*[ \\t\\f][^>\\r\\n]*>|[^ \\t\\f\\r\\n]*)')
  sources <- list()
  pending <- list(code)
  seen <- character()
  while (length(pending)) {
    text <- pending[[1L]]
    pending <- pending[-1L]
    matches <- regmatches(text, gregexpr(tokens, text, perl = TRUE))[[1L]]
    for (token in matches[startsWith(matches, "#include")]) {
      name <- sub("^#include[ \\t\\f\\r\\n]+", "", token, perl = TRUE)
      if (grepl('^("[^"\\r\\n]*"|<[^>\\r\\n]*>)$', name, perl = TRUE))
        name <- substring(name, 2L, nchar(name) - 1L)
      if (!nzchar(name) || name %in% seen) next
      seen <- c(seen, name)
      for (directory in include_paths) {
        # Concatenation matches stanc's search, including absolute-looking names.
        path <- paste0(directory, "/", name)
        if (!file.exists(path) || dir.exists(path)) next
        source <- tryCatch(read_utf8_file(path), error = function(e) NULL)
        if (is.null(source)) next
        sources[[name]] <- source
        pending[[length(pending) + 1L]] <- source
        break
      }
    }
  }
  sources
}

warn_compiler <- function(text) {
  text <- paste(text[nzchar(text)], collapse = "\n")
  if (nzchar(trimws(text))) warning(sub("\\s+$", "", text), call. = FALSE)
  invisible(NULL)
}

# Compiler messages from the runtime: list(kind, text). Warnings go out as
# one warning.
warn_compiler_messages <- function(messages) {
  if (is.null(messages)) return(invisible(NULL))
  warn_compiler(messages$text[messages$kind == "warning"])
  invisible(NULL)
}

stanc_js_path <- function() {
  system.file("js", "stanc.js", package = "stanli")
}

# One V8 context per session: loading 3.0 MB of JavaScript takes a moment
# and the compiler is stateless afterwards.
stanc_js <- function() {
  if (!is.null(stanc_js_ctx$ctx)) return(stanc_js_ctx$ctx)
  if (!requireNamespace("V8", quietly = TRUE))
    stop("compiling Stan source needs either a runtime with the embedded ",
         "compiler, the stanli-compile executable beside the runtime, or the ",
         "V8 package for the bundled JavaScript compiler.", call. = FALSE)
  js <- stanc_js_path()
  if (!nzchar(js) || !file.exists(js))
    stop("the bundled stanc.js is missing from the installed package",
         call. = FALSE)
  ctx <- V8::v8()
  ctx$source(js)
  stanc_js_ctx$ctx <- ctx
  ctx
}

mir_from_js <- function(code, name = "stanli_model", include_paths = character(),
                        fast_math = FALSE, file = NULL) {
  ctx <- stanc_js()
  ctx$assign("stanli_fast", isTRUE(fast_math))
  ctx$assign("stanli_src", enc2utf8(code))
  ctx$assign("stanli_name", name)
  ctx$assign("stanli_includes", stan_include_sources(code, include_paths))
  ctx$assign("stanli_named", !is.null(file))
  ctx$assign("stanli_file", if (is.null(file)) "" else enc2utf8(file))
  out <- ctx$eval(
    "(function () {
       function exported(name) {
         if (Object.prototype.hasOwnProperty.call(globalThis, name))
           return {present: true, value: globalThis[name]};
         var common = globalThis.module && globalThis.module.exports;
         if (common && Object.prototype.hasOwnProperty.call(common, name))
           return {present: true, value: common[name]};
         return {present: false, value: null};
       }
       var name = stanli_fast ? 'stanli_compile_fast' : 'stanli_compile';
       var found = exported(name);
       if (!found.present)
         return JSON.stringify({e: stanli_fast
                                     ? 'fast_math needs a stanc.js with the ' +
                                       'stanli_compile_fast export'
                                     : 'no stanli_compile() export',
                                c: 'JavaScript compiler'});
       var compile = found.value;
       if (typeof compile !== 'function')
         return JSON.stringify({e: 'export is not a function', c: name});
       try {
         var r = stanli_named
           ? compile(stanli_name, stanli_src, stanli_includes, stanli_file)
           : compile(stanli_name, stanli_src, stanli_includes);
         if (!r || typeof r !== 'object')
           return JSON.stringify({e: 'compiler returned no result object',
                                  c: name});
         if (r.errors)
           return JSON.stringify({e: String(r.errors), c: name});
         if (typeof r.result === 'undefined')
           return JSON.stringify({e: 'compiler returned no MIR', c: name});
         return JSON.stringify({r: String(r.result), c: name,
                                w: (r.warnings || []).map(String)});
       } catch (e) {
         return JSON.stringify({e: String(e), c: name});
       }
     })()")
  parsed <- jsonlite::fromJSON(out, simplifyVector = TRUE)
  if (!is.null(parsed$e))
    stop(parsed$c, ": ", paste(parsed$e, collapse = "\n"), call. = FALSE)
  warn_compiler(parsed$w)
  parsed$r
}

# The stanli-compile executable beside the runtime, or NULL. It is only
# looked for there: its output is a versioned stanli/runtime contract, so an
# unrelated stanli-compile on PATH must not pair with this library.
# Not under webR: there are no processes to run.
find_native_compiler <- function(
    sysname = Sys.info()[["sysname"]],
    runtime = stanli_runtime_path()) {
  if (identical(sysname, "Emscripten")) return(NULL)
  suffix <- if (identical(sysname, "Windows")) ".exe" else ""
  portable <- file.path(dirname(runtime), paste0("stanli-compile", suffix))
  if (file.exists(portable) && !dir.exists(portable)) portable else NULL
}

# Read compiler output as explicit UTF-8 bytes. Captured system2 output is
# otherwise converted through the native Windows locale before R sees it.
read_compiler_output <- function(path) {
  size <- file.info(path)$size
  if (is.na(size) || size == 0) return("")
  value <- rawToChar(readBin(path, "raw", n = size))
  Encoding(value) <- "UTF-8"
  value
}

write_utf8_file <- function(path, value) {
  con <- file(path, open = "wb")
  tryCatch(
    writeBin(charToRaw(enc2utf8(value)), con),
    finally = close(con))
}

js_string_literal <- function(value) {
  encodeString(enc2utf8(value), quote = "'")
}

# The compiler prints every error of its own to stderr, so a failure with
# empty stderr means that the program did not run to completion.
silent_compiler_failure <- function(compiler, status, r_warnings) {
  status <- as.integer(status)
  shown_status <- if (length(status) == 1L && !is.na(status))
    as.character(status) else "unknown"
  paste0(
    "stanc failed: the Stan compiler exited with status ", shown_status,
    " and printed no error message.\n",
    "Compiler: ", compiler, "\n",
    if (length(r_warnings))
      paste0("R reported: ", paste(r_warnings, collapse = "\n"), "\n"),
    if (identical(status, 127L))
      "Status 127 means that R could not run the program.\n",
    "The compiler prints an error for every problem that it finds, so this ",
    "means that the program did not start or was stopped before it ",
    "finished.\n",
    "To fix this:\n",
    "1. In a fresh R session, run stanli::stanli_install(overwrite = TRUE) ",
    "to download the runtime again.\n",
    "2. If it still fails, check whether security or antivirus software on ",
    "this machine blocked or removed ", compiler, ", and allow it.")
}

mir_from_binary <- function(compiler, code, run_stanc = system2,
                            include_paths = character(), fast_math = FALSE,
                            file = NULL) {
  work <- tempfile("stanli-compile-")
  if (!dir.create(work))
    stop("could not create a temporary compiler directory", call. = FALSE)
  on.exit(unlink(work, recursive = TRUE, force = TRUE), add = TRUE)

  source <- file.path(work, "model.stan")
  write_utf8_file(source, code)

  stdout_file <- file.path(work, "stdout")
  stderr_file <- file.path(work, "stderr")
  messages_file <- file.path(work, "messages.json")
  include_args <- as.vector(rbind(rep("--include-path", length(include_paths)),
                                  shQuote(include_paths)))
  name_args <- if (is.null(file)) character() else c("--filename", shQuote(file))
  args <- c(if (fast_math) "--fast-math", name_args, include_args,
            "--messages-json", shQuote(messages_file), shQuote(source))
  r_warnings <- character()
  status <- tryCatch(
    withCallingHandlers(
      run_stanc(compiler, args, stdout = stdout_file, stderr = stderr_file),
      warning = function(w) {
        r_warnings <<- c(r_warnings, conditionMessage(w))
        invokeRestart("muffleWarning")
      }),
    error = function(e) {
      stop("could not run the bundled Stan compiler: ", conditionMessage(e),
           call. = FALSE)
    })
  out <- read_compiler_output(stdout_file)
  diagnostics <- read_compiler_output(stderr_file)
  if (!identical(as.integer(status), 0L) || !nzchar(out)) {
    if (nzchar(diagnostics))
      stop("stanc failed:\n", diagnostics, call. = FALSE)
    stop(silent_compiler_failure(compiler, status, r_warnings), call. = FALSE)
  }
  messages <- jsonlite::fromJSON(read_compiler_output(messages_file),
                                 simplifyVector = TRUE)
  warn_compiler(as.character(messages$warnings))
  out
}

# The webR path. Source and MIR travel through the shared Emscripten
# filesystem rather than through eval_js values: the MIR of a real model
# is megabytes, a file path survives any marshalling limit, and neither
# string ever needs escaping into a JavaScript literal.
# The support package is reached by computed name and deliberately not declared
# in Suggests: the ordinary R package also named `webr` is unrelated (its
# 17-dependency install once broke the macOS and Windows check runners), while
# webR's host support package is supplied by webR itself. The sysname guard
# keeps the unrelated package from being touched on a native machine.
webr_eval_js <- function() {
  if (!identical(Sys.info()[["sysname"]], "Emscripten")) return(NULL)
  pkg <- "webr"
  if (!requireNamespace(pkg, quietly = TRUE)) return(NULL)
  f <- get0("eval_js", envir = asNamespace(pkg))
  if (is.function(f)) f else NULL
}

mir_from_webr <- function(eval_js, code, name = "stanli_model",
                          include_paths = character(), fast_math = FALSE,
                          file = NULL) {
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
  write_utf8_file(src, code)
  includes <- stan_include_sources(code, include_paths)
  # Keep the existing file transport for potentially large source mappings too.
  incfiles <- character()
  on.exit(unlink(incfiles), add = TRUE)
  include_js <- vapply(names(includes), function(name) {
    path <- tempfile(fileext = ".stan")
    incfiles <<- c(incfiles, path)
    write_utf8_file(path, includes[[name]])
    sprintf("includes[%s] = Module.FS.readFile(%s, {encoding: 'utf8'});",
            js_string_literal(name), js_string_literal(path))
  }, character(1))
  src_js <- js_string_literal(src)
  name_js <- js_string_literal(name)
  mirf_js <- js_string_literal(mirf)
  file_js <- if (is.null(file)) "null" else js_string_literal(file)
  # The script leaves its outcome in globalThis.__stanli_webr and returns
  # whether compilation failed; the error and each warning are then read as
  # separate values, since eval_js converts only scalars.
  failed <- eval_js(sprintf("(() => {
    const done = (error, warnings) => {
      globalThis.__stanli_webr = {failed: error !== null, error: error,
                                  warnings: warnings};
      return error !== null;
    };
    const src = Module.FS.readFile(%s, {encoding: 'utf8'});
    const includes = Object.create(null);
    %s
    function exported(name) {
      if (Object.prototype.hasOwnProperty.call(globalThis, name))
        return {present: true, value: globalThis[name]};
      const common = globalThis.module && globalThis.module.exports;
      if (common && Object.prototype.hasOwnProperty.call(common, name))
        return {present: true, value: common[name]};
      return {present: false, value: null};
    }
    const fast = %s;
    const name = fast ? 'stanli_compile_fast' : 'stanli_compile';
    const found = exported(name);
    if (!found.present)
      return done(fast ? 'JavaScript compiler: fast_math needs a stanc.js with the stanli_compile_fast export'
                       : 'JavaScript compiler: no stanli_compile() export', []);
    const compile = found.value;
    if (typeof compile !== 'function')
      return done(name + ': export is not a function', []);
    const file = %s;
    try {
      const r = file === null ? compile(%s, src, includes)
                              : compile(%s, src, includes, file);
      if (!r || typeof r !== 'object')
        return done(name + ': compiler returned no result object', []);
      if (r.errors) return done(name + ': ' + String(r.errors), []);
      if (typeof r.result === 'undefined')
        return done(name + ': compiler returned no MIR', []);
      Module.FS.writeFile(%s, String(r.result));
      return done(null, (r.warnings || []).map(String));
    } catch (e) {
      return done(name + ': ' + String(e), []);
    }
  })()", src_js, paste(include_js, collapse = "\n"),
    if (isTRUE(fast_math)) "true" else "false",
    file_js, name_js, name_js, mirf_js))
  # V8 hands every value back as a string, webR as the R scalar.
  if (isTRUE(as.logical(failed)))
    stop(eval_js("globalThis.__stanli_webr.error"), call. = FALSE)
  n <- as.integer(eval_js("globalThis.__stanli_webr.warnings.length"))
  warn_compiler(vapply(seq_len(n) - 1L, function(i)
    eval_js(sprintf("globalThis.__stanli_webr.warnings[%d]", i)),
    character(1)))
  read_compiler_output(mirf)
}

stanc_mir <- function(code, include_paths = character(), fast_math = FALSE,
                      file = NULL) {
  compiler <- find_native_compiler()
  if (!is.null(compiler))
    return(mir_from_binary(compiler, code, include_paths = include_paths,
                           fast_math = fast_math, file = file))
  ejs <- webr_eval_js()
  if (!is.null(ejs))
    return(mir_from_webr(ejs, code, include_paths = include_paths,
                         fast_math = fast_math, file = file))
  mir_from_js(code, include_paths = include_paths, fast_math = fast_math,
              file = file)
}
