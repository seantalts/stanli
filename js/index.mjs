// stanli: full Stan in the browser. stanc3 (compiled to JS) turns Stan
// source into MIR, and a WASM build of the stanli runtime lowers it to an
// op graph and runs NUTS. Everything happens client side, off the main
// thread, in workers this module owns.
//
//   import { compile, sample } from "stanli";
//   const { mir } = await compile({ code });
//   const fits = await Promise.all([1, 2, 3, 4].map((c) =>
//       sample({ mir, data, seed: c })));
//
// Workers pool up to the hardware's concurrency, so independent calls --
// one chain each -- run simultaneously. Compiling once and passing `mir`
// keeps the 2.8 MB compiler in a single worker (or out of the page
// entirely, if the model was precompiled at build time with
// stanc --debug-transformed-mir).

const pool = [];
const waiters = [];
const MAX_WORKERS = Math.max(
    2, Math.min(8, ((typeof navigator !== "undefined" &&
                     navigator.hardwareConcurrency) || 4)));

function acquire() {
  let slot = pool.find((s) => !s.busy);
  if (!slot && pool.length < MAX_WORKERS) {
    slot = { worker: new Worker(new URL("./worker.js", import.meta.url)),
             busy: false };
    pool.push(slot);
  }
  if (slot) {
    slot.busy = true;
    return Promise.resolve(slot);
  }
  return new Promise((res) => waiters.push(res));
}

function release(slot) {
  const next = waiters.shift();
  if (next) next(slot);  // handed over still busy
  else slot.busy = false;
}

function request(msg, opts) {
  return acquire().then((slot) => {
    const w = slot.worker;
    return new Promise((resolve, reject) => {
      w.onmessage = (e) => {
        const m = e.data;
        if (m.status) {
          if (opts.onProgress) opts.onProgress(m.status);
          return;
        }
        if (m.liveMeta || m.live) {
          if (opts.onLive) opts.onLive(m);
          return;
        }
        if (m.error) reject(new Error(m.error));
        else resolve(m.done);
      };
      w.onerror = (e) => {
        reject(new Error("stanli worker: " + (e.message ||
                                              "failed to load")));
      };
      w.postMessage(msg);
    }).finally(() => {
      w.onmessage = null;
      w.onerror = null;
      release(slot);
    });
  });
}

/** Load the heavy artifacts before they are needed. Resolves when one
 * worker holds the parsed stanc3 compiler and `chains - 1` more hold the
 * instantiated wasm runtime, so a later compile() or sample() starts at
 * full speed instead of paying the fetch+parse on the user's click.
 * Safe to call more than once; the pool reuses warmed workers.
 * @param {object} [opts]
 * @param {number} [opts.chains=2] Workers to warm (first gets stanc3).
 * @param {function} [opts.onProgress]
 */
export function preload(opts) {
  const o = opts || {};
  const n = Math.max(1, Math.min(MAX_WORKERS, o.chains || 2));
  const jobs = [request({ cmd: "preload", stanc: true }, o)];
  for (let k = 1; k < n; ++k)
    jobs.push(request({ cmd: "preload", stanc: false }, o));
  return Promise.all(jobs);
}

/** Compile Stan source to transformed MIR (one worker loads stanc3).
 * @returns {Promise<{mir: string, ms: {stanc: number}}>} */
export function compile(opts) {
  return request({ cmd: "compile", code: opts.code }, opts);
}

/** Compile (unless `mir` is given) and draw from the posterior.
 *
 * @param {Object} opts
 * @param {string} [opts.code]     Stan source (compiled in the worker by
 *   stanc3, which loads lazily on first use).
 * @param {string} [opts.mir]      Precompiled transformed MIR (from
 *   `compile()` here, or `stanc --debug-transformed-mir` at build time).
 *   When given, the 2.8 MB compiler never loads: the runtime alone is
 *   ~1.3 MB gzipped.
 * @param {Object|string} [opts.data]  Data as an object or JSON text.
 * @param {number} [opts.seed=1]       Chain seed (sampler and GQ RNG).
 * @param {number} [opts.warmup=1000]
 * @param {number} [opts.samples=1000]
 * @param {number} [opts.delta=0.8]    Adaptation target acceptance.
 * @param {function(string)} [opts.onProgress]  Stage announcements.
 * @param {function(Object)} [opts.onLive]  Streaming draws while NUTS
 *   runs: {liveMeta: {names, warmup, samples}} once per call, then
 *   {live: {phase: "warmup"|"sampling", i, nCon?, rows?}} where rows is
 *   a transferred ArrayBuffer of constrained draws, nCon wide.
 * @returns {Promise<{names: string[], samples: number,
 *                    columns: Object<string, Float64Array>,
 *                    ms: {stanc: number, lower: number, sample: number,
 *                         total: number}}>}
 *   One column per CSV column CmdStan would write: constrained
 *   parameters, transformed parameters, and generated quantities.
 */
export function sample(opts) {
  return request({
    cmd: "run",
    code: opts.code,
    mir: opts.mir,
    live: !!opts.onLive,
    dataJson: typeof opts.data === "string"
        ? opts.data
        : JSON.stringify(opts.data || {}),
    seed: opts.seed == null ? 1 : opts.seed,
    warmup: opts.warmup == null ? 1000 : opts.warmup,
    samples: opts.samples == null ? 1000 : opts.samples,
    delta: opts.delta == null ? 0.8 : opts.delta,
  }, opts).then((done) => {
    const { names, samples, ms } = done;
    const flat = new Float64Array(done.columns);
    const columns = {};
    names.forEach((name, i) => {
      columns[name] = flat.subarray(i * samples, (i + 1) * samples);
    });
    return { names, samples, columns, ms };
  });
}
