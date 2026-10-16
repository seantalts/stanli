# stanli

Full [Stan](https://mc-stan.org) in the browser. stanc3, the real Stan
compiler built to JavaScript, compiles the model. A WebAssembly build of the
stanli runtime samples it with Stan's NUTS over precompiled stan-math kernels.
There is no server and no C++ toolchain; everything runs in the tab. Live
demo: <https://seantalts.github.io/stanli/>.

## Install

```console
npm install @seantalts/stanli
```

The package runs in browsers that support module Web Workers.

## Quick start

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

Columns cover the full CmdStan CSV: parameters, transformed parameters and
generated quantities, with RNG draws seeded from `seed`. With no generated
quantities, `generatedStart === fit.names.length`.

`fastMath: true` opts in to [fast mode](https://github.com/seantalts/stanli/blob/main/docs/fast-mode.md): faster
gradients, with results that can differ from the default in the last bits.
`fit.fastMath` reports the mode a fit ran in. With a precompiled `mir`, give
`compile()` and `sample()` the same value:

```js
const { mir } = await compile({ code, fastMath: true });
const fit = await sample({ mir, data, fastMath: true });
```

## Sampling and diagnostics

Each `sample()` call runs one chain. Workers pool up to the machine's core
count (at most eight), so independent calls run at the same time. Compile once
and pass `mir` to run several chains:

```js
import { compile, diagnose, sample } from "@seantalts/stanli";
const { mir } = await compile({ code });
const fits = await Promise.all([1, 2, 3, 4].map((seed) =>
  sample({ mir, data, seed })));
console.log(await diagnose(fits));
```

`diagnose()` returns the same text report as the R and Python packages:
divergences, maximum-treedepth saturation, E-BFMI, rank-normalized R-hat, and
bulk/tail ESS. It takes one NUTS fit or an array of chains from the same model
and settings.

`fit.samplerStats` holds seven doubles per post-warmup draw, in order:
`lp__`, `accept_stat__`, `stepsize__`, `treedepth__`, `n_leapfrog__`,
`divergent__`, `energy__`. `fit.maxDepth` is the treedepth limit (10).

## Other algorithms

`sampler: "walnuts"` runs WALNUTS, NUTS with a step length that adapts within
each trajectory ([arXiv:2506.18746](https://arxiv.org/abs/2506.18746)).
`maxError` sets its largest allowed log-density drift per macro step (default
0.5).

`sampler: "pathfinder"` fits a normal approximation along an L-BFGS path. It
ignores `warmup` and `delta`, treats `samples` as its draw count, and returns
a `fit.pathfinder` block with the path, the selected iteration, its ELBO, and
an importance-weight k-hat.

WALNUTS and Pathfinder return `null` for `samplerStats`, and `diagnose()`
refuses them.

NUTS can also start from single-path Pathfinder. The seed controls both
stages, and `{}` uses the defaults:

```js
const fit = await sample({
  code, data, seed: 303,
  pathfinderInit: { numIterations: 500, numElboDraws: 25 },
});
```

`historySize` and Pathfinder's own `initRadius` are the other options. This
mode does no PSIS resampling.

## API reference

| Function | Does |
| --- | --- |
| `sample({code \| mir, data, seed, warmup, samples, delta, sampler, maxError, pathfinderInit, fastMath, onProgress, onLive})` | Compile (unless `mir` is given) and draw. Defaults: `seed` 1, `warmup` 1000, `samples` 1000, `delta` 0.8, `sampler` `"nuts"`. |
| `compile({code, fastMath})` | Compile Stan source to portable MIR once, for reuse across `sample()` calls. |
| `diagnose(fit \| fits)` | Text diagnostic report for NUTS fits. |
| `preload({chains})` | Start loading the compiler and WASM runtime in the background. |

`data` is an object or JSON text; `NaN` and infinities are passed through as
Stan expects. `onLive` streams draws while NUTS runs (or path iterates for
Pathfinder), for live plots.

## Platforms

The compiler loads only when a call passes Stan source. It is about 3 MB raw
and 425 KB gzipped. The package also carries the stock `stancjs.bc.js` of
about the same size for one release cycle, loaded only if the preferred
compiler is unavailable. `preload()` at page idle lets the first `sample()`
skip the fetch and parse. An app with a fixed model can compile it at build
time (`stanc --O1 --debug-optimized-mir model.stan`) and pass `mir`; then no
compiler loads and the WASM runtime alone is about 1.5 MB gzipped.

## Limits

- No optimization, ADVI or Laplace; the
  [R package](https://github.com/seantalts/stanli/blob/main/r/README.md#other-algorithms) has them.
- No metric or step-size options; NUTS uses the adapted diagonal metric.
- No within-chain threading; run chains as separate `sample()` calls.
- `wasm32` caps memory at 4 GB. One 79,000-parameter corpus model exceeds it;
  typical models fit.

How stanli works is in [docs/how-it-works.md](https://github.com/seantalts/stanli/blob/main/docs/how-it-works.md), how it
is tested (including inside this WASM build) in [TESTING.md](https://github.com/seantalts/stanli/blob/main/TESTING.md),
and how fast it is in [docs/benchmarks.md](https://github.com/seantalts/stanli/blob/main/docs/benchmarks.md).
