// The WASM build end to end under Node: load the module, compile eight
// schools from Stan source, evaluate a gradient, sample, and check the
// posterior mean of mu. Mirrors the wheel smoke test.
//
//   node tests/test_wasm.cjs [stanli.js] [stanli-compiler.js] [stancjs.bc.js]
"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");
const {spawnSync} = require("child_process");

const modPath = path.resolve(
    process.argv[2] || path.join(__dirname, "..", "build-wasm", "stanli.js"));
const createStanli = require(modPath);
const compilerPath = process.argv[3] && path.resolve(process.argv[3]);
const fallbackPath = process.argv[4] && path.resolve(process.argv[4]);

function fail(msg) {
  console.error("FAIL " + msg);
  process.exit(1);
}

function nativeMir(fixtures) {
  const executable = process.platform === "win32" ? "stanc.exe" : "stanc";
  const stanc = path.resolve(
      process.env.STANC || path.join(__dirname, "..", "deps", "stanc3",
                                    executable));
  if (!fs.existsSync(stanc))
    throw new Error("missing pinned compiler: " + stanc);
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "stanli-wasm-stanc-"));
  try {
    const source = path.join(root, "es.stan");
    fs.copyFileSync(path.join(fixtures, "es.stan"), source);
    const compiled = spawnSync(
        stanc, ["--O1", "--debug-optimized-mir", source],
        {encoding: "utf8"});
    if (compiled.error) throw compiled.error;
    if (compiled.status !== 0)
      throw new Error("stanc failed: " + (compiled.stderr || compiled.status));
    return compiled.stdout;
  } finally {
    fs.rmSync(root, {recursive: true, force: true});
  }
}

