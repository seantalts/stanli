# The fallback path works without a runtime: stanc3 compiled to JavaScript and
# run through V8. Keep one compiler test independent of runtime availability so
# every package-checking host exercises source compilation.

portable_payload <- function(mir) {
  expect_true(startsWith(mir, "STANLI2:"))
  jsonlite::base64_dec(substring(mir, 9L))
}

raw_contains <- function(haystack, needle) {
  if (is.character(needle)) needle <- charToRaw(enc2utf8(needle))
  if (length(needle) == 0L) return(TRUE)
  if (length(haystack) < length(needle)) return(FALSE)
  any(vapply(seq_len(length(haystack) - length(needle) + 1L), function(i) {
    identical(haystack[i:(i + length(needle) - 1L)], needle)
  }, logical(1)))
}

test_that("the bundled JavaScript compiler produces MIR", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")
  skip_if(!nzchar(stanli:::stanc_js_path()) ||
            !file.exists(stanli:::stanc_js_path()),
          "stanc.js is not in this installation")

  mir <- stanli:::mir_from_js("
    data { int<lower=0> N; vector[N] y; }
    parameters { real mu; real<lower=0> sigma; }
    model { y ~ normal(mu, sigma); }")

  expect_type(mir, "character")
  payload <- portable_payload(mir)
  # Decode the envelope before looking for the model's own symbols. This
  # rejects a valid-looking prefix around an empty or unrelated document.
  expect_gt(length(payload), 100L)
  expect_true(raw_contains(payload, "mu"))
  expect_true(raw_contains(payload, "sigma"))
  expect_true(raw_contains(payload, "normal"))
})

test_that("the bundled JavaScript compiler applies O1", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")
  skip_if(!nzchar(stanli:::stanc_js_path()) ||
            !file.exists(stanli:::stanc_js_path()),
          "stanc.js is not in this installation")

  mir <- stanli:::mir_from_js("
    parameters { real theta; }
    model {
      if (1) theta ~ normal(0, 1);
      else theta ~ student_t(3, 0, 1);
    }")

  # O1 removes the dead branch while preserving the live source arithmetic.
  payload <- portable_payload(mir)
  expect_true(raw_contains(payload, "normal_lpdf"))
  expect_false(raw_contains(payload, "student_t_lpdf"))
})

test_that("the V8 helper prefers portable output and never retries its errors", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")

  state <- stanli:::stanc_js_ctx
  had_ctx <- exists("ctx", envir = state, inherits = FALSE)
  old_ctx <- state$ctx
  on.exit(if (had_ctx) state$ctx <- old_ctx else
            rm("ctx", envir = state), add = TRUE)

  ctx <- V8::v8()
  ctx$eval("globalThis.__stanli_calls = {
    portable: 0, classic: 0, arguments: 0, name: null, source: null
  };
  globalThis.stanli_compile = function(name, source) {
    globalThis.__stanli_calls.portable += 1;
    globalThis.__stanli_calls.arguments = arguments.length;
    globalThis.__stanli_calls.name = name;
    globalThis.__stanli_calls.source = source;
    return {result: 'STANLI2:ZmFrZQ==', warnings: ['warning with θ']};
  };
  globalThis.stanc = function() {
    globalThis.__stanli_calls.classic += 1;
    return {result: '(legacy MIR)'};
  };")
  state$ctx <- ctx

  source <- enc2utf8("transformed data { print(\"π ☃ θ\"); } model {}")
  expect_identical(
    stanli:::mir_from_js(source, enc2utf8("portable_π")),
    "STANLI2:ZmFrZQ==")
  calls <- jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_calls)"))
  expect_identical(calls$portable, 1L)
  expect_identical(calls$classic, 0L)
  expect_identical(calls$arguments, 3L)
  expect_identical(calls$name, enc2utf8("portable_π"))
  expect_identical(calls$source, source)

  ctx$eval("globalThis.stanli_compile = function() {
    globalThis.__stanli_calls.portable += 1;
    return {errors: ['portable failure'], warnings: []};
  };")
  expect_error(stanli:::mir_from_js("bad model"),
               "stanli_compile: portable failure", fixed = TRUE)
  calls <- jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_calls)"))
  expect_identical(calls$portable, 2L)
  expect_identical(calls$classic, 0L)

  ctx$eval("globalThis.stanli_compile = {};")
  expect_error(stanli:::mir_from_js("model {}"),
               "stanli_compile: export is not a function", fixed = TRUE)
  calls <- jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_calls)"))
  expect_identical(calls$classic, 0L)

  ctx$eval("delete globalThis.stanli_compile;
  globalThis.stanc = function(name, source, flags) {
    globalThis.__stanli_calls.classic += 1;
    globalThis.__stanli_calls.fallback_arguments = arguments.length;
    globalThis.__stanli_calls.fallback_flags = flags;
    return {result: '(legacy MIR)'};
  };")
  expect_identical(stanli:::mir_from_js("model {}"), "(legacy MIR)")
  calls <- jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_calls)"))
  expect_identical(calls$classic, 1L)
  expect_identical(calls$fallback_arguments, 4L)
  expect_identical(calls$fallback_flags,
                   c("O1", "debug-optimized-mir"))
})

