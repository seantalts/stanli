// stanli: full Stan in the browser. The custom stanc3 build (compiled to JS)
// turns Stan source into portable MIR, and a WASM build of the stanli runtime
// lowers it to an op graph and runs NUTS. Everything happens client side, off
// the main thread, in workers this module owns.
//
//   import { compile, sample } from "@seantalts/stanli";
//   const { mir } = await compile({ code });
//   const fits = await Promise.all([1, 2, 3, 4].map((c) =>
//       sample({ mir, data, seed: c })));
//
// Workers pool up to the hardware's concurrency, so independent calls --
// one chain each -- run simultaneously. Compiling once and passing `mir`
// keeps the 3.0 MB preferred compiler in a single worker (or out of the page
// entirely, if the model was precompiled at build time with
// stanc --O1 --debug-optimized-mir). The package carries stock stancjs for one
// rollback cycle, but the worker loads it only if the portable compiler fails.

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

/** Compile Stan source to portable MIR (one worker loads stanc3).
 * @returns {Promise<{mir: string, ms: {stanc: number}}>} */
export function compile(opts) {
  return request({ cmd: "compile", code: opts.code }, opts);
}

function pathfinderInitOptions(value) {
  if (value == null) return null;
  if (typeof value !== "object" || Array.isArray(value))
    throw new TypeError("pathfinderInit must be an options object");
  const defaults = { numIterations: 1000, numElboDraws: 25,
                     historySize: 5, initRadius: 2 };
  const unknown = Object.keys(value).filter((key) => !(key in defaults));
  if (unknown.length)
    throw new RangeError("unknown pathfinderInit option" +
                         (unknown.length > 1 ? "s" : "") + ": " +
                         unknown.join(", "));
  const out = { ...defaults, ...value };
  for (const name of ["numIterations", "numElboDraws", "historySize"])
    if (!Number.isInteger(out[name]) || out[name] <= 0 ||
        out[name] > 2147483647)
      throw new RangeError(`pathfinderInit ${name} must be a positive integer`);
  if (typeof out.initRadius !== "number" || !Number.isFinite(out.initRadius) ||
      out.initRadius < 0)
    throw new RangeError(
        "pathfinderInit initRadius must be finite and nonnegative");
  return out;
}

/** Compile (unless `mir` is given) and draw from the posterior.
 *
 * @param {Object} opts
 * @param {string} [opts.code]     Stan source (compiled in the worker by
 *   stanc3, which loads lazily on first use).
 * @param {string} [opts.mir]      Precompiled MIR (from `compile()` here, or
 *   `stanc --O1 --debug-optimized-mir` at build time). When given, neither
 *   browser compiler loads: the runtime alone is ~1.5 MB gzipped.
 * @param {Object|string} [opts.data]  Data as an object or JSON text.
 * @param {number} [opts.seed=1]       Chain seed (sampler and GQ RNG).
 * @param {number} [opts.warmup=1000]
 * @param {number} [opts.samples=1000]
 * @param {number} [opts.delta=0.8]    Adaptation target acceptance (NUTS).
 * @param {Object} [opts.pathfinderInit]  Generate the NUTS start with
 *   single-path Pathfinder. `{}` uses defaults; supported keys are
 *   `numIterations`, `numElboDraws`, `historySize`, and `initRadius`.
 *   The sampling seed controls both stages. NUTS only; no PSIS resampling.
 * @param {string} [opts.sampler="nuts"]  "nuts", "walnuts" (within-orbit
 *   adaptive step-length NUTS, arXiv:2506.18746), or "pathfinder"
 *   (a normal approximation fitted along an L-BFGS path). Pathfinder
 *   ignores warmup and delta, treats `samples` as its draw count, and
 *   returns an extra `pathfinder` block.
 * @param {number} [opts.maxError]     WALNUTS only: largest drift in the
 *   joint log density allowed across one macro step before the step is
 *   halved within the trajectory. Omit for the runtime default (0.5).
 * @param {function(string)} [opts.onProgress]  Stage announcements.
 * @param {function(Object)} [opts.onLive]  Streaming draws while NUTS
 *   runs: {liveMeta: {names, warmup, samples}} once per call, then
 *   {live: {phase: "warmup"|"sampling", i, nCon?, rows?}} where rows is
 *   a transferred ArrayBuffer of constrained draws, nCon wide.
 *   Pathfinder streams {live: {phase: "path", iter, lp}} instead, one
 *   message per L-BFGS iterate.
 * @returns {Promise<{names: string[], samples: number, generatedStart: number,
 *                    columns: Object<string, Float64Array>,
 *                    exactLp: boolean,
 *                    sampler: string, maxDepth: number|null,
 *                    samplerStats: Float64Array|null,
 *                    pathfinder?: {path: {iter, lp}[], khat: number,
 *                                  selectedIter: number,
 *                                  selectedElbo: number,
 *                                  elapsedMs: number},
 *                    ms: {stanc: number, lower: number, sample: number,
 *                         total: number}}>}
 *   One column per CSV column CmdStan would write: constrained
 *   parameters, transformed parameters, and generated quantities.
 *   NUTS also returns post-warmup samplerStats in draw-major order:
 *   lp__, accept_stat__, stepsize__, treedepth__, n_leapfrog__,
 *   divergent__, energy__. Other methods return null for samplerStats.
 */
