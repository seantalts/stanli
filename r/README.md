# stanli for R

Compile and sample Stan models without a C++ toolchain.

## Install

Not on CRAN yet. Until it is, install from R-universe or GitHub; the
package lives in the `r/` subdirectory of the repository:

```r
# prebuilt binaries from R-universe (recommended)
install.packages("stanli", repos = "https://seantalts.r-universe.dev")

# or from source
# install.packages("remotes")
# remotes::install_github("seantalts/stanli", subdir = "r")

# or from a checkout
# R CMD INSTALL r
```

On macOS and Windows the R-universe route installs a prebuilt binary. A
source install builds the 40 KB C bridge, so it wants the toolchain R
already expects for source packages: Xcode command line tools on macOS,
`r-base-dev` on Debian and Ubuntu, Rtools on Windows. The sampler
itself is not compiled here -- it arrives prebuilt below.

Then, once per machine:

```r
stanli_install()
```

That downloads the ~9-12 MB runtime for your platform into
`tools::R_user_dir("stanli", "cache")`, under the release the package
was built against, so the binding and the library always agree: after a
package upgrade the old runtime is simply not found and this step runs
once more. Nothing is fetched without it. Set `STANLI_RUNTIME` to use a
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

Four chains of eight schools (8000 draws, full summary and diagnostics)
take about 70 ms, because the model is lowered to a graph over
precompiled kernels rather than translated to C++ and compiled.

`summary()` uses stan's own estimators (rank-normalized split R-hat,
bulk/tail ESS, MCSE), so the numbers agree with `stansummary`.
`stanli_diagnose()` runs the checks a Bayesian workflow turns on,
including E-BFMI, the one that catches a badly explored heavy tail.
`optimize_model()` finds the posterior mode by L-BFGS, and its
`$unconstrained` element is what `sample_model(init = )` takes.

Chains run in parallel by default. Threading does not change the
answer: each chain owns its executor and its RNG stream, so a parallel
run is byte-identical to a sequential one, which the test suite
asserts.

## How it is put together

Two pieces are not in the package, for two different reasons.

**The runtime** is a ~29 MB shared library holding stan-math, every
density kernel, and the interpreter. CRAN builds its own binaries from
source and would have to compile all of that, so `stanli_install()`
downloads the prebuilt library instead. Because the package and the
library are separately versioned, they can drift, and drift here would
not crash: `stanli_sample_opts` is a struct this package declares a
copy of, so mismatched layouts would read fields at the wrong offsets
and sample happily from the wrong seed. So the C ABI carries a layout
version (`stanli_abi_version()`) and loading refuses on a mismatch with
a message saying which side to update.

**The Stan compiler** uses the embedded stanc3 and stanli OCaml pipeline in the
macOS and Linux release runtimes. On native hosts without an embedded compiler,
`STANLI_STANC` remains the first choice as an explicit stock-compiler override
for bisects. Otherwise R prefers
`stanli-compile` beside the runtime, then stock `stanc` beside the runtime or
on `PATH`, and finally stanc3 compiled to JavaScript through V8. The portable
producer is never taken from `PATH`, because it must match the runtime's
schema. Launch errors, nonzero exits, and empty output are reported; invalid
portable output is rejected when decoded. None causes an automatic retry
through stock stanc.

The v0.9.2 compatibility runtime carries pristine `stanc.exe`; the selection
logic falls through to it automatically. Windows runtime tarballs built from
this revision add `stanli-compile.exe` beside that rollback compiler for one
release cycle. Both are short-lived subprocesses; there is no OCaml compiler
DLL. The bundled JavaScript compiler records its exact stanc3 repository,
revision, and content hash. CI verifies that provenance and compiles valid and
invalid models through the file on Linux, macOS, and Windows.

## What is not here yet

`init` is on the **unconstrained** scale: unconstraining a user's
starting values needs the inverse parameter transforms, and stanli has
only the forward ones.

`optimize_model()` returns the posterior **mode**. CmdStan's `optimize`
defaults to `jacobian=0`, the penalized maximum likelihood, which
stanli cannot offer: the change-of-variables Jacobian is folded into
the graph when the model is lowered.
