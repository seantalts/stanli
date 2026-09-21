// The webR runtime artifact loads into webR and resolves the C API.
//
// This is the ABI question the emsdk pin exists to answer: a side module
// built against the wrong Emscripten fails right here, in dyn.load. The
// full R-package flow (r-universe bridge, log_prob_grad, sampling) was
// verified by hand against webR 0.6.0 when this landed; this test keeps
// the load-and-resolve half, which needs nothing beyond the npm webr
// package.
//
// When the bundled compiler is supplied, load that exact tracked artifact
// through webR's host-JavaScript bridge and exercise its portable result too.
//
// Usage: node tests/test_webr.mjs build-wasm-side/libstanli.so \
//          r/inst/js/stanc.js
// The compiler-only mode runs the same R helper checks on pull requests
// without building a side module:
//   node tests/test_webr.mjs --compiler-only r/inst/js/stanc.js
import { WebR } from 'webr';
import fs from 'node:fs';

const soPath = process.argv[2];
const compilerPath = process.argv[3];
const compilerOnly = soPath === '--compiler-only';
if (!soPath || (compilerOnly && !compilerPath)) {
  console.error(
      'usage: node tests/test_webr.mjs <libstanli.so> [stanli-compiler.js]\n' +
      '   or: node tests/test_webr.mjs --compiler-only <stanli-compiler.js>');
  process.exit(2);
}

const webR = new WebR();
await webR.init();
const arch = (await (await webR.evalR('R.version$arch')).toJs()).values[0];
if (arch !== 'wasm32') {
  console.error(`FAIL unexpected webR arch ${arch}`);
  process.exit(1);
}

if (!compilerOnly) {
  await webR.FS.writeFile('/tmp/libstanli.so',
                          new Uint8Array(fs.readFileSync(soPath)));

  // The symbols the R bridge dlsyms, stanli_abi_version first: it is the
  // handshake, and the one a hand-kept export list forgot once already.
  const res = await webR.evalR(`
    dyn.load('/tmp/libstanli.so')
    syms <- c('stanli_abi_version', 'stanli_model_new', 'stanli_exact_lp',
              'stanli_grad', 'stanli_sample_multi',
              'stanli_sample_multi_progress', 'stanli_optimize',
              'stanli_diagnose_text', 'stanli_wa_row')
    syms[!vapply(syms, is.loaded, logical(1))]
  `);
  const missing = (await res.toJs()).values;
  if (missing.length > 0) {
    console.error('FAIL symbols missing from the side module:', missing.join(' '));
    process.exit(1);
  }
}

