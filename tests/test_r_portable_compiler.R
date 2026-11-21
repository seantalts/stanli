# Exercise the compiler actually bundled in the installed R package. Its output
# goes through the package's real V8 helper and public MIR entry point, backed
# by whichever runtime STANLI_RUNTIME selects for this fresh R process.

args <- commandArgs(trailingOnly = TRUE)
if (length(args) != 0L)
  stop("usage: test_r_portable_compiler.R", call. = FALSE)

if (!requireNamespace("V8", quietly = TRUE) ||
    !requireNamespace("jsonlite", quietly = TRUE))
  stop("the compiler check requires V8 and jsonlite", call. = FALSE)

suppressPackageStartupMessages(library(stanli))
compiler <- normalizePath(stanli:::stanc_js_path(), mustWork = TRUE)

# Put the installed package's exact artifact in the context cached by
# mir_from_js(). Wrap the export so each helper call is counted and records
# the warnings that the source-compilation API leaves internal on success.
ctx <- V8::v8()
ctx$source(compiler)
ctx$eval("(function () {
  var portable = globalThis.stanli_compile;
  if (typeof portable !== 'function')
    throw new Error('no stanli_compile() export');
  globalThis.__stanli_bundled = {portable_calls: 0, warnings: []};
  globalThis.stanli_compile = function() {
    globalThis.__stanli_bundled.portable_calls += 1;
    var r = portable.apply(this, arguments);
    globalThis.__stanli_bundled.warnings = r.warnings
      ? Array.prototype.map.call(r.warnings, String) : [];
    return r;
  };
})()")

compiler_state <- function() {
  jsonlite::fromJSON(
    ctx$eval("JSON.stringify(globalThis.__stanli_bundled)"),
    simplifyVector = TRUE)
}

state <- getFromNamespace("stanc_js_ctx", "stanli")
state$ctx <- ctx

compile_bundled <- function(name, code) {
  before <- compiler_state()
  error <- NULL
  result <- tryCatch(
    stanli:::mir_from_js(enc2utf8(code), enc2utf8(name)),
    error = function(e) {
      error <<- conditionMessage(e)
      NULL
    })
  after <- compiler_state()
  if (!identical(after$portable_calls, before$portable_calls + 1L))
    stop("the R helper did not make exactly one bundled compiler call",
         call. = FALSE)
  list(result = result,
       errors = if (is.null(error)) character() else error,
       warnings = after$warnings)
}

portable_payload <- function(mir) {
  stopifnot(startsWith(mir, "STANLI5:"))
  jsonlite::base64_dec(substring(mir, 9L))
}

raw_contains <- function(haystack, text) {
  needle <- charToRaw(enc2utf8(text))
  if (length(needle) == 0L) return(TRUE)
  if (length(haystack) < length(needle)) return(FALSE)
  any(vapply(seq_len(length(haystack) - length(needle) + 1L), function(i) {
    identical(haystack[i:(i + length(needle) - 1L)], needle)
  }, logical(1)))
}

unicode_source <- enc2utf8("\
transformed data { print(\"pi: π, snowman: ☃, wave: 👋\"); }
parameters { real mu; }
model { mu ~ std_normal(); }")
first <- compile_bundled("unicode_bundled", unicode_source)
second <- compile_bundled("unicode_bundled", unicode_source)
stopifnot(
  length(first$errors) == 0L,
  is.character(first$result), length(first$result) == 1L,
  startsWith(first$result, "STANLI5:"),
  !grepl("[\r\n]$", first$result),
  validUTF8(first$result),
  raw_contains(portable_payload(first$result), "π"),
  identical(first$result, second$result),
  identical(first$warnings, second$warnings)
)

warning_source <- "
parameters { real x; }
model { if (0 < x < 1) target += 0; }"
warning_result <- compile_bundled("warning_bundled", warning_source)
stopifnot(length(warning_result$errors) == 0L,
          length(warning_result$warnings) > 0L)

bad <- compile_bundled(
  "malformed_bundled", "parameters { real x } model {}")
stopifnot(is.null(bad$result), length(bad$errors) > 0L,
          grepl("stanli_compile", bad$errors, fixed = TRUE))

if (!stanli_available())
  stop("the real stanli runtime is required for this check", call. = FALSE)

runtime_source <- "
parameters { real mu; }
model { mu ~ std_normal(); }
generated quantities {
  real mu_twice = 2 * mu;
  real y_rep = normal_rng(mu, 1);
}"
runtime_compilation <- compile_bundled("runtime_bundled", runtime_source)
stopifnot(length(runtime_compilation$errors) == 0L,
          startsWith(runtime_compilation$result, "STANLI5:"))

# `mir` is the public compatibility seam on the runtime version that owns this
# producer. The appended transform_inits section is backward-compatible for
# current decoders, but a previous runtime's strict v2 decoder rejects bytes it
# does not know. In the deliberate current-package/previous-runtime CI cell,
# exercise that runtime through its matching embedded compiler instead; the
# compiler checks above still validate the current package's bundled artifact.
decode_error <- NULL
model <- tryCatch(
  stanli_model(mir = runtime_compilation$result),
  error = function(e) {
    decode_error <<- conditionMessage(e)
    NULL
  })
runtime_decodes_bundled <- is.null(decode_error)
if (!runtime_decodes_bundled) {
  if (!grepl("trailing bytes", decode_error, fixed = TRUE))
    stop("the bundled MIR failed unexpectedly: ", decode_error,
         call. = FALSE)
  model <- stanli_model(code = runtime_source)
}
gradient <- log_prob_grad(model, 0.25)
stopifnot(is.finite(gradient$lp),
          length(gradient$grad) == 1L,
          isTRUE(all.equal(unname(gradient$grad), -0.25,
                           tolerance = 1e-12)))

fit <- sample_model(model, chains = 1L, seed = 9182L, warmup = 20L,
                    samples = 12L, init = 0, init_radius = 0,
                    parallel_chains = 1L)
stopifnot(all(c("mu", "mu_twice", "y_rep") %in% fit$columns),
          all(is.finite(fit$draws[, , "y_rep"])),
          isTRUE(all.equal(as.numeric(fit$draws[, , "mu_twice"]),
                           2 * as.numeric(fit$draws[, , "mu"]),
                           tolerance = 1e-12)))

message(if (runtime_decodes_bundled) {
  "R bundled portable compiler OK: V8, decoder, gradient, sampling, and GQ"
} else {
  paste("R package/previous runtime OK; bundled compiler V8 contract and",
        "matching embedded decoder, gradient, sampling, and GQ")
})
