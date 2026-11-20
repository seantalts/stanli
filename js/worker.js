// The sampling worker: stanc3 (js_of_ocaml) compiles the model to MIR,
// stanli.wasm lowers it and runs NUTS or WALNUTS. Everything heavy lives
// here so the page never blocks. Protocol: {cmd: "run", code, dataJson,
// seed, warmup, samples, delta, sampler, maxError, pathfinderInit, fastMath} in;
// {status} progress
// messages and one {done} or {error} out. `sampler` is "nuts",
// "walnuts" or "pathfinder".
"use strict";
importScripts("stanli.js");

const ready = createStanli();

// The stanli compiler (stanc3 plus the portable-MIR encoder) loads only when
// a request arrives with Stan source. Apps that ship a fixed model compile it
// to MIR ahead of time and never pay for the compiler.
function ensureStanc() {
  if (typeof globalThis.stanli_compile !== "function")
    importScripts("stanli-compiler.js");
}

// The MIR, the compiler's warnings, and the O1 fallback notice (null unless
// O1 was skipped for the structural budget). Throws on a compile error.
function compileMir(code, fastMath) {
  ensureStanc();
  const sc = fastMath ? globalThis.stanli_compile_fast("embedded_model", code)
                      : globalThis.stanli_compile("embedded_model", code);
  if (sc.errors) throw new Error(Array.from(sc.errors).join("\n"));
  const warnings = Array.from(sc.warnings || [], String);
  const o1Fallback = sc.o1_fallback == null ? null : String(sc.o1_fallback);
  return { mir: String(sc.result), warnings, o1Fallback };
}

// Reuse the native report (including rank-normalized R-hat and bulk/tail
// ESS) across all chains. Pack the column-oriented JS draws into the C ABI's
// chain/draw/column order. Keep this work off the UI thread.
function diagnose(M, req) {
  const allocations = [];
  const alloc = (bytes) => {
    const ptr = M._malloc(bytes);
    allocations.push(ptr);
    return ptr;
  };
  try {
    const { names, samples, chains, stats, maxDepth } = req;
    const nCols = names.length;
    const drawsPtr = alloc(8 * chains.length * samples * nCols);
    const statsPtr = alloc(8 * chains.length * samples * 7);
    const namesPtr = alloc(4 * nCols);
    const namePtrs = names.map((name) => {
      const ptr = M.stringToNewUTF8(name);
      allocations.push(ptr);
      return ptr;
    });
    // Allocations can grow WASM memory: acquire heap views only afterwards.
    new Uint32Array(M.HEAPF64.buffer, namesPtr, nCols).set(namePtrs);
    for (let c = 0; c < chains.length; ++c) {
      M.HEAPF64.set(stats[c], statsPtr / 8 + c * samples * 7);
      for (let j = 0; j < nCols; ++j)
        for (let s = 0; s < samples; ++s)
          M.HEAPF64[drawsPtr / 8 + (c * samples + s) * nCols + j] =
              chains[c][names[j]][s];
    }
    let size = 8192;
    let outPtr = alloc(size);
    const report = () => Number(M._stanli_diagnose_text(
        drawsPtr, BigInt(chains.length), BigInt(samples), BigInt(nCols),
        namesPtr, statsPtr, maxDepth, outPtr, size));
    const needed = report();
    if (!needed) throw new Error("Could not compute sampling diagnostics");
    if (needed > size) {
      size = needed;
      outPtr = alloc(size);
      if (!report()) throw new Error("Could not compute sampling diagnostics");
    }
    const countsPtr = alloc(8);
    if (M._stanli_diagnose_counts(
        drawsPtr, BigInt(chains.length), BigInt(samples), BigInt(nCols),
        statsPtr, maxDepth, countsPtr, countsPtr + 4) !== 0)
      throw new Error("Could not compute sampling diagnostics");
    const counts = new Int32Array(M.HEAPF64.buffer, countsPtr, 2);
    return { text: M.UTF8ToString(outPtr), failed: counts[0],
             unavailable: counts[1] };
  } finally {
    for (const ptr of allocations) M._free(ptr);
  }
}


