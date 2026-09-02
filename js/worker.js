// The sampling worker: stanc3 (js_of_ocaml) compiles the model to MIR,
// stanli.wasm lowers it and runs NUTS or WALNUTS. Everything heavy lives
// here so the page never blocks. Protocol: {cmd: "run", code, dataJson,
// seed, warmup, samples, delta, sampler, maxError, pathfinderInit} in;
// {status} progress
// messages and one {done} or {error} out. `sampler` is "nuts",
// "walnuts" or "pathfinder".
"use strict";
importScripts("stanli.js");

const ready = createStanli();

// The stanli compiler (stanc3 plus the portable-MIR encoder) loads only when
// a request arrives with Stan source. Keep stock stancjs beside it for one
// compatibility cycle; it emits legacy MIR that the runtime still accepts.
// Apps that ship a fixed model precompile it to MIR at build time and never
// pay for either compiler.
function ensureStanc() {
  if (typeof globalThis.stanli_compile === "function" ||
      typeof globalThis.stanc === "function") return;
  try {
    importScripts("stanli-compiler.js");
  } catch (_) {
    // An older deployment can have the worker before the new compiler
    // artifact. Its stock compiler remains a complete fallback.
  }
  if (typeof globalThis.stanli_compile !== "function")
    importScripts("stancjs.bc.js");
}

