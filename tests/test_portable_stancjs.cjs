// Cross-toolchain contract for the stanli-owned compiler: native OCaml and
// js_of_ocaml must emit exactly the same canonical bytes.
//
//   node tests/test_portable_stancjs.cjs \
//     path/to/stanli-compiler.js path/to/stanli_compiler_cli.exe \
//     [path/to/stock-stancjs.bc.js]
"use strict";
const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const zlib = require("zlib");

const repo = path.resolve(__dirname, "..");
const compilerPath = path.resolve(process.argv[2]);
const nativePath = path.resolve(process.argv[3]);
const fallbackPath = process.argv[4] && path.resolve(process.argv[4]);

function fail(message) {
  console.error("FAIL " + message);
  process.exit(1);
}

function portablePayload(encoded, name) {
  if (!encoded.startsWith("STANLI2:"))
    fail(name + ": missing compact portable-MIR envelope");
  const text = encoded.slice(8);
  const payload = Buffer.from(text, "base64");
  if (payload.toString("base64") !== text)
    fail(name + ": non-canonical base64 payload");
  return payload;
}

const exported = require(compilerPath);
const compile =
    (exported && exported.stanli_compile) || globalThis.stanli_compile;
const compileModel =
    (exported && exported.stanli_compile_model) || globalThis.stanli_compile_model;
if (typeof compile !== "function") fail("no stanli_compile() export");
if (typeof compileModel !== "function") fail("no stanli_compile_model() export");
const compatibleStanc = (exported && exported.stanc) || globalThis.stanc;
if (typeof compatibleStanc !== "function") fail("no compatible stanc() export");
const customVersion = compatibleStanc("version-test", "", ["version"]);
if (customVersion.errors ||
    /%%(?:NAME|VERSION)%%/.test(String(customVersion.result)))
  fail("stanli compiler has unsubstituted version metadata");

// importScripts executes a classic script with no CommonJS module. Evaluate
// the generated artifact under those conditions as well as through require().
const classic = {console};
vm.createContext(classic);
vm.runInContext(fs.readFileSync(compilerPath, "utf8"), classic, {
  filename: compilerPath,
});
if (typeof classic.stanli_compile !== "function" ||
    typeof classic.stanc !== "function")
  fail("classic-script compiler did not export both entry points");
const classicResult = classic.stanli_compile(
    "embedded_model", "parameters { real x; } model { x ~ std_normal(); }");
if (classicResult.errors ||
    !String(classicResult.result).startsWith("STANLI2:"))
  fail("classic-script portable compilation failed");

let fallback = null;
if (fallbackPath) {
  const fallbackExported = require(fallbackPath);
  fallback = (fallbackExported && fallbackExported.stanc) || globalThis.stanc;
  if (typeof fallback !== "function") fail("no fallback stanc() export");
  const version = fallback("version-test", "", ["version"]);
  if (version.errors || /%%(?:NAME|VERSION)%%/.test(String(version.result)))
    fail("fallback stancjs has unsubstituted version metadata");
}

const models = [
  ["ordinary", "tests/fixtures/es.stan"],
  ["nested-udf", "tests/fixtures/view_udf_local_data_branch.stan"],
  ["loop-control", "tests/fixtures/paramcond_break.stan"],
  ["vectorized-loop", "tests/compiler/portable_vectorize_loop.stan"],
  ["full-span-assignment",
    "tests/compiler/portable_full_span_assignment.stan"],
  ["same-lane-density-loop",
    "tests/compiler/portable_same_lane_density_loop.stan"],
  ["mother", "tests/stanc3/mother.stan"],
  ["folded-float", "tests/compiler/portable_folded_float.stan"],
  ["int32-overflow", "tests/compiler/portable_int32_overflow.stan"],
  ["unicode", "tests/compiler/portable_unicode.stan"],
  ["include", "tests/compiler/portable_include.stan", {
    "portable_include_functions.stan": fs.readFileSync(
        path.join(repo, "tests/compiler/portable_include_functions.stan"),
        "utf8"),
  }],
];
const measurements = [];