export function sample(opts) {
  const sampler = opts.sampler === "walnuts" || opts.sampler === "pathfinder"
      ? opts.sampler : "nuts";
  const pathfinderInit = pathfinderInitOptions(opts.pathfinderInit);
  if (pathfinderInit && sampler !== "nuts")
    throw new RangeError("pathfinderInit is available only with NUTS");
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
    sampler,
    maxError: opts.maxError == null ? 0 : opts.maxError,
    pathfinderInit,
  }, opts).then((done) => {
    const { names, samples, generatedStart, ms, exactLp, pathfinder,
            sampler, maxDepth } = done;
    const flat = new Float64Array(done.columns);
    const columns = {};
    names.forEach((name, i) => {
      columns[name] = flat.subarray(i * samples, (i + 1) * samples);
    });
    const samplerStats = done.samplerStats
        ? new Float64Array(done.samplerStats) : null;
    return { names, samples, generatedStart, columns, ms, exactLp, pathfinder,
             sampler, maxDepth, samplerStats };
  });
}

/** Diagnose one NUTS fit, or an array of chains from the same model and
 * configuration. Returns the native R/Python diagnostic report as text,
 * using only post-warmup draws. Inputs are copied, never transferred away.
 * WALNUTS and Pathfinder do not expose the required sampler statistics.
 * @returns {Promise<string>} */
export async function diagnose(fits) {
  const chains = Array.isArray(fits) ? fits : [fits];
  const first = chains[0];
  if (!first || !Number.isInteger(first.samples) || first.samples < 1 ||
      !Array.isArray(first.names) || !first.names.length)
    throw new Error("diagnose requires nonempty NUTS draws");
  for (const fit of chains) {
    if (!fit || fit.sampler !== "nuts" || fit.samples !== first.samples ||
        fit.maxDepth !== first.maxDepth || !Number.isInteger(fit.maxDepth) ||
        fit.maxDepth < 1 || !Array.isArray(fit.names) ||
        fit.names.length !== first.names.length ||
        fit.names.some((name, j) => name !== first.names[j]) ||
        !(fit.samplerStats instanceof Float64Array) ||
        fit.samplerStats.length !== first.samples * 7 ||
        !fit.columns || first.names.some((name) =>
          !(fit.columns[name] instanceof Float64Array) ||
          fit.columns[name].length !== first.samples))
      throw new Error("diagnose requires matching NUTS chains with sampler statistics");
  }
  return request({ cmd: "diagnose", names: first.names, samples: first.samples,
                   maxDepth: first.maxDepth,
                   chains: chains.map((fit) => fit.columns),
                   stats: chains.map((fit) => fit.samplerStats) }, {});
}
