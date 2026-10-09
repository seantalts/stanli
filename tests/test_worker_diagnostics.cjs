// Run the public JS API and actual worker against WASM, with a minimal Node
// Worker adapter. No sampler/diagnostic mocks: this checks transfers, packing,
// chain aggregation, parameter attribution, and unavailable-data handling.
// node tests/test_worker_diagnostics.cjs stanli.js stanli-compiler.js [index.mjs]
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { fileURLToPath, pathToFileURL } = require("node:url");
const createStanli = require(path.resolve(process.argv[2]));
const compiler = require(path.resolve(process.argv[3]));

globalThis.Worker = class {
  constructor(url) {
    const context = vm.createContext({
      performance, onmessage: null,
      importScripts: (asset) => {
        if (asset === "stanli.js") context.createStanli = createStanli;
        else if (asset === "stanli-compiler.js") Object.assign(context, compiler);
        else throw new Error("Unexpected worker asset: " + asset);
      },
      postMessage: (message, transfer = []) => {
        const data = structuredClone(message, { transfer });
        queueMicrotask(() => this.onmessage?.({ data }));
      },
    });
    vm.runInContext(fs.readFileSync(fileURLToPath(url), "utf8"), context);
    this.context = context;
  }
  postMessage(message) {
    this.context.onmessage({ data: structuredClone(message) }).catch((error) => {
      this.onerror?.(error);
    });
  }
};