test_that("the native portable compiler owns flags and keeps warnings separate", {
  seen <- NULL
  run_stanc <- function(compiler, args, stdout, stderr) {
    seen <<- list(compiler = compiler, args = args,
                  stdout = stdout, stderr = stderr,
                  work = dirname(stdout))
    source <- file.path(dirname(stdout), "model.stan")
    source_bytes <- readBin(source, "raw", n = file.info(source)$size)
    expect_identical(source_bytes,
                     charToRaw(enc2utf8("model { // θ\r\n}\r\n")))
    con <- file(stdout, open = "wb")
    writeChar("STANLI2:dGhldGEgzrg=", con,
              eos = NULL, useBytes = TRUE)
    close(con)
    writeLines("a successful frontend warning", stderr, useBytes = TRUE)
    0L
  }

  expect_identical(stanli:::mir_from_binary(
                     "fake-stanli-compile", "model { // θ\r\n}\r\n",
                     portable = TRUE,
                     run_stanc = run_stanc),
                   "STANLI2:dGhldGEgzrg=")
  expect_identical(seen$compiler, "fake-stanli-compile")
  expect_length(seen$args, 1)
  expect_false(dir.exists(seen$work))
})

test_that("UTF-8 Stan files survive locale-independent compiler staging", {
  source_file <- tempfile(fileext = ".stan")
  on.exit(unlink(source_file), add = TRUE)
  source_text <- enc2utf8("transformed data { print(\"\u03b8\"); }\nmodel {}\n")
  writeBin(charToRaw(source_text), source_file)

  old_locale <- Sys.getlocale("LC_CTYPE")
  on.exit(suppressWarnings(Sys.setlocale("LC_CTYPE", old_locale)), add = TRUE)
  expect_true(nzchar(suppressWarnings(Sys.setlocale("LC_CTYPE", "C"))))

  code <- stanli:::read_utf8_file(source_file)
  expected <- charToRaw(source_text)
  expect_identical(charToRaw(code), expected)

  run_stanc <- function(compiler, args, stdout, stderr) {
    staged <- file.path(dirname(stdout), "model.stan")
    expect_identical(readBin(staged, "raw", n = file.info(staged)$size),
                     expected)
    writeChar("STANLI2:ZmFrZQ==", stdout, eos = NULL,
              useBytes = TRUE)
    file.create(stderr)
    0L
  }
  expect_identical(
    stanli:::mir_from_binary("fake-stanli-compile", code, portable = TRUE,
                              run_stanc = run_stanc),
    "STANLI2:ZmFrZQ==")
})

test_that("the pristine stanc fallback requests O1 optimized MIR", {
  seen <- NULL
  run_stanc <- function(compiler, args, stdout, stderr) {
    seen <<- list(compiler = compiler, args = args, work = dirname(stdout))
    writeLines("(fake MIR)", stdout, useBytes = TRUE)
    writeLines("generated side effect", file.path(dirname(stdout), "model.hpp"))
    file.create(stderr)
    0L
  }

  expect_identical(stanli:::mir_from_binary(
                     "fake-stanc", "model {}", run_stanc = run_stanc),
                   "(fake MIR)")
  expect_identical(seen$compiler, "fake-stanc")
  expect_identical(seen$args[1:2],
                   c("--O1", "--debug-optimized-mir"))
  expect_false(dir.exists(seen$work))
})

