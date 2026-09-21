# Teaching Bayesian workflow with stanli

## Before the first class

Use R-universe binaries. Students with supported macOS or Windows R
installations need two lines; the first also installs the tutorial packages:

```r
install.packages(c("stanli", "posterior", "bayesplot", "loo", "tidybayes"), repos = c("https://seantalts.r-universe.dev", "https://cloud.r-project.org"), type = "binary")
stanli::stanli_install()
```

`stanli_install()` downloads the prebuilt runtime for the platform and R
architecture, including its compiler, from the GitHub release the package is
pinned to, and extracts it under `tools::R_user_dir("stanli", "cache")` in a
directory named after that release. `stanli::stanli_runtime_path()` prints the
library path. Run it once per machine and again after a package upgrade;
compilation and sampling make no downloads.

### Linux labs

On Linux the ordinary repository URL selects source packages. R-universe's
current Linux binary target is Ubuntu 26.04 (resolute) for R-release and
R-devel on x86_64 and arm64; other distributions need a prepared lab image or a
hosted session. See the
[R-universe binary installation guide](https://docs.r-universe.dev/install/binaries.html)
for supported targets. On that Ubuntu target, replace the first line with:

```r
repos <- sprintf("https://%s.r-universe.dev/bin/linux/resolute-%s/%s/", c("seantalts", "cran"), R.version$arch, paste(R.version$major, sub("\\..*$", "", R.version$minor), sep = "."))
install.packages(c("stanli", "posterior", "bayesplot", "loo", "tidybayes"), repos = repos)
```

then run `stanli::stanli_install()`. Verify the example below on the exact lab
image before students arrive.

## Five-line eight-schools example

Paste these five lines after installation; no model file or downloaded data is
needed. The model uses the noncentered eight-schools parameterization.

```r
library(stanli)
m <- stanli_model(code = "data { int J; vector[J] y; vector[J] sigma; } parameters { real mu; real<lower=0> tau; vector[J] z; } transformed parameters { vector[J] theta = mu + tau * z; } model { mu ~ normal(0,5); tau ~ cauchy(0,5); z ~ std_normal(); y ~ normal(theta,sigma); }", data = list(J=8L, y=c(28,8,-3,7,-1,1,18,12), sigma=c(15,10,16,11,9,11,10,18)))
fit <- sample_model(m, chains = 4, seed = 1, refresh = 0)
summary(fit); stanli_diagnose(fit)
bayesplot::mcmc_trace(as_draws_array(fit), pars = "mu", np = bayesplot::nuts_params(fit))
```

If the report flags divergences, rerun with `delta = 0.99` and check again.
[Coming from cmdstanr](from-cmdstanr.md) has the full translation table,
initial values, posterior predictive checks, LOO, tidybayes, and saving a fit.

## Models from brms and Rethinking

Both routes produce a `stanli_fit`, not a `brmsfit` or a fitted `ulam`
object, so package methods such as `brms::conditional_effects()` do not apply
to the result. Parameter names are the ones the package generates; see
`fit$columns`. Pass the same formula, data, family and priors to both `make_*`
calls rather than editing the generated Stan text:

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

For a book model, keep its data preparation. A model the book makes
deliberately difficult has the same poor diagnostics here; changing the
engine does not fix an unidentified model. The brms and Rethinking fixtures in
the [corpus](corpus-status.md) are replayed against CmdStan in CI.

## Offline labs

Install R, the stanli R package, and the tutorial packages on the lab image
while online, and keep their binary installers or a repository snapshot for
rebuilding it. The runtime is a separate download: prepare one tarball for
each R architecture in the room, from the release the installed package is
pinned to. `stanli:::stanli_runtime_release` prints that tag.

| R platform | Runtime tarball | Library inside |
|---|---|---|
| macOS Apple Silicon | `stanli-runtime-darwin-arm64.tar.gz` | `libstanli.dylib` |
| macOS Intel | `stanli-runtime-darwin-x86_64.tar.gz` | `libstanli.dylib` |
| Windows x86_64 | `stanli-runtime-windows-x86_64.tar.gz` | `stanli.dll` |
| Linux x86_64 | `stanli-runtime-linux-x86_64.tar.gz` | `libstanli.so` |
| Linux arm64 | `stanli-runtime-linux-arm64.tar.gz` | `libstanli.so` |

For example, download the Apple Silicon runtime on a connected machine:

```r
tag <- stanli:::stanli_runtime_release  # the release this package pairs with
asset <- "stanli-runtime-darwin-arm64.tar.gz"
download.file(paste0("https://github.com/seantalts/stanli/releases/download/", tag, "/", asset), asset, mode = "wb")
```

Copy the tarball to the lab. Before loading stanli, extract all of its files;
the Windows compiler and supporting DLLs must stay beside the runtime:

```r
dir.create("~/stanli-runtime", showWarnings = FALSE)
untar("stanli-runtime-darwin-arm64.tar.gz", exdir = "~/stanli-runtime")
Sys.setenv(STANLI_RUNTIME = path.expand("~/stanli-runtime/libstanli.dylib"))
library(stanli)
```

Set an absolute `STANLI_RUNTIME` path in the lab's `.Renviron` to persist it
across sessions. With that path set, skip `stanli_install()`. For R running
under Rosetta, use the Intel runtime even on Apple Silicon hardware.

<a id="time-from-a-fresh-r-session-to-the-first-posterior"></a>
## Time to the first posterior

`tools/bench_r_first_posterior.R` times fresh `Rscript --vanilla` sessions
from process startup through four chains of the example above, excluding
installation, summaries and plotting. CI records the result on every platform
under `output/teaching-performance/`; on current runners a session takes well
under a second. Rerun it on the classroom hardware before promising a timing
to students.

## Students without a laptop

The [browser build](https://seantalts.github.io/stanli/) runs Stan in a web
browser with model presets, sampling, plots, and diagnostics, with no install.
R package exercises still need an R session, such as a lab computer or a
hosted classroom environment.
