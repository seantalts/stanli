// Exercise the worker's compiler selection without loading the WASM runtime.
// importScripts is replaced with a loader for the actual generated compiler
// artifacts; the compile command needs only the runtime's resolved promise.
//
//   node tests/test_worker_compiler.cjs custom worker.js portable.js stock.js
//   node tests/test_worker_compiler.cjs fallback worker.js portable.js stock.js
"use strict";

const fs = require("fs");
const path = require("path");

const mode = process.argv[2];
const workerPath = path.resolve(process.argv[3]);
const portablePath = path.resolve(process.argv[4]);
const stockPath = path.resolve(process.argv[5]);

if (mode !== "custom" && mode !== "fallback") {
  console.error("mode must be custom or fallback");
  process.exit(2);
}

const imports = [];
const messages = [];

globalThis.importScripts = (asset) => {
  imports.push(asset);
  if (asset === "stanli.js") {
    globalThis.createStanli = () => Promise.resolve({});
    return;
  }
  if (asset === "stanli-compiler.js") {
    if (mode === "fallback") throw new Error("portable compiler unavailable");
    Object.assign(globalThis, require(portablePath));
    return;
  }
  if (asset === "stancjs.bc.js") {
    Object.assign(globalThis, require(stockPath));
    return;
  }
  throw new Error("unexpected importScripts asset: " + asset);
};
globalThis.postMessage = (message) => messages.push(message);
// DedicatedWorkerGlobalScope provides this binding before the worker script
// runs. Define the corresponding Node global for the harness.
globalThis.onmessage = null;

require(workerPath);

const code = fs.readFileSync(
    path.join(__dirname, "fixtures", "es.stan"), "utf8");

const fastCode = `
data { int<lower=0> N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> sigma; }
model { y ~ normal(a + b * x, sigma); }`;

Promise.resolve(globalThis.onmessage({data: {cmd: "compile", code}}))
    .then(() => {
      const completed = messages.filter((message) => message.done);
      if (completed.length !== 1 || typeof completed[0].done.mir !== "string")
        throw new Error("worker did not return one compiled MIR document");

      const mir = completed[0].done.mir;
      const portable = mir.startsWith("STANLI2:");
      if ((mode === "custom") !== portable)
        throw new Error(mode + " selected the wrong MIR producer");

      const expected = mode === "custom" ?
          ["stanli.js", "stanli-compiler.js"] :
          ["stanli.js", "stanli-compiler.js", "stancjs.bc.js"];
      if (JSON.stringify(imports) !== JSON.stringify(expected))
        throw new Error("unexpected import order: " + JSON.stringify(imports));

      return globalThis.onmessage({data: {cmd: "compile", code: fastCode}})
          .then(() => globalThis.onmessage(
              {data: {cmd: "compile", code: fastCode, fastMath: true}}));
    })
    .then(() => {
      const results = messages.filter((message) => message.done || message.error)
                          .slice(1);
      if (results.length !== 2) throw new Error("expected two more replies");
      if (mode === "fallback") {
        if (results[0].error || !results[1].error ||
            !/fastMath/.test(results[1].error))
          throw new Error("fastMath without the portable compiler must fail " +
                          "naming fastMath: " + JSON.stringify(results[1]).slice(0, 200));
        console.log("test_worker_compiler " + mode + " OK");
        return;
      }
      const [plain, fast] = results.map((message) => message.done);
      if (!plain || !fast) throw new Error("fast compile failed: " +
                                           JSON.stringify(results).slice(0, 200));
      if (plain.fastMath !== false || fast.fastMath !== true)
        throw new Error("compile did not report fastMath");
      if (!fast.mir.startsWith("STANLI2:") || fast.mir === plain.mir)
        throw new Error("fastMath did not change the compiled MIR");
      console.log("test_worker_compiler " + mode + " OK");
    })
    .catch((error) => {
      console.error("FAIL " + String(error && error.stack || error));
      process.exit(1);
    });
