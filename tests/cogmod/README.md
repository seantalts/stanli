# Models from cogmod

[cogmod](https://github.com/DominiqueMakowski/cogmod) is an R package of
custom brms families for cognitive models: reaction-time distributions and
sequential-sampling choice models whose densities branch on parameter
values. [Issue #422](https://github.com/seantalts/stanli/issues/422)
reported that this code shape compiles slowly under stanli; this corpus is
the fixture set that investigation used, checked in so the fix and its
numerical correctness stay tested.

Every `.stan` and `.json` here is `make_stancode` and `make_standata`
output from brms 2.23.0 against cogmod dev commit
[`732be30bdae2a4cc7f38cf78b5ca7cfeb85dc732`](https://github.com/DominiqueMakowski/cogmod/tree/732be30bdae2a4cc7f38cf78b5ca7cfeb85dc732)
(MIT; not yet a tagged release), written out unchanged.
[`tools/gen_cogmod_models.R`](../../tools/gen_cogmod_models.R) is the
generator; it needs brms and cogmod installed from that commit, and
rerunning it reproduces this directory byte for byte. Model ids are
prefixed `cm_` so they cannot collide with the rest of the corpus.

A generated model's `functions {}` block includes Stan code authored in
cogmod for its custom density. See [`licenses/MIT`](licenses/MIT) and
[`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md).

These models go through the same oracle as the corpus: CmdStan's recorded
log density and full gradient in `docs/internal/artifacts/corpus-refs.json.gz`,
replayed by [`tools/verify_refs.py`](../../tools/verify_refs.py) in CI on
every push. The shared [inventory](../../tools/corpus_inventory.py) resolves
model names, source/data paths and provenance for numerical replay and
benchmarks. Unlike `tests/brms`, these points are held to the 1e-9
scaled-error gate rather than a same-platform ULP limit: there is no Linux
x86_64 recording for this collection, so every platform keeps the
documented scaled-error checks. Rejections and non-finite classifications
must agree everywhere. This is a recorded regression contract, not a bound
for every possible data set or parameter value.

## What they cover

21 families (lognormal, logstudent, loggamma, invgaussian, exwald, bisa,
gamma, invgamma, weibull, invweibull, logweibull, lba1, exgaussian, geg,
rdm, lba2, ddm, lnr, betagate, choco, betadiscrete) plus `cm_lnr_bench`,
which reproduces issue #422's original report: the log-normal race model
fit to real `speed_acc` reaction-time data (rtdists; participants 1-3,
RT <= 2s, every third row kept to stay under 100 KB; the generator's
header comment explains why that still spans every participant and
condition). Every other family simulates about 200 observations from its
own `rcogmod_*()`, with one covariate on its main location parameter and
every other declared parameter intercept-only. No parameter is pinned to a
constant, even where cogmod's own docs recommend it for identifiability:
the point of this corpus is the parameter-dependent branch, and a pinned
parameter is a step away from becoming a compile-time constant at the call
site, which is the opposite of what is wanted.
[`tools/gen_cogmod_models.R`](../../tools/gen_cogmod_models.R) has the
exact formulas and simulation parameters.

## Regenerating and recording

```
Rscript tools/gen_cogmod_models.R tests/cogmod path/to/speed_acc.RData
python3 tools/verify_sample.py deps/cmdstan deps/posteriordb cm_gamma
```

See the generator's header comment for how to obtain `speed_acc.RData`
without installing rtdists (which needs libgsl). The recorder stamps the
file with the revision of every checkout under `deps/`, and refuses to
merge into a file recorded against a different one; read the per-point
lines before committing, since a `MISMATCH` on a new model is a finding.