(async () => {
  const wrapper = path.resolve(process.argv[4] || "js/index.mjs");
  const { compile, sample, diagnose } = await import(pathToFileURL(wrapper));
  const { mir } = await compile({ code: `
    parameters { real x; real y; }
    model { x ~ normal(0, 1); y ~ normal(10, 2); }` });
  const live = [];
  const fits = await Promise.all([1, 2, 3, 4].map((seed) => sample({
    mir, seed, warmup: 500, samples: 1000,
    onLive: seed === 1 ? (message) => live.push(message) : undefined,
  })));
  for (const fit of fits) {
    assert.equal(fit.sampler, "nuts");
    assert.equal(fit.generatedStart, 2);
    assert.equal(fit.maxDepth, 10);
    assert.equal(fit.samplerStats.length, 7000);
    assert(fit.samplerStats.every(Number.isFinite));
  }
  assert(live.some((m) => m.liveMeta));
  assert(live.some((m) => m.live?.phase === "warmup"));
  const rows = live.filter((m) => m.live?.rows).flatMap((m) =>
    Array.from(new Float64Array(m.live.rows)));
  for (let s = 0; s < fits[0].samples; ++s)
    fits[0].names.forEach((name, j) =>
      assert.equal(rows[s * fits[0].names.length + j], fits[0].columns[name][s]));

  const saved = structuredClone(fits);
  const text = await diagnose(fits);
  assert.match(text, /No divergent transitions/);
  assert.match(text, /No transitions saturated the maximum treedepth of 10/);
  assert.match(text, /E-BFMI is above 0.3 in every chain/);
  assert.match(text, /R-hat is below 1.01/);
  assert.match(text, /Bulk ESS is at least 100 per chain/);
  assert.match(text, /Tail ESS is at least 100 per chain/);
  assert.match(text, /No problems detected/);
  assert.deepEqual(fits, saved, "diagnosis must not detach or mutate fit data");

  // Failures in different chains/columns catch transpose errors and ensure
  // warnings count all chains, not just the first or last completed worker.
  const bad = structuredClone(fits);
  for (let c = 0; c < bad.length; ++c)
    for (let s = 0; s < bad[c].samples; ++s) {
      bad[c].columns.y[s] += c * 100;
      bad[c].samplerStats[s * 7 + 6] = s;
    }
  bad[0].samplerStats[5] = 1;
  bad[2].samplerStats[5] = 1;
  bad[3].samplerStats[12] = 1;
  bad[1].samplerStats[3] = 10;
  bad[3].samplerStats[10] = 11;
  const warning = await diagnose(bad);
  assert.match(warning, /3 of 4000 transitions .* diverged/);
  assert.match(warning, /2 of 4000 transitions saturated the maximum treedepth of 10/);
  assert.match(warning, /E-BFMI is below 0.3 in 4 of 4 chains/);
  assert.match(warning, /R-hat reaches .* \(y\)/);
  assert.doesNotMatch(warning, /No problems detected/);

  const short = { ...fits[0], samples: 1,
    columns: { x: Float64Array.of(1), y: Float64Array.of(2) },
    samplerStats: fits[0].samplerStats.slice(0, 7) };
  const incomplete = await diagnose(short);
  assert.match(incomplete, /E-BFMI is unavailable/);
  assert.match(incomplete, /R-hat is unavailable/);
  assert.doesNotMatch(incomplete, /No problems detected/);

  for (const invalid of [[], [fits[0], short],
    [{ ...fits[0], samplerStats: null }],
    [{ ...fits[0], names: ["missing"] }],
    [{ ...fits[0], sampler: "walnuts" }],
    [{ ...fits[0], sampler: "pathfinder" }]])
    await assert.rejects(diagnose(invalid), /diagnose requires/);

  for (const sampler of ["walnuts", "pathfinder"]) {
    const fit = await sample({ mir, sampler, warmup: 100, samples: 100 });
    assert.equal(fit.sampler, sampler);
    assert.equal(fit.samplerStats, null);
    await assert.rejects(diagnose(fit), /diagnose requires/);
  }
  const gq = await sample({ code: `
    parameters { real x; }
    model { x ~ normal(0, 1); }
    generated quantities { real doubled = 2 * x; }`,
    warmup: 100, samples: 100 });
  assert.equal(gq.generatedStart, 1);
  assert.equal(gq.names.length, 2);
  assert.deepEqual(gq.columns.doubled, gq.columns.x.map((x) => 2 * x));
  assert.match(await diagnose(gq), /treedepth of 10/);

  const regression = `
    data { int<lower=0> N; vector[N] x; vector[N] y; }
    parameters { real a; real b; real<lower=0> sigma; }
    model { y ~ normal(a + b * x, sigma); }`;
  const regressionData = { N: 4, x: [1, 2, 3, 4], y: [1.1, 1.9, 3.2, 3.9] };
  const base = { data: regressionData, warmup: 200, samples: 200, seed: 5 };
  const plain = await compile({ code: regression });
  const quick = await compile({ code: regression, fastMath: true });
  assert.equal(plain.fastMath, false);
  assert.equal(quick.fastMath, true);
  assert.notEqual(quick.mir, plain.mir);
  assert.equal((await compile({ code: regression, fastMath: false })).mir,
               plain.mir);

  const unset = await sample({ ...base, mir: plain.mir });
  const off = await sample({ ...base, mir: plain.mir, fastMath: false });
  assert.equal(unset.fastMath, false);
  assert.equal(off.fastMath, false);
  for (const name of unset.names)
    assert.deepEqual(off.columns[name], unset.columns[name]);

  const fromMir = await sample({ ...base, mir: quick.mir, fastMath: true });
  const fromCode = await sample({ ...base, code: regression, fastMath: true });
  for (const fit of [fromMir, fromCode]) {
    assert.equal(fit.fastMath, true);
    assert.deepEqual(fit.names, unset.names);
    assert(fit.samplerStats.every(Number.isFinite));
    for (const name of fit.names) assert(fit.columns[name].every(Number.isFinite));
  }
  for (const name of fromMir.names)
    assert.deepEqual(fromCode.columns[name], fromMir.columns[name]);
  for (const bad of [1, "yes"])
    assert.throws(() => sample({ ...base, mir: plain.mir, fastMath: bad }),
                  /fastMath must be true or false/);
  assert.throws(() => compile({ code: regression, fastMath: 1 }),
                /fastMath must be true or false/);

  console.log("test_worker_diagnostics OK: clean, failing, short, and unsupported fits; fast mode");
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