createStanli().then((M) => {
  const fixtures = path.join(__dirname, "fixtures");
  let mir;
  let portableCompile = null;
  if (compilerPath) {
    const exported = require(compilerPath);
    const portable =
        (exported && exported.stanli_compile) || globalThis.stanli_compile;
    const stanc = (exported && exported.stanc) || globalThis.stanc;
    const code = fs.readFileSync(path.join(fixtures, "es.stan"), "utf8");
    let compiled;
    if (typeof portable === "function") {
      portableCompile = portable;
      compiled = portable("embedded_model", code);
      if (!compiled.errors &&
          !String(compiled.result).startsWith("STANLI2:"))
        fail("portable stancjs returned legacy or malformed MIR");
    } else if (typeof stanc === "function") {
      compiled = stanc("es_model", code, ["O1", "debug-optimized-mir"]);
    } else {
      fail("browser compiler did not export stanli_compile() or stanc()");
    }
    if (compiled.errors)
      fail("browser compiler: " + Array.from(compiled.errors).join("\n"));
    mir = String(compiled.result);

    if (fallbackPath) {
      const fallbackExported = require(fallbackPath);
      const fallback =
          (fallbackExported && fallbackExported.stanc) || globalThis.stanc;
      if (typeof fallback !== "function")
        fail("fallback stancjs did not export stanc()");
      const version = fallback("version-test", "", ["version"]);
      if (version.errors || /%%(?:NAME|VERSION)%%/.test(String(version.result)))
        fail("fallback stancjs has unsubstituted version metadata");
    }
  } else {
    try {
      mir = nativeMir(fixtures);
    } catch (error) {
      fail(String(error));
    }
  }
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

  // Pathfinder through the entry the worker calls, including the
  // per-iterate callback: the path is the only way that data leaves the
  // service, so a silent no-op callback would be an empty plot with no
  // other symptom.
  const sumPtr = M._malloc(8 * 4);
  const climb = [];
  const cbPtr = M.addFunction((iter, plp) => climb.push([iter, plp]), "vidi");
  const rcP = M._stanli_run_pathfinder(model, 1, 1, samples, drawsPtr, 0, 0,
                                       sumPtr, cbPtr, 0, errPtr, errLen);
  M.removeFunction(cbPtr);
  if (rcP !== 0) fail("pathfinder: " + M.UTF8ToString(errPtr));
  if (climb.length < 2) fail("pathfinder path has " + climb.length + " iterates");
  if (climb[0][0] !== 0) fail("pathfinder path starts at " + climb[0][0]);
  const khat = M.HEAPF64[sumPtr / 8];
  const selected = M.HEAPF64[sumPtr / 8 + 1];
  if (!Number.isFinite(khat)) fail("pathfinder khat " + khat);
  if (!(selected >= 0 && selected < climb.length))
    fail("pathfinder selected iterate " + selected);
  let muSumP = 0;
  for (let s = 0; s < samples; ++s) {
    M._stanli_constrain(model, drawsPtr + 8 * s * n, rowPtr);
    const v = M.HEAPF64[rowPtr / 8 + muIdx];
    if (!Number.isFinite(v)) fail("pathfinder nonfinite draw " + s);
    muSumP += v;
  }
  const muP = muSumP / samples;
  const pfMs = M.HEAPF64[sumPtr / 8 + 3];
  M._free(sumPtr);

  // The web initialization handoff: Pathfinder writes one unconstrained
  // point, then the additive stream entry starts NUTS from exactly that
  // buffer while retaining sampler statistics and live-draw compatibility.
  const pfInitPtr = M._malloc(8 * n);
  const pfInitPath = [];
  const pfInitCb = M.addFunction(
      (iter, plp) => pfInitPath.push([iter, plp]), "vidi");
  const rcPI = M._stanli_pathfinder_inits(
      model, 303, 1, 1, 100, 10, 5, 2, pfInitPtr, pfInitCb, 0, errPtr,
      errLen);
  M.removeFunction(pfInitCb);
  if (rcPI !== 0)
    fail("pathfinder initialization: " + M.UTF8ToString(errPtr));
  if (!pfInitPath.length ||
      !M.HEAPF64.subarray(pfInitPtr / 8, pfInitPtr / 8 + n)
          .every(Number.isFinite))
    fail("pathfinder initialization produced no finite start");
  const initSamples = 20;
  const initStatsPtr = M._malloc(8 * initSamples * 7);
  const rcPISample = M._stanli_sample_stream_stats_init(
      model, 303, 30, initSamples, 0.8, pfInitPtr, drawsPtr, initStatsPtr, 0,
      0, errPtr, errLen);
  if (rcPISample !== 0)
    fail("sampling from Pathfinder initialization: " +
         M.UTF8ToString(errPtr));
  if (!M.HEAPF64.subarray(drawsPtr / 8, drawsPtr / 8 + initSamples * n)
          .every(Number.isFinite) ||
      !M.HEAPF64.subarray(initStatsPtr / 8, initStatsPtr / 8 + initSamples * 7)
          .every(Number.isFinite))
    fail("sampling from Pathfinder initialization produced nonfinite output");
  M._free(initStatsPtr);
  M._free(pfInitPtr);

  M._stanli_model_free(model);

  // A source-to-WASM generated-quantities smoke for the portable producer.
  // Eight schools exercises gradients and samplers but has no GQ block, so
  // compile the existing RNG fixture and run the write_array entry as well.
  if (portableCompile) {
    const gqCode = fs.readFileSync(path.join(fixtures, "gqrng.stan"), "utf8");
    const gqCompiled = portableCompile("gqrng", gqCode);
    if (gqCompiled.errors)
      fail("gqrng compiler: " + Array.from(gqCompiled.errors).join("\n"));
    const gqMirPtr = M.stringToNewUTF8(String(gqCompiled.result));
    const gqDataPtr = M.stringToNewUTF8(
        fs.readFileSync(path.join(fixtures, "gqrng.json"), "utf8"));
    const gqModel = M._stanli_model_new(
        gqMirPtr, gqDataPtr, errPtr, errLen);
    M._free(gqMirPtr);
    M._free(gqDataPtr);
    if (!gqModel) fail("gqrng model_new: " + M.UTF8ToString(errPtr));

    const gqN = Number(M._stanli_n_unconstrained(gqModel));
    const gqColumns = Number(M._stanli_wa_n_columns(gqModel));
    if (gqN !== 1 || gqColumns !== 5)
      fail("gqrng shape " + gqN + " parameters, " + gqColumns + " columns");
    const gqNames = [];
    for (let i = 0; i < gqColumns; ++i)
      gqNames.push(M.UTF8ToString(
          M._stanli_wa_column_name(gqModel, BigInt(i))));
    if (gqNames.join(",") !== "sigma,yrep,crep,branchy,p")
      fail("gqrng columns " + gqNames.join(","));

    const gqQPtr = M._malloc(8 * gqN);
    const gqRowPtr = M._malloc(8 * gqColumns);
    M.HEAPF64[gqQPtr / 8] = 0.53;
    M._stanli_wa_seed_chain(gqModel, 11, 1);
    if (M._stanli_wa_row(gqModel, gqQPtr, gqRowPtr) !== 0)
      fail("gqrng write_array failed");
    const gqRow = Array.from(
        M.HEAPF64.subarray(gqRowPtr / 8, gqRowPtr / 8 + gqColumns));
    if (!gqRow.every(Number.isFinite) ||
        !Number.isInteger(gqRow[2]) || gqRow[2] < 0 || gqRow[2] > 3 ||
        gqRow[3] !== 1 || gqRow[4] !== 6)
      fail("gqrng invalid row " + JSON.stringify(gqRow));
    M._free(gqQPtr);
    M._free(gqRowPtr);
    M._stanli_model_free(gqModel);
  }

  console.log("test_wasm OK  lp(0) = " + lp.toFixed(6) + "  mean(mu) = " +
              mu.toFixed(3) + "  walnuts mean(mu) = " + muW.toFixed(3) +
              "  pathfinder mean(mu) = " + muP.toFixed(3) +
              " in " + pfMs.toFixed(0) + " ms" +
              " (khat " + khat.toFixed(2) + ", " + climb.length + " iterates)");
}).catch((e) => fail(String(e && e.stack || e)));
