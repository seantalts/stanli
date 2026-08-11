// The WASM build end to end under Node: load the module, compile eight
// schools from its pinned MIR fixture, evaluate a gradient, sample, and
// check the posterior mean of mu. Mirrors the wheel smoke test.
//
//   node tests/test_wasm.cjs [path/to/stanli.js]
"use strict";
const fs = require("fs");
const path = require("path");

const modPath = path.resolve(
    process.argv[2] || path.join(__dirname, "..", "build-wasm", "stanli.js"));
const createStanli = require(modPath);

function fail(msg) {
  console.error("FAIL " + msg);
  process.exit(1);
}

createStanli().then((M) => {
  const fixtures = path.join(__dirname, "fixtures");
  const mir = fs.readFileSync(path.join(fixtures, "es.tmir.sexp"), "utf8");
  const data = fs.readFileSync(
      path.join(fixtures, "eight_schools.json"), "utf8");

  const mirPtr = M.stringToNewUTF8(mir);
  const dataPtr = M.stringToNewUTF8(data);
  const errLen = 8192;
  const errPtr = M._malloc(errLen);
  const model = M._stanli_model_new(mirPtr, dataPtr, errPtr, errLen);
  if (!model) fail("model_new: " + M.UTF8ToString(errPtr));
  M._free(mirPtr);
  M._free(dataPtr);

  // int64 returns arrive as BigInt (WASM_BIGINT).
  const n = Number(M._stanli_n_unconstrained(model));
  if (n !== 10) fail("n_unconstrained " + n + ", want 10");

  // One gradient at zeros: finite lp, finite gradient.
  const qPtr = M._malloc(8 * n);
  const lpPtr = M._malloc(8);
  const gradPtr = M._malloc(8 * n);
  M.HEAPF64.fill(0, qPtr / 8, qPtr / 8 + n);
  if (M._stanli_grad(model, qPtr, lpPtr, gradPtr) !== 0) fail("grad rc");
  const lp = M.HEAPF64[lpPtr / 8];
  if (!Number.isFinite(lp)) fail("nonfinite lp " + lp);
  for (let i = 0; i < n; ++i) {
    const g = M.HEAPF64[gradPtr / 8 + i];
    if (!Number.isFinite(g)) fail("nonfinite grad[" + i + "] " + g);
  }

  // Sample and check the posterior mean of mu, like the wheel smoke test.
  const warmup = 1000, samples = 1000;
  const drawsPtr = M._malloc(8 * samples * n);
  const rc = M._stanli_sample(model, 1, warmup, samples, 0.8, drawsPtr,
                              errPtr, errLen);
  if (rc !== 0) fail("sample: " + M.UTF8ToString(errPtr));

  const nCon = Number(M._stanli_n_constrained(model));
  let muIdx = -1;
  for (let i = 0; i < nCon; ++i) {
    const name = M.UTF8ToString(M._stanli_constrained_name(model, BigInt(i)));
    if (name === "mu") muIdx = i;
  }
  if (muIdx < 0) fail("no mu column");

  const rowPtr = M._malloc(8 * nCon);
  let muSum = 0;
  for (let s = 0; s < samples; ++s) {
    M._stanli_constrain(model, drawsPtr + 8 * s * n, rowPtr);
    muSum += M.HEAPF64[rowPtr / 8 + muIdx];
  }
  const mu = muSum / samples;
  if (!(mu > 3.0 && mu < 6.0)) fail("mu " + mu + " outside (3, 6)");

  // WALNUTS through the same streaming C entry the worker uses: same
  // posterior, so the same window on mean(mu).
  const rcW = M._stanli_sample_walnuts_stream(model, 1, warmup, samples, 0,
                                              drawsPtr, 0, 0, errPtr, errLen);
  if (rcW !== 0) fail("walnuts: " + M.UTF8ToString(errPtr));
  let muSumW = 0;
  for (let s = 0; s < samples; ++s) {
    M._stanli_constrain(model, drawsPtr + 8 * s * n, rowPtr);
    muSumW += M.HEAPF64[rowPtr / 8 + muIdx];
  }
  const muW = muSumW / samples;
  if (!(muW > 3.0 && muW < 6.0)) fail("walnuts mu " + muW + " outside (3, 6)");

  M._stanli_model_free(model);
  console.log("test_wasm OK  lp(0) = " + lp.toFixed(6) + "  mean(mu) = " +
              mu.toFixed(3) + "  walnuts mean(mu) = " + muW.toFixed(3));
}).catch((e) => fail(String(e && e.stack || e)));
