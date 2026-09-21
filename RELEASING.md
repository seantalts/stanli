# Releasing

`.github/workflows/wheels.yml` builds five wheels: macOS arm64 and x86_64,
manylinux_2_28 x86_64 and aarch64, and Windows x86_64. Pull requests with
source changes run the manylinux x86_64 build plus a compiler-only Windows
gate that cross-builds both compiler artifacts and byte-compares the
portable producer with the JavaScript producer. Prose-only changes take a
static path; the allowlist fails closed, so an unfamiliar path gets the
full build. All five builds run after merge, nightly, and on release tags.
The four Unix wheels link the embedded compiler; Windows packages
`stanli-compile.exe` and stock `stanc.exe` beside `stanli.dll`. Every wheel
runs the test suite, checks its platform contract, and samples eight
schools from a clean installed environment.

## Cutting a release

Bump the version in `python/stanli/__init__.py`, `js/package.json`,
`r/DESCRIPTION`, and `r/R/install.R` in one commit (`stanli_runtime_release`
there pins the runtime the R package downloads), add a `CHANGELOG.md`
entry, then tag:

```
git tag -a v0.1.0 -m "stanli 0.1.0" && git push origin v0.1.0
```

One `v*` tag publishes every channel: PyPI, npm, and the GitHub runtime
release the R package installs from. The publish jobs assert the tag
matches each manifest, so a forgotten bump fails the release rather than
shipping channels that disagree. Uploads go through PyPI and npm trusted
publishing; no API token exists in the repo, and the `pypi` and `npm`
deployment environments are restricted to release tags. Before any
publisher runs, the workflow rebuilds the downstream `stanr` package
against the tagged runtime and runs its Stanli-backend tests.

No sdist is published. Building from source needs a 30-minute OCaml
toolchain step, so an sdist would only turn "no wheel for your platform"
into a confusing build failure.

## R

The `runtime-release` job attaches the runtime tarballs
(`stanli-runtime-<os>-<arch>.tar.gz`, what `stanli_install()` downloads)
and the R source package to the GitHub Release. R-universe's registry
tracks `*release`, so a new release advances the package automatically;
`remotes::install_github("seantalts/stanli", subdir = "r")` builds the
bridge from any commit. Both routes call `stanli_install()` to fetch the
pinned runtime.

Bumping `STANLI_ABI_VERSION` in `runtime/include/stanli/capi.h` means
bumping `STANLI_R_ABI_VERSION` in `r/src/bridge.c` too; `r.yml` fails if
they disagree. Bump it when a C ABI struct changes layout or a function
changes signature; adding a function does not need one.

The R sampling tests run in `wheels.yml` against the Linux library that
build produced and fail if skipped; `r.yml` separately checks the package
without a runtime.

## npm

`@seantalts/stanli` rides the same `v*` tag. An `npm-vX.Y.Z` tag republishes
npm alone when npm fails on a release that already went out everywhere
else. Three quirks: a trusted publisher attaches only to a package that
already exists, so a package's first version goes out by hand with
`npm publish`, and the publisher is configured on npmjs.com (Settings,
Trusted publisher: GitHub Actions, `seantalts` / `stanli` / `wheels.yml` /
environment `npm`). The package is scoped because npm's name-similarity
filter rejects unscoped `stanli`, which is why `publishConfig.access` is
public. And the job passes `actions/setup-node` no `registry-url`: given
one it writes an `.npmrc` carrying a placeholder token, npm sees
credentials and never attempts the OIDC exchange, and the registry
answers a write it cannot authorize with 404 rather than 401.