// Pathfinder: one L-BFGS climb, a normal approximation fitted along it,
// and draws from that approximation -- milliseconds, where a sampler
// takes seconds. There is no warmup and no per-draw streaming to do,
// but the climb itself streams out iterate by iterate so the page can
// animate it.
//
// chain_id is 1, which is what the worker's NUTS chain uses too, so
// Pathfinder and a NUTS chain given the same seed start from the exact
// same point. lp and lp_approx per draw are available from the same
// call and left unasked-for here: k-hat, the one thing the page shows
// them for, already comes back computed.
function runPathfinder(M, model, req, numDraws, drawsPtr, errPtr, errLen) {
  const sumPtr = M._malloc(8 * 4);
  const path = [];
  const cbPtr = M.addFunction((iter, lp) => {
    path.push({ iter, lp });
    if (req.live) postMessage({ live: { phase: "path", iter, lp } });
  }, "vidi");
  const rc = M._stanli_run_pathfinder(model, req.seed >>> 0, 1, numDraws,
                                      drawsPtr, 0, 0, sumPtr, cbPtr, 0,
                                      errPtr, errLen);
  M.removeFunction(cbPtr);
  const sum = Array.from(M.HEAPF64.subarray(sumPtr / 8, sumPtr / 8 + 4));
  M._free(sumPtr);
  if (rc !== 0) throw new Error(M.UTF8ToString(errPtr));
  return { path, khat: sum[0], selectedIter: sum[1], selectedElbo: sum[2],
           elapsedMs: sum[3] };
}

// Fit a single Pathfinder approximation and draw one unconstrained start for
// this worker's NUTS chain. A page runs chains in separate workers, each with
// its already-established per-chain seed, so the starts and subsequent chains
// reproduce together without sharing mutable WASM state.
function runPathfinderInit(M, model, req, initPtr, errPtr, errLen) {
  const opts = req.pathfinderInit;
  const cbPtr = M.addFunction((iter, lp) => {
    if (req.live)
      postMessage({ live: { phase: "pathfinder-init", iter, lp } });
  }, "vidi");
  const rc = M._stanli_pathfinder_inits(
      model, req.seed >>> 0, 1, 1, opts.numIterations, opts.numElboDraws,
      opts.historySize, opts.initRadius, initPtr, cbPtr, 0, errPtr, errLen);
  M.removeFunction(cbPtr);
  if (rc !== 0)
    throw new Error("Pathfinder initialization failed: " +
                    M.UTF8ToString(errPtr));
}

// Byte offsets of a C struct's fields, read from the runtime's own layout
// table so that the worker never hard-codes them.
function structLayout(M, type) {
  const entries = M._stanli_field_layouts();
  const count = Number(M._stanli_n_field_layouts());
  const fields = {};
  let size = 0;
  for (let i = 0; i < count; ++i) {
    // {const char* type, const char* field, size_t offset, size, type_size}
    const e = new Uint32Array(M.HEAPF64.buffer, entries + 20 * i, 5);
    if (M.UTF8ToString(e[0]) !== type) continue;
    fields[M.UTF8ToString(e[1])] = e[2];
    size = e[4];
  }
  if (!size) throw new Error("the runtime does not describe " + type);
  return { fields, size };
}

// One NUTS chain through the multi-chain sampler, as Python and R run it:
// generated quantities draw from the chain's own stream between stored
// draws, and a rejected generated quantity leaves a row of NaN. When `live`
// is given, live.onRow(i, rowPtr) sees each stored output row as it is
// written and live.onWarmup(i) each warmup transition. Returns the output
// rows and the sampler columns, draw-major.
function sampleNuts(M, model, req, warmup, samples, initPtr, width, live,
                    errPtr, errLen) {
  const allocations = [];
  const alloc = (bytes) => {
    const ptr = M._malloc(bytes);
    allocations.push(ptr);
    return ptr;
  };
  let cbPtr = 0;
  try {
    const so = structLayout(M, "stanli_sample_opts");
    const mo = structLayout(M, "stanli_metric_opts");
    const optsPtr = alloc(so.size);
    const metricPtr = alloc(mo.size);
    M._stanli_sample_opts_init(optsPtr);
    M._stanli_metric_opts_init(metricPtr);
    const u32 = (field, v) =>
      new Uint32Array(M.HEAPF64.buffer, optsPtr + so.fields[field], 1)[0] = v;
    const i32 = (field, v) =>
      new Int32Array(M.HEAPF64.buffer, optsPtr + so.fields[field], 1)[0] = v;
    u32("seed", req.seed >>> 0);
    i32("chains", 1);
    i32("chain_id", 1);
    i32("warmup", warmup);
    i32("samples", samples);
    new Float64Array(M.HEAPF64.buffer, optsPtr + so.fields.delta, 1)[0] =
        +req.delta;
    if (initPtr) u32("inits", initPtr);
    if (warmup === 0)
      new Int32Array(M.HEAPF64.buffer,
                     metricPtr + mo.fields.adapt_engaged, 1)[0] = 0;

    const valuesPtr = alloc(8 * samples * width);
    const statsPtr = alloc(8 * samples * 7);
    const reportsPtr = alloc(32);
    // refresh 1 calls back after every transition; the row of a sampling
    // transition is already in `values` then.
    if (live)
      cbPtr = M.addFunction((chain, iteration, total, isWarmup) => {
        const i = Number(iteration) - 1;
        if (isWarmup) live.onWarmup(i);
        else live.onRow(i - warmup, valuesPtr + 8 * (i - warmup) * width);
      }, "vijjijji");
    const rc = M._stanli_sample_multi_metric(
        model, optsPtr, metricPtr, live ? 1 : 0, 0, statsPtr, valuesPtr,
        cbPtr, 0, 0, 0, 0, reportsPtr, errPtr, errLen);
    if (rc !== 0) throw new Error(M.UTF8ToString(errPtr));
    return {
      values: M.HEAPF64.slice(valuesPtr / 8, valuesPtr / 8 + samples * width),
      stats: M.HEAPF64.slice(statsPtr / 8, statsPtr / 8 + samples * 7),
    };
  } finally {
    if (cbPtr) M.removeFunction(cbPtr);
    for (const ptr of allocations) M._free(ptr);
  }
}

