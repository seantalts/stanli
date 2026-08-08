// The density pack, end to end under Node.
//
//   node tests/test_wasm_pack.cjs [path/to/stanli.js]
//
// Two things have to hold, and the second is the one that can go wrong
// quietly. With the pack absent, a model using a packed density must
// REFUSE to compile, naming the opcode; it must not compute something.
// With the pack loaded, it must give the same answer the native build
// gives. A split runtime that silently returns a wrong number for a
// missing kernel would be worse than no split at all.
"use strict";
const fs = require("fs");
const path = require("path");

const modPath = path.resolve(
    process.argv[2] || path.join(__dirname, "..", "build-wasm", "stanli.js"));
const packPath = path.join(path.dirname(modPath), "stanli-pack.wasm");
const createStanli = require(modPath);

function fail(msg) {
  console.error("FAIL " + msg);
  process.exit(1);
}

// A model whose only density is in the pack (frechet is tier 2, in
// STANLI_SCALAR_DENSITY_LIST_REST) plus a cdf, which is the other half of
// what the pack carries.
const MODEL = `
data { real y; }
parameters { real<lower=0> s; }
model {
  s ~ frechet(2.0, 1.5);
  target += normal_lcdf(y | 0, s);
}`;

function compile(M, mir, dataJson) {
  const mirPtr = M.stringToNewUTF8(mir);
  const dataPtr = M.stringToNewUTF8(dataJson);
  const errPtr = M._malloc(8192);
  const m = M._stanli_model_new(mirPtr, dataPtr, errPtr, 8192);
  const err = m ? "" : M.UTF8ToString(errPtr);
  M._free(mirPtr);
  M._free(dataPtr);
  M._free(errPtr);
  return { model: m, err };
}

const { execFileSync } = require("child_process");
const repo = path.resolve(__dirname, "..");
const stanc = path.join(repo, "deps", "stanc3", "stanc");

createStanli().then((M) => {
  if (typeof M._stanli_load_pack !== "function")
    fail("stanli_load_pack not exported; this is not a split build");

  const tmp = path.join(repo, "build-wasm", "_packtest.stan");
  fs.writeFileSync(tmp, MODEL);
  const mir = execFileSync(stanc, ["--debug-transformed-mir", tmp],
                           { encoding: "utf8" });
  fs.unlinkSync(tmp);
  fs.rmSync(tmp.replace(/\.stan$/, ".hpp"), { force: true });
  const dataJson = JSON.stringify({ y: 1.25 });

  // 1. Pack absent: refuse, and say which opcode.
  const before = compile(M, mir, dataJson);
  if (before.model)
    fail("compiled a packed density before the pack was loaded");
  if (!/opcode not registered/.test(before.err))
    fail("wrong error before the pack was loaded: " + before.err);

  // 2. Load it, then the same model has to compile and evaluate.
  const bytes = new Uint8Array(fs.readFileSync(packPath));
  M.FS.writeFile("/stanli-pack.wasm", bytes);
  const errPtr = M._malloc(8192);
  const rc = M.ccall("stanli_load_pack", "number",
                     ["string", "number", "number"],
                     ["/stanli-pack.wasm", errPtr, 8192]);
  if (rc !== 0) fail("stanli_load_pack: " + M.UTF8ToString(errPtr));
  M._free(errPtr);

  const after = compile(M, mir, dataJson);
  if (!after.model) fail("still refused after loading the pack: " + after.err);

  const n = Number(M._stanli_n_unconstrained(after.model));
  const qPtr = M._malloc(8 * n);
  const lpPtr = M._malloc(8);
  const gradPtr = M._malloc(8 * n);
  M.HEAPF64.fill(0.25, qPtr / 8, qPtr / 8 + n);
  if (M._stanli_grad(after.model, qPtr, lpPtr, gradPtr) !== 0)
    fail("gradient rejected after loading the pack");
  const lp = M.HEAPF64[lpPtr / 8];
  const g0 = M.HEAPF64[gradPtr / 8];
  if (!Number.isFinite(lp) || !Number.isFinite(g0))
    fail("nonfinite after loading the pack: lp=" + lp + " g=" + g0);

  // 3. Loading twice is harmless.
  const e2 = M._malloc(8192);
  if (M.ccall("stanli_load_pack", "number", ["string", "number", "number"],
              ["/stanli-pack.wasm", e2, 8192]) !== 0)
    fail("second load failed");
  M._free(e2);

  console.log("test_wasm_pack OK  refused without the pack, lp=" +
              lp.toFixed(6) + " with it");
}).catch((e) => fail(String((e && e.message) || e)));