for (const [name, relative, includes] of models) {
  const model = path.join(repo, relative);
  const code = fs.readFileSync(model, "utf8");
  const native = execFileSync(nativePath, [model], {
    maxBuffer: 1 << 28,
  });
  const js = compile("embedded_model", code, includes);
  const nativeModel = execFileSync(nativePath, ["--model-only", model], {
    maxBuffer: 1 << 28,
  });
  const jsModel = compileModel("embedded_model", code, includes);
  if (jsModel.errors || !Buffer.from(String(jsModel.result)).equals(nativeModel))
    fail(name + ": model-only native/JS bytes differ");
  if (js.errors) fail(name + ": " + Array.from(js.errors).join("\n"));
  const encoded = String(js.result);
  const payload = portablePayload(encoded, name);
  if (encoded.endsWith("\n") || encoded.endsWith("\r"))
    fail(name + ": trailing newline");
  if (name === "folded-float" &&
      !payload.includes(Buffer.from("343333333333d33f", "hex")))
    fail(name + ": O1 did not preserve the folded 0.1 + 0.2 bit pattern");
  if (name === "int32-overflow" &&
      (!payload.includes(Buffer.from([0x80, 0x38, 0x01, 0x00])) ||
       !payload.includes(Buffer.from("Times__", "utf8")) ||
       !payload.includes(Buffer.from("IntDivide__", "utf8"))))
    fail(name + ": checked integer folding policy changed");
  const jsBytes = Buffer.from(encoded, "utf8");
  if (!jsBytes.equals(native)) {
    const length = Math.min(jsBytes.length, native.length);
    let at = 0;
    while (at < length && jsBytes[at] === native[at]) ++at;
    fail(name + ": native/JS bytes differ at byte " + at);
  }
  const repeated = compile("embedded_model", code, includes);
  if (repeated.errors || String(repeated.result) !== encoded)
    fail(name + ": repeated JS compilation changed bytes");

  let legacyBytes = null;
  let legacyGzipBytes = null;
  if (fallback) {
    const legacy = fallback(
        "embedded_model", code, ["O1", "debug-optimized-mir"], includes);
    if (legacy.errors)
      fail(name + " fallback: " + Array.from(legacy.errors).join("\n"));
    const legacyText = String(legacy.result);
    legacyBytes = Buffer.byteLength(legacyText);
    legacyGzipBytes = zlib.gzipSync(legacyText).length;

    // The checked overflow policy deliberately differs from pristine stanc3:
    // it retains an expression whose native and JavaScript host-int folds
    // disagree. All ordinary compatible-stanc inputs must remain identical.
    if (name !== "int32-overflow") {
      const compatible = compatibleStanc(
          "embedded_model", code, ["O1", "debug-optimized-mir"], includes);
      if (compatible.errors || String(compatible.result) !== legacyText)
        fail(name + ": custom stanc() differs from stock fallback");
    }
  }
  measurements.push({
    model: name,
    portable_bytes: Buffer.byteLength(encoded),
    portable_gzip_bytes: zlib.gzipSync(encoded).length,
    legacy_bytes: legacyBytes,
    legacy_gzip_bytes: legacyGzipBytes,
  });
}

const warningCode =
    "parameters { real x; } model { if (0 < x < 1) target += 0; }";
const portableWarning = compile("embedded_model", warningCode);
const referenceStanc = fallback || compatibleStanc;
const stockWarning = referenceStanc(
    "embedded_model", warningCode, ["O1", "debug-optimized-mir"]);
if (portableWarning.errors || stockWarning.errors ||
    JSON.stringify(Array.from(portableWarning.warnings)) !==
        JSON.stringify(Array.from(stockWarning.warnings)) ||
    portableWarning.warnings.length === 0)
  fail("portable warning output differs from stanc()");

const badCode = "parameters { real x } model {} ";
const bad = compile("embedded_model", badCode);
if (!bad.errors || bad.result !== undefined)
  fail("malformed Stan did not return an error-only object");
const stockBad = referenceStanc(
    "embedded_model", badCode, ["O1", "debug-optimized-mir"]);
if (JSON.stringify(Array.from(bad.errors)) !==
    JSON.stringify(Array.from(stockBad.errors)) ||
    !String(bad.errors[1]).includes(badCode.trim()))
  fail("portable frontend diagnostic differs from stanc()");

console.log(JSON.stringify({portable_mir_measurements: measurements}, null, 2));
console.log("test_portable_stancjs OK");