onmessage = async (e) => {
  const req = e.data;
  const t0 = performance.now();
  const say = (status) => postMessage({ status });
  try {
    if (req.cmd === "preload") {
      // Warm the worker before anyone needs it: instantiate the wasm
      // runtime and, when asked, parse the 3.0 MB compiler. Sent by the
      // page at idle so the first Run pays neither load.
      if (req.stanc) ensureStanc();
      await ready;
      postMessage({ done: { warmed: true,
                            stanc: typeof globalThis.stanli_compile === "function",
                            ms: { preload: performance.now() - t0 } } });
      return;
    }
    if (req.cmd === "compile") {
      // Compile once here, sample everywhere: chain workers take the MIR
      // and never load the compiler.
      say("compiling Stan -> MIR (stanc3)");
      const sc = compileMir(req.code, !!req.fastMath);
      postMessage({ done: { mir: sc.mir, fastMath: !!req.fastMath,
                            warnings: sc.warnings, o1Fallback: sc.o1Fallback,
                            ms: { stanc: performance.now() - t0 } } });
      return;
    }
    const M = await ready;
    if (req.cmd === "diagnose") {
      postMessage({ done: diagnose(M, req) });
      return;
    }
    let mir = req.mir;
    let o1Fallback = null;
    if (!mir) {
      say("compiling Stan -> MIR (stanc3)");
      ({ mir, o1Fallback } = compileMir(req.code, !!req.fastMath));
    }
    const tStanc = performance.now();

    say("lowering MIR -> op graph");
    const mirPtr = M.stringToNewUTF8(mir);
    const dataPtr = M.stringToNewUTF8(req.dataJson || "{}");
    const errLen = 8192;
    const errPtr = M._malloc(errLen);
    // The construction seed CmdStan would use: transformed data draws
    // from it once, at build time, so every chain of this run shares them.
    const optsPtr = M._malloc(24);
    M._stanli_model_opts_init(optsPtr);
    const fields = new Uint32Array(M.HEAPF64.buffer, optsPtr, 6);
    fields[0] = (req.seed ?? 1) >>> 0;
    fields[2] = req.fastMath ? 1 : 0;
    const model = M._stanli_model_new_with_opts(mirPtr, dataPtr, optsPtr,
                                                errPtr, errLen);
    M._free(optsPtr);
    M._free(mirPtr);
    M._free(dataPtr);
    if (!model) throw new Error(M.UTF8ToString(errPtr));
    const note = M.UTF8ToString(M._stanli_warnings(model));
    if (note) console.warn(note);
    const tCompile = performance.now();

    const n = Number(M._stanli_n_unconstrained(model));
    const samples = req.samples | 0;
    const warmup = req.warmup | 0;
    const pathfinder = req.sampler === "pathfinder";
    const walnuts = req.sampler === "walnuts";
    say(pathfinder
        ? "Pathfinder: L-BFGS path + " + samples + " draws"
        : (walnuts ? "WALNUTS: " : "NUTS: ") + warmup + " warmup + " +
          samples + " draws");
    // Every CmdStan CSV column (constrained params, transformed parameters,
    // generated quantities). Models without a generated quantities section
    // have only the constrained view, which stanli_constrain writes.
    const useWa = Number(M._stanli_wa_n_columns(model)) > 0;
    const nCon = Number(M._stanli_n_output_columns(model));
    const generatedStart = useWa
        ? Number(M._stanli_wa_n_generated_start(model)) : nCon;
    const names = [];
    for (let i = 0; i < nCon; ++i)
      names.push(M.UTF8ToString(
          M._stanli_output_column_name(model, BigInt(i))));

    // Stream: the page plots the chain as it grows from the constrained
    // parameters, the first columns of each row, in chunks of 20 rows to
    // keep message traffic sane. Streaming is opt-in: without a live
    // consumer the sampler runs the plain path and pays nothing for
    // callbacks or message traffic.
    let live = null;
    if (req.live && !pathfinder) {
      const nLive = Number(M._stanli_n_constrained(model));
      postMessage({ liveMeta: { names: names.slice(0, nLive), warmup,
                                samples } });
      let pend = [];
      live = {
        onWarmup(i) {
          if (i % 25 === 0 || i + 1 === warmup)
            postMessage({ live: { phase: "warmup", i: i + 1 } });
        },
        onRow(i, rowPtr) {
          pend.push(M.HEAPF64.slice(rowPtr / 8, rowPtr / 8 + nLive));
          if (pend.length >= 20 || i + 1 === samples) {
            const chunk = new Float64Array(pend.length * nLive);
            pend.forEach((r, k) => chunk.set(r, k * nLive));
            pend = [];
            postMessage({ live: { phase: "sampling", i: i + 1, nCon: nLive,
                                  rows: chunk.buffer } }, [chunk.buffer]);
          }
        },
      };
    }

    let pf = null;
    let samplerStats = null;
    let rows = null;  // output rows, draw-major
    if (pathfinder || walnuts) {
      // These two return unconstrained draws; their rows are written after.
      // WALNUTS's tunable is the max Hamiltonian error per macro step, and 0
      // asks the runtime for its default.
      const drawsPtr = M._malloc(8 * samples * n);
      let cbPtr = 0;
      try {
        if (pathfinder) {
          pf = runPathfinder(M, model, req, samples, drawsPtr, errPtr, errLen);
        } else {
          const rowPtr = live ? M._malloc(8 * nCon) : 0;
          if (live)
            cbPtr = M.addFunction((i, wu) => {
              if (wu) return live.onWarmup(i);
              M._stanli_constrain(model, drawsPtr + 8 * i * n, rowPtr);
              live.onRow(i, rowPtr);
            }, "viii");
          const rc = M._stanli_sample_walnuts_stream(
              model, req.seed >>> 0, warmup, samples, +req.maxError || 0,
              drawsPtr, cbPtr, 0, errPtr, errLen);
          if (rowPtr) M._free(rowPtr);
          if (rc !== 0) throw new Error(M.UTF8ToString(errPtr));
        }
        rows = new Float64Array(nCon * samples);
        const rowPtr = M._malloc(8 * nCon);
        if (useWa) M._stanli_wa_seed_chain(model, req.seed >>> 0, 1);
        for (let s = 0; s < samples; ++s) {
          if (useWa) {
            if (M._stanli_wa_row(model, drawsPtr + 8 * s * n, rowPtr) !== 0)
              throw new Error("write_array failed on draw " + s);
          } else {
            M._stanli_constrain(model, drawsPtr + 8 * s * n, rowPtr);
          }
          rows.set(M.HEAPF64.subarray(rowPtr / 8, rowPtr / 8 + nCon),
                   s * nCon);
        }
        M._free(rowPtr);
      } finally {
        if (cbPtr) M.removeFunction(cbPtr);
        M._free(drawsPtr);
      }
    } else {
      const initPtr = req.pathfinderInit ? M._malloc(8 * n) : 0;
      try {
        if (req.pathfinderInit) {
          say("Pathfinder initialization: fitting approximation");
          runPathfinderInit(M, model, req, initPtr, errPtr, errLen);
          say("Pathfinder initialization complete; starting NUTS");
        }
        const run = sampleNuts(M, model, req, warmup, samples, initPtr, nCon,
                               live, errPtr, errLen);
        rows = run.values;
        samplerStats = run.stats;
      } finally {
        if (initPtr) M._free(initPtr);
      }
    }
    const tSample = performance.now();

    say("computing CSV columns");
    const cols = new Float64Array(nCon * samples);
    for (let s = 0; s < samples; ++s)
      for (let i = 0; i < nCon; ++i)
        cols[i * samples + s] = rows[s * nCon + i];
    M._free(errPtr);
    M._stanli_model_free(model);

    postMessage({
      done: {
        names, samples, generatedStart, pathfinder: pf,
        fastMath: !!req.fastMath, o1Fallback,
        sampler: pathfinder ? "pathfinder" : walnuts ? "walnuts" : "nuts",
        maxDepth: pathfinder ? null : 10,
        samplerStats: samplerStats ? samplerStats.buffer : null,
        exactLp: M._stanli_exact_lp() !== 0,
        columns: cols.buffer,
        ms: {
          stanc: tStanc - t0,
          lower: tCompile - tStanc,
          sample: tSample - tCompile,
          total: performance.now() - t0,
        },
      },
    }, samplerStats ? [cols.buffer, samplerStats.buffer] : [cols.buffer]);
  } catch (err) {
    postMessage({ error: String(err && err.message || err) });
  }
};
