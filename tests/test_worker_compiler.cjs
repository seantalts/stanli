// Exercise the worker's compile command without loading the WASM runtime.
// importScripts is replaced with a loader for the actual generated compiler
// artifact; the compile command needs only the runtime's resolved promise.
//
//   node tests/test_worker_compiler.cjs worker.js stanli-compiler.js
"use strict";

const fs = require("fs");
const path = require("path");

const workerPath = path.resolve(process.argv[2]);
const compilerPath = path.resolve(process.argv[3]);

const imports = [];
const messages = [];

globalThis.importScripts = (asset) => {
  imports.push(asset);
  if (asset === "stanli.js") {
    globalThis.createStanli = () => Promise.resolve({});
    return;
  }
  if (asset === "stanli-compiler.js") {
    Object.assign(globalThis, require(compilerPath));
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

const warningCode =
    "parameters { real x; } model { if (0 < x < 1) target += 0; }";

// Past the compiler's structural budget, so O1 is skipped.
const budgetCode = "parameters { real x; }\nmodel {\nreal s = 0;\n" +
    Array.from({length: 9}, (_, d) => `for (i${d} in 1:1) {\n`).join("") +
    "s += 1;\n".repeat(2100) + "}\n".repeat(9) + "x ~ normal(s, 1);\n}\n";

async function reply(request) {
  const before = messages.length;
  await globalThis.onmessage({data: request});
  const results = messages.slice(before).filter((m) => m.done || m.error);
  if (results.length !== 1)
    throw new Error("expected one reply, got " + JSON.stringify(results));
  if (results[0].error) throw new Error(results[0].error);
  return results[0].done;
}

(async () => {
  const plain = await reply({cmd: "compile", code});
  if (typeof plain.mir !== "string" || !plain.mir.startsWith("STANLI4:"))
    throw new Error("worker did not return portable MIR");
  if (JSON.stringify(imports) !==
      JSON.stringify(["stanli.js", "stanli-compiler.js"]))
    throw new Error("unexpected import order: " + JSON.stringify(imports));
  if (plain.o1Fallback !== null || !Array.isArray(plain.warnings) ||
      plain.warnings.length !== 0)
    throw new Error("a small clean model reported compiler messages: " +
                    JSON.stringify([plain.warnings, plain.o1Fallback]));

  const slow = await reply({cmd: "compile", code: fastCode});
  const fast = await reply({cmd: "compile", code: fastCode, fastMath: true});
  if (slow.fastMath !== false || fast.fastMath !== true)
    throw new Error("compile did not report fastMath");
  if (!fast.mir.startsWith("STANLI4:") || fast.mir === slow.mir)
    throw new Error("fastMath did not change the compiled MIR");

  const warned = await reply({cmd: "compile", code: warningCode});
  if (warned.warnings.length !== 1 ||
      !warned.warnings[0].includes("chained comparison") ||
      warned.o1Fallback !== null)
    throw new Error("unexpected warnings: " + JSON.stringify(warned.warnings));

  const budget = await reply({cmd: "compile", code: budgetCode});
  if (typeof budget.o1Fallback !== "string" ||
      !budget.o1Fallback.includes("skipped O1"))
    throw new Error("the O1 fallback was not reported: " +
                    JSON.stringify(budget.o1Fallback));
  if (budget.warnings.some((w) => w.includes("skipped O1")))
    throw new Error("the O1 fallback notice is also in the warnings");

  console.log("test_worker_compiler OK");
})().catch((error) => {
  console.error("FAIL " + String(error && error.stack || error));
  process.exit(1);
});