test_that("a failing preferred compiler is not hidden by partial output", {
  run_stanc <- function(compiler, args, stdout, stderr) {
    writeLines("partial portable output", stdout)
    writeLines("source-bearing syntax error", stderr)
    1L
  }

  expect_error(
    stanli:::mir_from_binary("fake-stanli-compile", "bad model",
                              portable = TRUE, run_stanc = run_stanc),
    "source-bearing syntax error")
})

test_that("native discovery prefers the packaged portable compiler", {
  work <- tempfile("stanli-compiler-discovery-")
  dir.create(work)
  on.exit(unlink(work, recursive = TRUE), add = TRUE)
  runtime <- file.path(work, "stanli.dll")
  portable <- file.path(work, "stanli-compile.exe")
  stanc <- file.path(work, "stanc.exe")
  file.create(runtime, portable, stanc)

  from_path <- function(name) {
    expect_identical(name, "stanc.exe")
    "C:/tools/stanc.exe"
  }
  found <- stanli:::find_native_compiler("Windows", runtime, "", from_path)
  expect_true(found$portable)
  expect_identical(normalizePath(found$path, winslash = "/", mustWork = TRUE),
                   normalizePath(portable, winslash = "/", mustWork = TRUE))

  # Explicit configuration remains the bisect/override mechanism.
  expect_identical(
    stanli:::find_native_compiler("Windows", runtime, "C:/old/stanc.exe",
                                   from_path),
    list(path = "C:/old/stanc.exe", portable = FALSE))

  unlink(portable)
  found <- stanli:::find_native_compiler("Windows", runtime, "", from_path)
  expect_false(found$portable)
  expect_identical(normalizePath(found$path, winslash = "/", mustWork = TRUE),
                   normalizePath(stanc, winslash = "/", mustWork = TRUE))
  unlink(stanc)
  expect_identical(
    stanli:::find_native_compiler("Windows", runtime, "", from_path),
    list(path = "C:/tools/stanc.exe", portable = FALSE))

  expect_null(stanli:::find_native_compiler(
    "Emscripten", runtime, "",
    function(name) stop("webR must not search for a process")))
})

