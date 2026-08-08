// stanli_check's contract, driven through the WASM build under Node:
//   OK <lp> <g0> <g1> ...   / COMPILE_FAIL ... / EVAL_FAIL ...
// EVAL_FAIL means evaluation threw; a nonfinite lp or gradient is a
// value and gets printed, exactly as stanli_check and ref_driver do.
// stanc runs natively (Node spawns the same pinned binary); the model
// compiles and evaluates inside stanli.wasm. This is what lets
// tools/verify_refs.py replay the corpus references against the browser
// build: tools/wasm_check.sh adapts the argv.
//
//   node tools/wasm_check.cjs model.stan data.json [--point N]
//
// The WASM C ABI has no write_array entry point yet, so --wa-values
// reports failure; replay wa-carrying models with --no-wa.
"use strict";
const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const repo = path.resolve(__dirname, "..");
const args = process.argv.slice(2);
const model = args[0], dataFile = args[1];
let point = 0, waValues = false;
for (let i = 2; i < args.length; ++i) {
  if (args[i] === "--point") point = parseInt(args[++i], 10);
  else if (args[i] === "--wa-values") waValues = true;
}

// Same deterministic points as stanli_check.cpp / ref_driver.cpp.
function evalPoint(i, variant) {
  switch (variant) {
    case 1: return 0.02 * ((i % 5) - 2);
    case 2: return 0.0;
    default: return 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3);
  }
}

let mir;
try {
  mir = execFileSync(path.join(repo, "deps", "stanc3", "stanc"),
                     ["--debug-transformed-mir", model],
                     { maxBuffer: 1 << 28, encoding: "utf8" });
} catch (e) {
  console.log("COMPILE_FAIL stanc: " + String(e.message).split("\n")[0]);
  process.exit(1);
}
const data = fs.readFileSync(dataFile, "utf8");

// $STANLI_WASM_MODULE points the replay at another build tree, which is
// how two wasm configurations get compared against the same references.
const createStanli = require(process.env.STANLI_WASM_MODULE
    ? path.resolve(process.env.STANLI_WASM_MODULE)
    : path.join(repo, "build-wasm", "stanli.js"));
createStanli().then((M) => {
  const mirPtr = M.stringToNewUTF8(mir);
  const dataPtr = M.stringToNewUTF8(data);
  const errLen = 8192;
  const errPtr = M._malloc(errLen);
  const m = M._stanli_model_new(mirPtr, dataPtr, errPtr, errLen);
  if (!m) {
    console.log("COMPILE_FAIL " +
                M.UTF8ToString(errPtr).split("\n")[0]);
    process.exit(1);
  }
  const n = Number(M._stanli_n_unconstrained(m));
  const qPtr = M._malloc(8 * n);
  const lpPtr = M._malloc(8);
  const gradPtr = M._malloc(8 * n);
  for (let i = 0; i < n; ++i) M.HEAPF64[qPtr / 8 + i] = evalPoint(i, point);
  if (M._stanli_grad(m, qPtr, lpPtr, gradPtr) !== 0) {
    console.log("EVAL_FAIL evaluation threw");
    process.exit(1);
  }
  // Nonfinite values are reported, not refused -- see the note in
  // tools/stanli_check.cpp. This driver has to keep the same contract as
  // that one or the WASM replay stops being the same oracle.
  const lp = M.HEAPF64[lpPtr / 8];
  let nBad = 0;
  const parts = ["OK", fmt(lp)];
  for (let i = 0; i < n; ++i) {
    const g = M.HEAPF64[gradPtr / 8 + i];
    if (!Number.isFinite(g)) ++nBad;
    parts.push(fmt(g));
  }
  if (!Number.isFinite(lp) || nBad > 0)
    console.error("wasm_check: nonfinite lp=" + (Number.isFinite(lp) ? 0 : 1) +
                  " gradients=" + nBad);
  console.log(parts.join(" "));
  if (waValues) {
    const nWa = Number(M._stanli_wa_n_columns(m));
    if (nWa <= 0) {
      console.log("WANAMES FAIL no write_array\nWAVALS FAIL");
    } else {
      const names = [];
      for (let i = 0; i < nWa; ++i)
        names.push(M.UTF8ToString(M._stanli_wa_column_name(m, BigInt(i))));
      M._stanli_wa_seed(m, 1234);
      const waPtr = M._malloc(8 * nWa);
      if (M._stanli_wa_row(m, qPtr, waPtr) !== 0) {
        console.log("WANAMES FAIL wa_row failed\nWAVALS FAIL");
      } else {
        const vals = [];
        for (let i = 0; i < nWa; ++i) vals.push(fmt(M.HEAPF64[waPtr / 8 + i]));
        console.log("WANAMES " + names.join(","));
        console.log("WAVALS " + vals.join(" "));
      }
    }
  }
  process.exitCode = 0;
}).catch((e) => {
  console.log("EVAL_FAIL " + String(e).split("\n")[0]);
  process.exit(1);
});

// %.17g equivalent: shortest round-trip representation, which is what
// Number.prototype.toString gives for doubles.
function fmt(x) {
  return String(x);
}
