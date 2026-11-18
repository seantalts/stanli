# stanli

Full [Stan](https://mc-stan.org) in the browser. stanc3, the official Stan
compiler, is built to JavaScript and compiles the model. A WebAssembly build
of the stanli runtime then samples it with Stan's NUTS sampler and
precompiled stan-math kernels. Everything runs in the browser tab, with no
server and no C++ toolchain. Live demo: <https://seantalts.github.io/stanli/>.

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

`fastMath: true` turns on [fast mode](https://github.com/seantalts/stanli/blob/main/docs/fast-mode.md).
Gradients are faster, and results can differ from the default in the last
bits. `fit.fastMath` reports the mode that a fit ran in. If you precompile a
`mir`, give `compile()` and `sample()` the same value:

```js
const { mir } = await compile({ code, fastMath: true });
const fit = await sample({ mir, data, fastMath: true });
```

## Sampling and diagnostics

Each `sample()` call runs one chain. stanli keeps a pool of workers, one per
core up to a maximum of eight, so independent calls run at the same time. To
run several chains, compile once and pass `mir` to each call:

```js
import { compile, diagnose, sample } from "@seantalts/stanli";
const { mir } = await compile({ code });
const fits = await Promise.all([1, 2, 3, 4].map((seed) =>
  sample({ mir, data, seed })));
console.log(await diagnose(fits));
```

`diagnose()` returns the same text report as the R and Python packages. The
report covers divergences, maximum-treedepth saturation, E-BFMI,
rank-normalized R-hat, and bulk and tail ESS. It takes one NUTS fit, or an
array of chains from the same model and settings.

`fit.samplerStats` holds seven doubles per post-warmup draw, in order:
`lp__`, `accept_stat__`, `stepsize__`, `treedepth__`, `n_leapfrog__`,
`divergent__`, `energy__`. `fit.maxDepth` is the treedepth limit (10).

## Other algorithms

`sampler: "walnuts"` runs WALNUTS, NUTS with a step length that adapts within
each trajectory ([arXiv:2506.18746](https://arxiv.org/abs/2506.18746)).
`maxError` sets the largest log-density drift that it allows in each macro
step (default 0.5).

`sampler: "pathfinder"` fits a normal approximation along an L-BFGS path. It
ignores `warmup` and `delta` and treats `samples` as its number of draws. It
returns a `fit.pathfinder` block with the path, the selected iteration, its
ELBO, and the k-hat of the importance weights.

WALNUTS and Pathfinder return `null` for `samplerStats`, and `diagnose()`
does not accept them.

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
| `compile({code, fastMath})` | Compile Stan source to portable MIR once, for reuse across `sample()` calls. Returns `{mir, warnings, o1Fallback}`; `o1Fallback` is null unless the model was too large for O1 and was compiled from unoptimized MIR. |
| `diagnose(fit \| fits)` | Text diagnostic report for NUTS fits. |
| `diagnoseReport(fit \| fits)` | `{ text, failed, unavailable }`: the report and its verdict as counts. |
| `preload({chains})` | Start loading the compiler and WASM runtime in the background. |

`data` is an object or JSON text. stanli passes `NaN` and infinities through
as Stan expects. For live plots, `onLive` receives draws while NUTS runs, or
each path iterate while Pathfinder runs.

## Platforms

The compiler loads only when a call passes Stan source. It is about 3 MB raw
and 425 KB gzipped. If you call `preload()` while the page is idle, the first
`sample()` does not wait to fetch and parse it.

An app with a fixed model can save the `mir` that `compile()` returns, or that
`stanli.stan_to_mir()` returns in Python, and pass it to `sample()`. Then the
compiler never loads, and the app downloads only the WASM runtime, which is
about 1.5 MB gzipped.

## Limits

- No optimization, ADVI or Laplace; the
  [R package](https://github.com/seantalts/stanli/blob/main/r/README.md#other-algorithms) has them.
- No metric or step-size options; NUTS uses the adapted diagonal metric.
- No within-chain threading; run chains as separate `sample()` calls.
- `wasm32` limits memory to 4 GB. One corpus model with 79,000 parameters
  needs more than that. Typical models fit.

## More

- How stanli works: [docs/how-it-works.md](https://github.com/seantalts/stanli/blob/main/docs/how-it-works.md).
- How we test it, including inside this WASM build: [TESTING.md](https://github.com/seantalts/stanli/blob/main/TESTING.md).
- How fast it is: [docs/benchmarks.md](https://github.com/seantalts/stanli/blob/main/docs/benchmarks.md).