test_that("the webR helper selects by presence and never retries errors", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")

  state <- stanli:::stanc_js_ctx
  had_loaded <- exists("webr_loaded", envir = state, inherits = FALSE)
  old_loaded <- state$webr_loaded
  on.exit(if (had_loaded) state$webr_loaded <- old_loaded else
            rm("webr_loaded", envir = state), add = TRUE)
  state$webr_loaded <- TRUE

  ctx <- V8::v8()
  literal_value <- enc2utf8("C:\\Users\\runner\\file_'π\n")
  expect_identical(
    ctx$eval(stanli:::js_string_literal(literal_value)), literal_value)
  ctx$eval("globalThis.__stanli_source = '';
  globalThis.__stanli_written = null;
  globalThis.__stanli_calls = {
    portable: 0, classic: 0, arguments: 0,
    name: null, source: null, warnings: 0
  };
  globalThis.Module = {FS: {
    readFile: function() { return globalThis.__stanli_source; },
    writeFile: function(path, value) {
      globalThis.__stanli_written = {path: path, value: String(value)};
    }
  }};
  globalThis.stanli_compile = function(name, source) {
    globalThis.__stanli_calls.portable += 1;
    globalThis.__stanli_calls.arguments = arguments.length;
    globalThis.__stanli_calls.name = name;
    globalThis.__stanli_calls.source = source;
    var r = {result: 'STANLI2:ZmFrZQ==', warnings: ['warning with ☃']};
    globalThis.__stanli_calls.warnings = r.warnings.length;
    return r;
  };
  globalThis.stanc = function() {
    globalThis.__stanli_calls.classic += 1;
    return {result: '(legacy MIR)'};
  };")

  eval_js <- function(script) {
    match <- regexec("Module\\.FS\\.readFile\\(([^,]+),", script)
    groups <- regmatches(script, match)[[1]]
    expect_length(groups, 2)
    source_path <- ctx$eval(groups[[2]])
    ctx$assign("__stanli_source",
               stanli:::read_compiler_output(source_path))
    status <- ctx$eval(script)
    payload <- ctx$eval(
      "globalThis.__stanli_written
        ? JSON.stringify(globalThis.__stanli_written) : ''")
    if (nzchar(payload)) {
      written <- jsonlite::fromJSON(payload, simplifyVector = TRUE)
      stanli:::write_utf8_file(written$path, written$value)
      ctx$eval("globalThis.__stanli_written = null")
    }
    status
  }

  source <- enc2utf8("model { // π ☃ θ\n}")
  model_name <- enc2utf8("webr_'π\\name")
  expect_identical(
    stanli:::mir_from_webr(eval_js, source, model_name),
    "STANLI2:ZmFrZQ==")
  calls <- jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_calls)"))
  expect_identical(calls$portable, 1L)
  expect_identical(calls$classic, 0L)
  expect_identical(calls$arguments, 3L)
  expect_identical(calls$name, model_name)
  expect_identical(calls$source, source)
  expect_identical(calls$warnings, 1L)

  ctx$eval("globalThis.stanli_compile = function() {
    globalThis.__stanli_calls.portable += 1;
    return {errors: ['portable failure'], warnings: []};
  };")
  expect_error(stanli:::mir_from_webr(eval_js, "bad model"),
               "stanli_compile: portable failure", fixed = TRUE)
  calls <- jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_calls)"))
  expect_identical(calls$portable, 2L)
  expect_identical(calls$classic, 0L)

  ctx$eval("globalThis.stanli_compile = {};")
  expect_error(stanli:::mir_from_webr(eval_js, "model {}"),
               "stanli_compile: export is not a function", fixed = TRUE)
  calls <- jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_calls)"))
  expect_identical(calls$classic, 0L)

  ctx$eval("delete globalThis.stanli_compile;
  globalThis.stanc = function(name, source, flags) {
    globalThis.__stanli_calls.classic += 1;
    globalThis.__stanli_calls.arguments = arguments.length;
    globalThis.__stanli_calls.fallback_flags = flags;
    return {result: '(legacy MIR)'};
  };")
  expect_identical(stanli:::mir_from_webr(eval_js, "model {}"),
                   "(legacy MIR)")
  calls <- jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_calls)"))
  expect_identical(calls$classic, 1L)
  expect_identical(calls$arguments, 4L)
  expect_identical(calls$fallback_flags,
                   c("O1", "debug-optimized-mir"))
})

test_that("a model that does not typecheck is an error, not empty MIR", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")
  skip_if(!nzchar(stanli:::stanc_js_path()) ||
            !file.exists(stanli:::stanc_js_path()),
          "stanc.js is not in this installation")

  # `y` is undeclared. The portable compiler reports this through `errors`
  # rather than a thrown exception, so the wrapper has to look for it: without
  # that check a broken model reaches the lowering pass as NULL.
  expect_error(
    stanli:::mir_from_js("parameters { real mu; } model { mu ~ normal(y, 1); }"),
    "stanli_compile", fixed = TRUE)
})

test_that("runtime asset names match the five published targets", {
  # r/R/install.R and the release workflow build these names independently.
  # Check the supported targets directly: R CMD check may itself run on an
  # unsupported host, such as Windows arm64.
  published <- c("stanli-runtime-darwin-arm64.tar.gz",
                 "stanli-runtime-darwin-x86_64.tar.gz",
                 "stanli-runtime-linux-x86_64.tar.gz",
                 "stanli-runtime-linux-arm64.tar.gz",
                 "stanli-runtime-windows-x86_64.tar.gz")
  os <- c("darwin", "darwin", "linux", "linux", "windows")
  arch <- c("arm64", "x86_64", "x86_64", "arm64", "x86_64")
  actual <- mapply(stanli:::runtime_asset, os, arch, USE.NAMES = FALSE)

  expect_equal(actual, published)
})