if (compilerPath) {
  await webR.FS.writeFile(
      '/tmp/stanli-compiler.js',
      new Uint8Array(fs.readFileSync(compilerPath)));
  await webR.FS.writeFile(
      '/tmp/stanli-stanc.R',
      new Uint8Array(fs.readFileSync('r/R/stanc.R')));
  await webR.FS.writeFile(
      '/tmp/stanli.R',
      new Uint8Array(fs.readFileSync('r/R/stanli.R')));
  await webR.FS.writeFile(
      '/tmp/portable-unicode.stan',
      new Uint8Array(fs.readFileSync('tests/compiler/portable_unicode.stan')));
  await webR.FS.writeFile(
      '/tmp/portable-bad.stan',
      new TextEncoder().encode('parameters { real x } model {}'));

  // Source the package's actual helper and point this webR session at the
  // bundled artifact. Source and MIR still cross through the shared
  // filesystem, avoiding evalR() marshalling for a multi-megabyte document.
  const bundled = await webR.evalR(String.raw`
    eval_js <- get0("eval_js", envir = asNamespace("webr"))
    if (!is.function(eval_js)) stop("webR has no eval_js host bridge")
    source("/tmp/stanli-stanc.R", local = .GlobalEnv)
    source("/tmp/stanli.R", local = .GlobalEnv)
    stanc_js_path <- function() "/tmp/stanli-compiler.js"

    # A browser worker has no Node process global. The npm webR harness runs
    # its worker under Node, where js_of_ocaml would otherwise select a
    # CommonJS filesystem and call an unavailable require(). Mask that test
    # runner detail while the helper evaluates the exact browser artifact.
    eval_js('globalThis.__stanli_test_process = {
      present: Object.prototype.hasOwnProperty.call(globalThis, "process"),
      value: globalThis.process
    }; globalThis.process = undefined; "ok"')
    restore_process <- function() eval_js(
      'if (globalThis.__stanli_test_process.present) {
         globalThis.process = globalThis.__stanli_test_process.value;
       } else {
         delete globalThis.process;
       }
       delete globalThis.__stanli_test_process;
       "ok"')

    statuses <- tryCatch({
      unicode_source <- read_compiler_output("/tmp/portable-unicode.stan")
      mir <- mir_from_webr(eval_js, unicode_source, "webr_bundled")
      write_utf8_file("/tmp/portable-bundled.mir", mir)
      unicode_ok <- eval_js('(() => {
        const mir = Module.FS.readFile("/tmp/portable-bundled.mir",
                                       {encoding: "utf8"});
        if (!mir.startsWith("STANLI2:"))
          return "FAIL missing portable envelope";
        const payload = Uint8Array.from(atob(mir.slice(8)),
                                        c => c.charCodeAt(0));
        const needle = new TextEncoder().encode("π ☃ é 👋");
        for (let i = 0; i + needle.length <= payload.length; ++i) {
          let equal = true;
          for (let j = 0; j < needle.length; ++j)
            equal = equal && payload[i + j] === needle[j];
          if (equal) return "ok";
        }
        return "FAIL UTF-8 changed";
      })()')
      good <- if (!startsWith(mir, "STANLI2:")) {
        "FAIL missing portable envelope"
      } else {
        unicode_ok
      }

      # Exercise the actual webR filesystem and eval_js bridge for nested
      # includes, including spaces and Unicode in directories and filenames.
      include_dir <- tempfile("stanli includes π ")
      dir.create(include_dir)
      dir.create(file.path(include_dir, "nested"))
      write_utf8_file(file.path(include_dir, "priors.stan"),
                      '#include "nested/prior π.stan"\n')
      write_utf8_file(file.path(include_dir, "nested", "prior π.stan"),
                      "x ~ normal(0, 1);\n")
      include_source <- paste0("parameters { real x; }\nmodel {\n",
                                "#include priors.stan\n}\n")
      check_includes <- function(prefix) {
        included_mir <- mir_from_webr(eval_js, include_source, "webr_includes",
                                      include_paths = include_dir)
        if (!startsWith(included_mir, prefix))
          return("FAIL nested includes produced no MIR")
        missing_message <- tryCatch({
          mir_from_webr(eval_js, "#include absent.stan\n", "webr_missing",
                         include_paths = include_dir)
          "FAIL missing include compiled"
        }, error = function(e) conditionMessage(e))
        if (!grepl("absent.stan", missing_message, fixed = TRUE))
          return("FAIL missing include lost its filename")
        "ok"
      }
      portable_includes <- check_includes("STANLI2:")

      instrumented <- eval_js('(() => {
        const portable = globalThis.stanli_compile;
        const classic = globalThis.stanc;
        if (typeof portable !== "function" || typeof classic !== "function")
          return "FAIL compiler exports missing";
        globalThis.__stanli_helper_calls = {
          portable: 0, classic: 0, warnings: 0,
          classic_arguments: 0, classic_flags: [], classic_includes: null
        };
        globalThis.stanli_compile = function() {
          globalThis.__stanli_helper_calls.portable += 1;
          const r = portable.apply(this, arguments);
          globalThis.__stanli_helper_calls.warnings = r.warnings
            ? r.warnings.length : 0;
          return r;
        };
        globalThis.stanc = function() {
          globalThis.__stanli_helper_calls.classic += 1;
          globalThis.__stanli_helper_calls.classic_arguments = arguments.length;
          globalThis.__stanli_helper_calls.classic_flags = arguments[2];
          globalThis.__stanli_helper_calls.classic_includes =
            arguments[3] && Object.keys(arguments[3]).sort();
          return classic.apply(this, arguments);
        };
        return "ok";
      })()')

      warning_source <- paste(
        "parameters { real x; }",
        "model { if (0 < x < 1) target += 0; }")
      warning_mir <- mir_from_webr(eval_js, warning_source, "webr_warning")
      warning_calls <- eval_js(
        'JSON.stringify(globalThis.__stanli_helper_calls)')
      warning_ok <- if (!startsWith(warning_mir, "STANLI2:")) {
        "FAIL warning source produced no portable MIR"
      } else if (!grepl('"warnings":[1-9]', warning_calls)) {
        "FAIL portable warning was not observed"
      } else {
        "ok"
      }

      bad_source <- read_compiler_output("/tmp/portable-bad.stan")
      bad_message <- tryCatch({
        mir_from_webr(eval_js, bad_source, "webr_bad")
        "FAIL malformed source produced a document"
      }, error = function(e) conditionMessage(e))
      final_calls <- eval_js('JSON.stringify(globalThis.__stanli_helper_calls)')
      bad <- if (!grepl("stanli_compile", bad_message, fixed = TRUE)) {
        "FAIL portable error was not reported"
      } else if (!grepl('"portable":2', final_calls, fixed = TRUE) ||
                 !grepl('"classic":0', final_calls, fixed = TRUE)) {
        "FAIL portable error retried through stanc"
      } else {
        "ok"
      }

      eval_js('delete globalThis.stanli_compile; "ok"')
      legacy_mir <- mir_from_webr(eval_js, "model {}", "webr_legacy")
      fallback_calls <- eval_js(
        'JSON.stringify(globalThis.__stanli_helper_calls)')
      fallback <- if (!startsWith(legacy_mir, "((functions_block")) {
        "FAIL absent portable export did not select legacy stanc"
      } else if (!grepl('"classic":1', fallback_calls, fixed = TRUE) ||
                 !grepl('"classic_arguments":4', fallback_calls,
                        fixed = TRUE) ||
                 !grepl('"classic_flags":["O1","debug-optimized-mir"]',
                        fallback_calls, fixed = TRUE) ||
                 !grepl('"classic_includes":[]', fallback_calls, fixed = TRUE)) {
        "FAIL legacy fallback received the wrong call shape"
      } else {
        "ok"
      }
      legacy_includes <- check_includes("((functions_block")
      c(good, portable_includes, instrumented, warning_ok, bad, fallback,
        legacy_includes)
    }, finally = restore_process())
    statuses
  `);
  const statuses = (await bundled.toJs()).values;
  const failure = statuses.find((status) => status !== 'ok');
  if (failure) {
    console.error(failure);
    process.exit(1);
  }
}

console.log(compilerOnly
  ? 'webr compiler helper OK: portable and legacy includes resolve'
  : compilerPath
  ? 'webr load OK: runtime symbols and R portable helper resolve'
  : 'webr load OK: side module loads and the bridge symbols resolve');
process.exit(0);
