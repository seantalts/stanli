// Gradient throughput for the WASM build, under Node.
//
//   node tools/bench_wasm.cjs [--module build-wasm/stanli.js]
//                             [--iters N] [--filter SUBSTR]
//
// The native benchmarks (tools/bench_grad.cpp, docs/benchmarks.md) say
// nothing about the browser build: it is a different compiler, a different
// libm, and no SIMD unless someone turned it on. Any claim about making
// the browser faster has to be measured here, on the models people
// actually run, or it is a guess about a machine we are not testing.
//
// Reports nanoseconds per gradient per model, and a geometric mean, so two
// builds can be diffed. Same MIR fixtures the tests use, so no stanc and
// no posteriordb checkout are needed.
"use strict";
const fs = require("fs");
const path = require("path");

const repo = path.resolve(__dirname, "..");
const args = process.argv.slice(2);
let modPath = path.join(repo, "build-wasm", "stanli.js");
let iters = 0;  // 0 = auto-calibrate to ~1s per model
let filter = "";
// --mir/--data time a model that is not a fixture, which is how a big
// vectorized shape (radon_pooled and friends) gets measured without
// making this tool depend on a posteriordb checkout.
let adhocMir = null, adhocData = null;
for (let i = 0; i < args.length; ++i) {
  if (args[i] === "--module") modPath = path.resolve(args[++i]);
  else if (args[i] === "--iters") iters = parseInt(args[++i], 10);
  else if (args[i] === "--filter") filter = args[++i];
  else if (args[i] === "--mir") adhocMir = path.resolve(args[++i]);
  else if (args[i] === "--data") adhocData = path.resolve(args[++i]);
}

// (fixture stem, data file or null). Chosen to span the shapes the
// browser sees: a vectorized density, a matrix product, a scalar
// recurrence, and a hierarchical model with transformed parameters.
const MODELS = [
  ["es", "eight_schools.json"],
  ["ar1", "ar1.json"],
  ["conj", "conj.json"],
];

const createStanli = require(modPath);

function loadAdhoc(M) {
  const mir = fs.readFileSync(adhocMir, "utf8");
  const data = adhocData ? fs.readFileSync(adhocData, "utf8") : "{}";
  const mirPtr = M.stringToNewUTF8(mir);
  const dataPtr = M.stringToNewUTF8(data);
  const errPtr = M._malloc(8192);
  const model = M._stanli_model_new(mirPtr, dataPtr, errPtr, 8192);
  M._free(mirPtr);
  M._free(dataPtr);
  if (!model) {
    const msg = M.UTF8ToString(errPtr);
    M._free(errPtr);
    throw new Error("adhoc: " + msg);
  }
  M._free(errPtr);
  return model;
}

function loadModel(M, stem, dataFile) {
  const fixtures = path.join(repo, "tests", "fixtures");
  const mir = fs.readFileSync(path.join(fixtures, stem + ".tmir.sexp"), "utf8");
  const data = dataFile
      ? fs.readFileSync(path.join(fixtures, dataFile), "utf8")
      : "{}";
  const mirPtr = M.stringToNewUTF8(mir);
  const dataPtr = M.stringToNewUTF8(data);
  const errPtr = M._malloc(8192);
  const model = M._stanli_model_new(mirPtr, dataPtr, errPtr, 8192);
  M._free(mirPtr);
  M._free(dataPtr);
  if (!model) {
    const msg = M.UTF8ToString(errPtr);
    M._free(errPtr);
    throw new Error(stem + ": " + msg);
  }
  M._free(errPtr);
  return model;
}

createStanli().then((M) => {
  const rows = [];
  const work = adhocMir ? [[path.basename(adhocMir, ".sexp"), null]] : MODELS;
  for (const [stem, dataFile] of work) {
    if (filter && !stem.includes(filter)) continue;
    let model;
    try {
      model = adhocMir ? loadAdhoc(M) : loadModel(M, stem, dataFile);
    } catch (e) {
      console.log(stem.padEnd(14) + "SKIP " + e.message.slice(0, 60));
      continue;
    }
    const n = Number(M._stanli_n_unconstrained(model));
    const qPtr = M._malloc(8 * n);
    const lpPtr = M._malloc(8);
    const gradPtr = M._malloc(8 * n);
    // A fixed, non-zero point: zeros can sit exactly on a branch boundary
    // and is not where a sampler spends its time.
    for (let i = 0; i < n; ++i)
      M.HEAPF64[qPtr / 8 + i] = 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3);

    const run = (k) => {
      for (let j = 0; j < k; ++j) M._stanli_grad(model, qPtr, lpPtr, gradPtr);
    };
    run(200);  // warm the JIT and the wasm tier-up before timing anything
    let k = iters;
    if (!k) {
      const t = process.hrtime.bigint();
      run(200);
      const per = Number(process.hrtime.bigint() - t) / 200;
      k = Math.max(200, Math.min(200000, Math.round(1e9 / per)));
    }
    const t0 = process.hrtime.bigint();
    run(k);
    const ns = Number(process.hrtime.bigint() - t0) / k;
    rows.push([stem, n, ns]);
    M._free(qPtr);
    M._free(lpPtr);
    M._free(gradPtr);
    M._stanli_model_free(model);
  }

  console.log("model".padEnd(14) + "params".padStart(8) +
              "ns/gradient".padStart(14));
  let logsum = 0;
  for (const [stem, n, ns] of rows) {
    console.log(stem.padEnd(14) + String(n).padStart(8) +
                ns.toFixed(0).padStart(14));
    logsum += Math.log(ns);
  }
  if (rows.length)
    console.log("geomean".padEnd(14) + "".padStart(8) +
                Math.exp(logsum / rows.length).toFixed(0).padStart(14));
});
