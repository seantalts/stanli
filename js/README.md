# stanli

Full [Stan](https://mc-stan.org) in the browser. stanc3 (the real Stan
compiler, compiled to JavaScript) turns Stan source into its intermediate
representation; a WebAssembly build of the stanli runtime lowers it to an
op graph over precompiled stan-math kernels and samples with NUTS. No
server, no C++ toolchain, everything in the tab. Live demo:
<https://seantalts.github.io/stanli/>.

```js
import { sample } from "stanli";

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
fit.ms;                // {stanc, lower, sample, total} in milliseconds
```

Columns cover the full CmdStan CSV: constrained parameters, transformed
parameters, and generated quantities (RNG draws stream from `seed`).
The heavy work runs in a worker the package owns, so the page never
blocks; calls queue and run one at a time.

The payload is ~9 MB installed (~2 MB over the wire with gzip): the WASM
runtime plus the stanc3 compiler. The compiler loads lazily, only when a
call passes Stan source. `preload()` starts both loads in the background
-- call it at page idle and the user's first `sample()` begins at full
speed instead of paying the fetch and parse on their click; an app that ships a fixed model can precompile
it at build time (`stanc --debug-transformed-mir model.stan`) and pass
`mir` instead of `code`, and the runtime alone is ~1.3 MB gzipped. 118 of 119 posteriordb corpus models
verify against CmdStan's log density, gradients, and write_array values
from inside this WASM build; see the
[repository](https://github.com/seantalts/stanli) for the verification
policy and numbers.

Not yet here: variational inference, optimization, multi-chain
threading. `wasm32` caps memory at 4 GB, which one 79,000-parameter
corpus model exceeds; everything typical fits.
