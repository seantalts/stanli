# stanli for R

Compile and sample Stan models without a C++ toolchain.

## Install

Not on CRAN yet. Until it is:

```r
# r-universe: binaries for Linux, macOS and Windows, rebuilt from main
install.packages("stanli", repos = "https://seantalts.r-universe.dev")

# or from a checkout
# R CMD INSTALL r
```

Then, once per machine:

```r
stanli_install()
```

That downloads the ~16 MB runtime for your platform into
`tools::R_user_dir("stanli", "cache")`. Nothing is fetched without it.
It takes the release the package was built against rather than whichever
is newest, so the binding and the library always agree; pass
`version = "latest"` to override, and set `STANLI_RUNTIME` to use a
library you built yourself.

## Use

```r
library(stanli)

m <- stanli_model(file = "eight_schools.stan", data = list(J = 8L, y = y, sigma = s))
fit <- sample_model(m, chains = 4, seed = 1)

summary(fit)          # mean, MCSE, sd, quantiles, bulk/tail ESS, R-hat
stanli_diagnose(fit)  # divergences, treedepth, E-BFMI, R-hat, ESS
as_draws_array(fit)   # a posterior::draws_array
```

Four chains of eight schools — 8000 draws, full summary and diagnostics —
take about 70 ms, because the model is lowered to a graph over
precompiled kernels rather than translated to C++ and compiled.

`summary()` reports rank-normalized split R-hat with bulk and tail
effective sample size and MCSE. Those are stan's own estimators, so the
numbers agree with `stansummary` rather than approximating it.
`stanli_diagnose()` runs the checks a Bayesian workflow turns on,
including **E-BFMI** — the one that catches a badly explored heavy tail,
which R-hat and ESS are both blind to.

`optimize_model()` finds the posterior mode by L-BFGS, and its
`$unconstrained` element is what `sample_model(init = )` takes.

## How it is put together

Two pieces are not in the package, for two different reasons.

**The runtime** is a ~16 MB shared library holding stan-math, every
density kernel, and the interpreter. CRAN builds its own binaries from
source and would have to compile all of that to produce one, so
`stanli_install()` downloads the prebuilt library into the user cache
directory instead. Nothing is fetched without being asked for. Point
`STANLI_RUNTIME` at a local build to use one you built yourself.

The consequence is that this package and the library it calls are
separately versioned artifacts, and can drift. Getting that wrong is not
a crash: `stanli_sample_opts` is a struct this package declares a copy
of, so a field added on one side and not the other would be read at the
wrong offsets and sample happily from the wrong seed at the wrong step
size. So the C ABI carries a layout version, the runtime reports it
through `stanli_abi_version()`, and loading refuses on a mismatch with a
message saying which side to update. The release workflow asserts the
pinned release equals the tag it is cutting.

**The Stan compiler** is stanc3 compiled to JavaScript and run through
the V8 package — the same approach rstan uses to ship a Stan compiler on
CRAN. It is one 2.8 MB file that compresses to about 0.4 MB in the source
tarball, with no toolchain and no per-platform binaries. When the runtime
embeds stanc3 -- every release build but Windows, which waits on opam's
native Windows support -- that path is used instead and V8 is never
loaded; a native `stanc` in `STANLI_STANC` or beside the runtime also
wins, because it is faster than either. The Windows runtime tarball
carries `stanc.exe` next to the DLL for exactly that reason, so V8 is a
fallback there too rather than a requirement.

`tests/test_stancjs.cjs` in the main repository checks that the
JavaScript compiler emits the same MIR as the native binary, byte for
byte.

## Sampling in parallel

Chains run in parallel by default. Threading does not change the answer:
each chain owns its executor and its RNG stream, so a parallel run is
byte-identical to a sequential one, which the test suite asserts in a
form that holds on a single-threaded build too.

## What is not here yet

`init` is on the **unconstrained** scale. Unconstraining a user's
starting values needs the inverse parameter transforms, and stanli has
only the forward ones.

`optimize_model()` returns the posterior **mode**. CmdStan's `optimize`
defaults to `jacobian=0`, the penalized maximum likelihood, which stanli
cannot offer: the change-of-variables Jacobian is folded into the graph
when the model is lowered.
