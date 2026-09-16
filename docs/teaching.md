# Teaching Bayesian workflow with stanli

For model coverage, numerical evidence, performance results, and examples
using brms or Rethinking, see [Teaching support](teaching-support.md).

## Before the first class

Use **R-universe binaries**. Give students with supported macOS or Windows R
installations these two lines (the first also installs the tutorial packages):

```r
install.packages(c("stanli", "posterior", "bayesplot", "loo", "tidybayes"), repos = c("https://seantalts.r-universe.dev", "https://cloud.r-project.org"), type = "binary")
stanli::stanli_install()
```

`stanli_install()` downloads the prebuilt runtime tarball for the platform and
R architecture, including its compiler, from the package's pinned GitHub
release. It extracts into
`file.path(tools::R_user_dir("stanli", "cache"), "v0.14.2")` for this package
version. `stanli::stanli_runtime_path()` prints the actual library path. Run
installation once per machine and again after a package upgrade changes the
runtime pin; model compilation and sampling make no downloads.

The performance report identifies its development runtime by source revision.
Classroom installations use the package's pinned public release; performance
changes reach those installations when a release includes them.

### Linux labs

The ordinary repository URL can select source packages on Linux. Instructors
should provision a matching binary repository and system libraries before
class. R-universe's current Linux binary target is Ubuntu 26.04 (resolute), for
R-release and R-devel on x86_64 and arm64; other distributions need a prepared
lab image or hosted session. See the
[R-universe binary installation guide](https://docs.r-universe.dev/install/binaries.html)
for supported targets and changes. On that Ubuntu target, replace the first
installation line with:

```r
repos <- sprintf("https://%s.r-universe.dev/bin/linux/resolute-%s/%s/", c("seantalts", "cran"), R.version$arch, paste(R.version$major, sub("\\..*$", "", R.version$minor), sep = "."))
install.packages(c("stanli", "posterior", "bayesplot", "loo", "tidybayes"), repos = repos)
```

Run `stanli::stanli_install()` next. Verify the installed binaries and the
example below on the exact lab image before students arrive. An instructor
must provision source-only dependencies ahead of class if binaries are absent.

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

If the report flags divergences, rerun with `delta = 0.99` and check the
diagnostics again.

For the table used in cmdstanr tutorials, use
`posterior::summarise_draws(as_draws_array(fit))`. See
[Coming from cmdstanr](from-cmdstanr.md) for the full translation table, initial
values, posterior predictive checks, LOO, tidybayes, and saving a fit.

## Offline labs

Install R, the stanli R package, and the tutorial packages on the lab image while
online. Keep their binary installers or a repository snapshot for rebuilding
that image. The runtime is a separate download: prepare one tarball for each
R architecture in the room, from the **release pinned by that package**.

| R platform | Runtime tarball | Library inside |
|---|---|---|
| macOS Apple Silicon | `stanli-runtime-darwin-arm64.tar.gz` | `libstanli.dylib` |
| macOS Intel | `stanli-runtime-darwin-x86_64.tar.gz` | `libstanli.dylib` |
| Windows x86_64 | `stanli-runtime-windows-x86_64.tar.gz` | `stanli.dll` |
| Linux x86_64 | `stanli-runtime-linux-x86_64.tar.gz` | `libstanli.so` |
| Linux arm64 | `stanli-runtime-linux-arm64.tar.gz` | `libstanli.so` |

For example, download the Apple Silicon runtime on a connected machine:

```r
tag <- "v0.14.2"  # match the lab's package runtime pin
asset <- "stanli-runtime-darwin-arm64.tar.gz"
download.file(paste0("https://github.com/seantalts/stanli/releases/download/", tag, "/", asset), asset, mode = "wb")
```

Copy the tarball to the lab. Before loading stanli, extract **all** its files
(the Windows compiler and supporting DLLs must stay beside the runtime):

```r
dir.create("~/stanli-runtime", showWarnings = FALSE)
untar("stanli-runtime-darwin-arm64.tar.gz", exdir = "~/stanli-runtime")
Sys.setenv(STANLI_RUNTIME = path.expand("~/stanli-runtime/libstanli.dylib"))
library(stanli)
```

Use the appropriate tarball/library from the table on other platforms. Set an
absolute `STANLI_RUNTIME` path in the lab's `.Renviron` to persist it across
sessions. With that path set, skip `stanli_install()` entirely. For R running
under Rosetta, use the Intel runtime even on Apple Silicon hardware.

## Time from a fresh R session to the first posterior

`tools/bench_r_first_posterior.R` launches three separate `Rscript --vanilla`
processes and records wall time through completed sampling: process startup,
loading stanli and its installed runtime, compiling the example, and four chains
with 1000 warmup plus 1000 post-warmup draws per chain. Installation/download,
summaries, and plotting are excluded. These are fresh sessions on a running
machine, not cold filesystem-cache measurements.

| Platform | Measured median wall time | Evidence |
|---|---|---|
| macOS arm64, macos-15 CI runner | 0.152 s | 0.157, 0.149, 0.152 s |
| Linux x86_64, ubuntu-24.04 CI runner | 0.217 s | 0.216, 0.220, 0.217 s |
| Windows x86_64, windows-2022 CI runner | 0.230 s | 0.230, 0.240, 0.220 s |

All three platforms use R 4.6.1 and the development runtime built in
[CI run 35060736527](https://github.com/seantalts/stanli/actions/runs/35060736527),
revision `6e462c2e`.
The same jobs pass the R ecosystem acceptance suite, including the independent
CSV oracle and native density/transform methods.

The [wheels workflow](../.github/workflows/wheels.yml) records Linux timings
against its freshly built runtime on source PRs, and macOS/Windows timings on
full manual runs, after merge, nightly, and on releases. Its job summaries and downloadable CSV
artifacts include the platform, R/package versions, and all three timings.
The CSV artifacts are `r-first-posterior-darwin-arm64`,
`r-first-posterior-linux`, and `r-first-posterior-windows-x86_64`.
Re-run on the classroom hardware before promising a timing to students.

## Students without a laptop

The [browser build](https://seantalts.github.io/stanli/) runs Stan locally in a
web browser with model presets, sampling, plots, and diagnostics. Students can
use a shared computer or another supported device without installing R. R
package exercises still require access to an R session, such as a lab computer
or a hosted classroom environment.
