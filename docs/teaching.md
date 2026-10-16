# Teaching with stanli

stanli runs Stan models in R with no C++ toolchain, so a class can start
sampling minutes after installing R.

## Before the first class

Students with supported macOS or Windows R installations need two lines. The
first also installs the tutorial packages from binaries:

```r
install.packages(c("stanli", "posterior", "bayesplot", "loo", "tidybayes"), repos = c("https://seantalts.r-universe.dev", "https://cloud.r-project.org"), type = "binary")
stanli::stanli_install()
```

`stanli_install()` downloads the prebuilt runtime, compiler included, from the
release the package is pinned to. Run it once per machine and again after a
package upgrade. Compiling and sampling make no downloads. See the
[R package install notes](../r/README.md#install) for source installs and
`STANLI_RUNTIME`.

### Linux labs

On Linux the ordinary repository URL selects source packages. R-universe's
Linux binary target is Ubuntu 26.04 (resolute), for R-release and R-devel on
x86_64 and arm64. Other distributions need a prepared lab image or a hosted
session; see the
[R-universe binary guide](https://docs.r-universe.dev/install/binaries.html).
On that Ubuntu target, replace the first line with:

```r
repos <- sprintf("https://%s.r-universe.dev/bin/linux/resolute-%s/%s/", c("seantalts", "cran"), R.version$arch, paste(R.version$major, sub("\\..*$", "", R.version$minor), sep = "."))
install.packages(c("stanli", "posterior", "bayesplot", "loo", "tidybayes"), repos = repos)
```

Then run `stanli::stanli_install()`. Check the example below on the exact lab
image before students arrive.

## Five-line eight-schools example

Paste these lines after installation. No model file or data download is
needed. The model is the noncentered eight-schools parameterization.

```r
library(stanli)
m <- stanli_model(code = "data { int J; vector[J] y; vector[J] sigma; } parameters { real mu; real<lower=0> tau; vector[J] z; } transformed parameters { vector[J] theta = mu + tau * z; } model { mu ~ normal(0,5); tau ~ cauchy(0,5); z ~ std_normal(); y ~ normal(theta,sigma); }", data = list(J=8L, y=c(28,8,-3,7,-1,1,18,12), sigma=c(15,10,16,11,9,11,10,18)))
fit <- sample_model(m, chains = 4, seed = 1, refresh = 0)
summary(fit); stanli_diagnose(fit)
bayesplot::mcmc_trace(as_draws_array(fit), pars = "mu", np = bayesplot::nuts_params(fit))
```

If the report flags divergences, rerun with `delta = 0.99` and check again.
[Coming from cmdstanr](from-cmdstanr.md) maps tutorial code to stanli,
including initial values, posterior predictive checks, LOO, tidybayes, and
saving a fit.

A fresh R session reaches this posterior in well under a second on current
hardware. `tools/bench_r_first_posterior.R` in the repository times it; run it
on the classroom machines before promising a number to students.

## Models from brms

Generate the Stan code and data with brms, then sample with stanli. Pass the
same formula, data, family and priors to both calls, and leave the generated
Stan text unedited:

```r
library(stanli)
d <- data.frame(x = seq(-1, 1, length.out = 20),
                y = c(-1.0, -0.6, -0.8, -0.4, -0.5, -0.1, -0.3, 0.1, 0.0, 0.3,
                      0.1, 0.5, 0.4, 0.8, 0.6, 1.0, 0.9, 1.3, 1.1, 1.5))
code <- brms::make_stancode(y ~ x, data = d, family = gaussian())
data <- brms::make_standata(y ~ x, data = d, family = gaussian())
model <- stanli_model(code = code, data = data)
fit <- sample_model(model, chains = 4, seed = 1, refresh = 0)
stanli_diagnose(fit)
```

This gives a `stanli_fit` with brms's parameter names; see `fit$columns`. To
get a full `brmsfit`, hand the converted fit to brms's `"mock"` backend. This
works with released brms (2.23) and needs RStan installed for the `stanfit`
class, but no C++ toolchain:

```r
bf <- brms::brm(y ~ x, data = d, family = gaussian(),
                backend = "mock", mock_fit = as_stanfit(fit))
summary(bf)
brms::conditional_effects(bf)
brms::loo(bf)
```

`summary()`, `posterior_predict()`, `log_lik()`, `loo()`,
`conditional_effects()` and `pp_check()` work on such a fit. Methods that
sample again (`update()`, `kfold()`) do not, because brms does not know how
the fit was made. A `backend = "stanli"` for brms itself exists as a branch
outside any brms release;
[output/brms-end-to-end](../output/brms-end-to-end/README.md) compares it with
the rstan backend on the corpus's brms formulas.

## Models from Rethinking

With rethinking 2.42, `sample = FALSE` returns the prepared model and data
without fitting:

```r
prepared <- rethinking::ulam(
  alist(y ~ dnorm(mu, sigma), mu ~ dnorm(0, 1), sigma ~ dexp(1)),
  data = list(y = c(-0.8, -0.2, 0.1, 0.4, 0.9)), sample = FALSE)
model <- stanli::stanli_model(code = prepared$model, data = prepared$data)
fit <- stanli::sample_model(model, chains = 4, seed = 1, refresh = 0)
stanli::stanli_diagnose(fit)
```

The result is a `stanli_fit`, so `ulam` methods do not apply to it. For a book model, keep its
data preparation. A model the book makes difficult on purpose has the same
poor diagnostics here; a different engine does not fix an unidentified model.

The brms and Rethinking models in the [corpus](corpus-status.md) are checked
against CmdStan in CI.

## Offline labs

Install R, stanli and the tutorial packages on the lab image while online, and
keep the binary installers or a repository snapshot for rebuilding it. The
runtime is a separate download: prepare one tarball per R architecture in the
room, from the release the installed package is pinned to.

| R platform | Runtime tarball | Library inside |
|---|---|---|
| macOS Apple Silicon | `stanli-runtime-darwin-arm64.tar.gz` | `libstanli.dylib` |
| macOS Intel | `stanli-runtime-darwin-x86_64.tar.gz` | `libstanli.dylib` |
| Windows x86_64 | `stanli-runtime-windows-x86_64.tar.gz` | `stanli.dll` |
| Linux x86_64 | `stanli-runtime-linux-x86_64.tar.gz` | `libstanli.so` |
| Linux arm64 | `stanli-runtime-linux-arm64.tar.gz` | `libstanli.so` |

For example, download the Apple Silicon runtime on a connected machine with
the lab's stanli version installed:

```r
tag <- paste0("v", packageVersion("stanli"))  # runtime releases match the package version
asset <- "stanli-runtime-darwin-arm64.tar.gz"
download.file(paste0("https://github.com/seantalts/stanli/releases/download/", tag, "/", asset), asset, mode = "wb")
```

Copy the tarball to the lab. Before loading stanli, extract all of its files;
on Windows the compiler and supporting DLLs must stay beside the runtime:

```r
dir.create("~/stanli-runtime", showWarnings = FALSE)
untar("stanli-runtime-darwin-arm64.tar.gz", exdir = "~/stanli-runtime")
Sys.setenv(STANLI_RUNTIME = path.expand("~/stanli-runtime/libstanli.dylib"))
library(stanli)
```

Put an absolute `STANLI_RUNTIME` path in the lab's `.Renviron` to keep it
across sessions, and skip `stanli_install()`. For R running under Rosetta, use
the Intel runtime even on Apple Silicon.

## Students without a laptop

The [browser build](https://seantalts.github.io/stanli/) runs Stan in a web
browser with model presets, sampling, plots and diagnostics, and needs no
install. R exercises still need an R session, such as a lab computer or a
hosted classroom environment.