function compileMir(code) {
  ensureStanc();
  if (typeof globalThis.stanli_compile === "function")
    return globalThis.stanli_compile("embedded_model", code);
  return globalThis.stanc(
      "browser_model", code, ["O1", "debug-optimized-mir"]);
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
    return M.UTF8ToString(outPtr);
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
// chain_id is 1, which is what stanli_sample_stream passes too, so
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
                            stanc: typeof globalThis.stanli_compile === "function" ||
                                   typeof globalThis.stanc === "function",
                            ms: { preload: performance.now() - t0 } } });
      return;
    }
    if (req.cmd === "compile") {
      // Compile once here, sample everywhere: chain workers take the MIR
      // and never load the compiler.
      say("compiling Stan -> MIR (stanc3)");
      ensureStanc();
      const sc = compileMir(req.code);
      if (sc.errors) throw new Error(Array.from(sc.errors).join("\n"));
      postMessage({ done: { mir: String(sc.result),
                            ms: { stanc: performance.now() - t0 } } });
      return;
    }
    const M = await ready;
    if (req.cmd === "diagnose") {
      postMessage({ done: diagnose(M, req) });
      return;
    }
    let mir = req.mir;
    if (!mir) {
      say("compiling Stan -> MIR (stanc3)");
      ensureStanc();
      const sc = compileMir(req.code);
      if (sc.errors) throw new Error(Array.from(sc.errors).join("\n"));
      mir = String(sc.result);
    }
    const tStanc = performance.now();

    say("lowering MIR -> op graph");
    const mirPtr = M.stringToNewUTF8(mir);
    const dataPtr = M.stringToNewUTF8(req.dataJson || "{}");
    const errLen = 8192;
    const errPtr = M._malloc(errLen);
    const model = M._stanli_model_new(mirPtr, dataPtr, errPtr, errLen);
    M._free(mirPtr);
    M._free(dataPtr);
    if (!model) throw new Error(M.UTF8ToString(errPtr));
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
    const drawsPtr = M._malloc(8 * samples * n);
    const initPtr = req.pathfinderInit ? M._malloc(8 * n) : 0;

    // Stream: every draw lands in the buffer before its callback, so the
    // page can plot the chain as it grows. Constrained rows batch in
    // chunks of 20 to keep message traffic sane.
    // Streaming is opt-in: without a live consumer the sampler runs the
    // plain path and pays nothing for callbacks or message traffic.
    let cbPtr = 0, liveRowPtr = 0;
    if (req.live && !pathfinder) {
    const nLive = Number(M._stanli_n_constrained(model));
    const liveNames = [];
    for (let i = 0; i < nLive; ++i)
      liveNames.push(
          M.UTF8ToString(M._stanli_constrained_name(model, BigInt(i))));
    postMessage({ liveMeta: { names: liveNames, warmup, samples } });
    liveRowPtr = M._malloc(8 * nLive);
    let pend = [];
    cbPtr = M.addFunction((i, wu) => {
      if (wu) {
        if (i % 25 === 0 || i + 1 === warmup)
          postMessage({ live: { phase: "warmup", i: i + 1 } });
        return;
      }
      M._stanli_constrain(model, drawsPtr + 8 * i * n, liveRowPtr);
      const row = new Float64Array(nLive);
      row.set(M.HEAPF64.subarray(liveRowPtr / 8, liveRowPtr / 8 + nLive));
      pend.push(row);
      if (pend.length >= 20 || i + 1 === samples) {
        const chunk = new Float64Array(pend.length * nLive);
        pend.forEach((r, k) => chunk.set(r, k * nLive));
        pend = [];
        postMessage({ live: { phase: "sampling", i: i + 1, nCon: nLive,
                              rows: chunk.buffer } }, [chunk.buffer]);
      }
    }, "viii");
    }
    // Same streaming contract either way; WALNUTS's tunable is the max
    // Hamiltonian error per macro step rather than NUTS's target
    // acceptance rate, and 0 asks the runtime for its default.
    let pf = null;
    let samplerStats = null;
    const statsPtr = pathfinder || walnuts ? 0 : M._malloc(8 * samples * 7);
    try {
      if (pathfinder) {
        pf = runPathfinder(M, model, req, samples, drawsPtr, errPtr, errLen);
      } else {
        if (req.pathfinderInit) {
          say("Pathfinder initialization: fitting approximation");
          runPathfinderInit(M, model, req, initPtr, errPtr, errLen);
          say("Pathfinder initialization complete; starting NUTS");
        }
        const rc = walnuts
            ? M._stanli_sample_walnuts_stream(model, req.seed >>> 0, warmup,
                                              samples, +req.maxError || 0,
                                              drawsPtr, cbPtr, 0, errPtr, errLen)
            : req.pathfinderInit
            ? M._stanli_sample_stream_stats_init(
                model, req.seed >>> 0, warmup, samples, +req.delta, initPtr,
                drawsPtr, statsPtr, cbPtr, 0, errPtr, errLen)
            : M._stanli_sample_stream_stats(
                model, req.seed >>> 0, warmup, samples, +req.delta, drawsPtr,
                statsPtr, cbPtr, 0, errPtr, errLen);
        if (rc !== 0) throw new Error(M.UTF8ToString(errPtr));
        if (statsPtr)
          samplerStats = M.HEAPF64.slice(statsPtr / 8, statsPtr / 8 + samples * 7);
      }
    } finally {
      if (cbPtr) M.removeFunction(cbPtr);
      if (liveRowPtr) M._free(liveRowPtr);
      if (statsPtr) M._free(statsPtr);
      if (initPtr) M._free(initPtr);
    }
    const tSample = performance.now();

    say("computing CSV columns");
    // write_array supplies every CmdStan CSV column (constrained params,
    // transformed parameters, generated quantities with seeded RNG);
    // models without a generate_quantities section fall back to the
    // constrained view.
    const nWa = Number(M._stanli_wa_n_columns(model));
    const useWa = nWa > 0;
    const nCon = useWa ? nWa : Number(M._stanli_n_constrained(model));
    const generatedStart = useWa
        ? Number(M._stanli_wa_n_generated_start(model)) : nCon;
    const names = [];
    for (let i = 0; i < nCon; ++i)
      names.push(M.UTF8ToString(
          useWa ? M._stanli_wa_column_name(model, BigInt(i))
                : M._stanli_constrained_name(model, BigInt(i))));
    if (useWa) M._stanli_wa_seed_chain(model, req.seed >>> 0, 1);
    const cols = new Float64Array(nCon * samples);
    const rowPtr = M._malloc(8 * nCon);
    for (let s = 0; s < samples; ++s) {
      if (useWa) {
        if (M._stanli_wa_row(model, drawsPtr + 8 * s * n, rowPtr) !== 0)
          throw new Error("write_array failed on draw " + s);
      } else {
        M._stanli_constrain(model, drawsPtr + 8 * s * n, rowPtr);
      }
      for (let i = 0; i < nCon; ++i)
        cols[i * samples + s] = M.HEAPF64[rowPtr / 8 + i];
    }
    M._free(rowPtr);
    M._free(drawsPtr);
    M._free(errPtr);
    M._stanli_model_free(model);

    postMessage({
      done: {
        names, samples, generatedStart, pathfinder: pf,
        sampler: pathfinder ? "pathfinder" : walnuts ? "walnuts" : "nuts",
        maxDepth: pathfinder ? null : 10,
        samplerStats: samplerStats ? samplerStats.buffer : null,
        // True unless the runtime was built with STANLI_LITE_LP, which
        // drops stan-math's propto instantiations and shifts lp__ by a
        // per-model constant. The shipped browser build does not, so this
        // is true; it stays in the payload so a caller that compares lp__
        // across engines can check rather than assume.
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
