# stanli

Full [Stan](https://mc-stan.org) in the browser. stanc3 (the real Stan
compiler, compiled to JavaScript) parses, typechecks, and optimizes the
model; the shared stanli OCaml pipeline encodes its typed MIR into portable
MIR. A WebAssembly build of the stanli runtime lowers that to an op graph over
precompiled stan-math kernels and samples with NUTS. No server, no C++
toolchain, everything in the tab. Live demo:
<https://seantalts.github.io/stanli/>.

```js
import { sample } from "@seantalts/stanli";

const fit = await sample({
  code: `
    data { int N; array[N] real y; }
    parameters { real mu; real<lower=0> sigma; }
    model { y ~ normal(mu, sigma); }`,
  data: { N: 3, y: [1.1, 0.4, 2.2] },
  seed: 1,
  onProgress: (s) => console.log(s),
});

fit.columns["mu"];     // Float64Array, one entry per draw
fit.names;             // every CSV column CmdStan would write
fit.generatedStart;    // index where generated-quantity columns begin
fit.ms;                // {stanc, lower, sample, total} in milliseconds
```

NUTS can take its starting point from single-path Pathfinder. The same seed
controls initialization and sampling, and an empty options object uses the
defaults:

```js
const fit = await sample({
  code,
  data,
  seed: 303,
  pathfinderInit: { numIterations: 500, numElboDraws: 25 },
});
```

`historySize` and Pathfinder's own `initRadius` are also supported. This mode
does not perform PSIS resampling.

For NUTS, `await diagnose(fit)` returns the same text report as the R and
Python bindings: divergences, maximum-treedepth saturation, E-BFMI,
rank-normalized R-hat, and bulk/tail ESS. Pass an array of fits from the
same model and configuration to diagnose all chains together:

```js
import { compile, diagnose, sample } from "@seantalts/stanli";
const { mir } = await compile({ code });
const fits = await Promise.all([1, 2, 3, 4].map((seed) =>
  sample({ mir, data, seed })));
console.log(await diagnose(fits));
```

`fit.samplerStats` contains seven doubles per post-warmup draw, in order:
`lp__`, `accept_stat__`, `stepsize__`, `treedepth__`, `n_leapfrog__`,
`divergent__`, `energy__`. `fit.maxDepth` records the sampling limit (10).
WALNUTS and Pathfinder return `null` for `samplerStats`; `diagnose()` rejects
these methods rather than treating missing statistics as successful checks.
The demo displays the report after NUTS runs, including comparisons, and
explicitly marks WALNUTS sampler diagnostics as unavailable. Pathfinder keeps
its existing importance-weight k-hat diagnostic.

Columns cover the full CmdStan CSV: constrained parameters, transformed
parameters, and generated quantities (RNG draws stream from `seed`).
When there are no generated quantities, `generatedStart === fit.names.length`.
The heavy work runs in a worker the package owns, so the page never
blocks; calls queue and run one at a time.

The preferred compiler, `stanli-compiler.js`, is 2,988,001 bytes raw and
424,607 bytes gzipped in the current measured build. For one rollback cycle
the package also contains stock `stancjs.bc.js` (2,966,778 bytes raw, 417,857
bytes gzipped). The worker loads the stock compiler only if the portable
compiler is unavailable; its O1 legacy MIR remains accepted by the runtime.
Carrying both temporarily doubles the compiler portion of the installed and
downloaded package.

The compiler loads lazily, only when a call passes Stan source. `preload()`
starts the compiler and WASM loads in the background; call it at page idle so
the first `sample()` skips the fetch and parse. An app that ships a fixed model
can precompile it at build time (`stanc --O1 --debug-optimized-mir model.stan`)
and pass `mir` instead of `code`; neither browser compiler loads, and the WASM
runtime alone is ~1.5 MB gzipped.

On Eight Schools, compact portable MIR is 6,932 bytes (1,793 gzipped), compared
with 33,320 bytes (2,000 gzipped) for legacy MIR. Across 51 fresh processes on
the same Apple arm64 release build, median decoder parsing was 0.074 ms versus
0.293 ms, and complete preparation was 0.278 ms versus 0.682 ms. These are
one-time preparation measurements; source compilation still dominates that
path.

118 of 119 posteriordb corpus models verify against CmdStan's log
density, gradients, and write_array values from inside this WASM build;
see the [repository](https://github.com/seantalts/stanli) for the
verification policy and numbers.

Not yet here: variational inference, optimization, multi-chain
threading. `wasm32` caps memory at 4 GB, which one 79,000-parameter
corpus model exceeds; everything typical fits.
