# Models from rethinking

This corpus contains every `ulam()` call in chapters 4–16 of Richard
McElreath's *Statistical Rethinking*, second edition: **61 call sites**, plus
one separately labeled hurdle example. There are 59 distinct Stan/data pairs;
repeated book fits remain in the inventory so coverage is auditable.

Every `.stan` is the unchanged `stancode()` output of rethinking 2.42 at the
[pinned commit](PROVENANCE.md). Every `.json` is the processed data list that
`ulam` would pass to Stan. No fitting takes place during generation.
[`tools/gen_rethinking_models.R`](../../tools/gen_rethinking_models.R) selects
formulas and preparation from the pinned book supplement rather than
transcribing them. It fails if any book call is missing from its inventory.

<!--gen:rethinking_verified-->62/62<!--/gen--> fixtures have recorded CmdStan
references: all 186 log-density/gradient points and all 186 output rows. The
recording build's worst scaled error was 1.48e-13. On the latest source build,
61/62 fixtures complete the replay, with a worst scaled error of 6.50e-14;
`m14.11` times out during preparation before numerical comparison
([#372](https://github.com/seantalts/stanli/issues/372)). This is pointwise
numerical coverage, not a claim that every posterior mixes well or that an
`ulam` backend already exists.

## What they cover

The chapter 9 fits include weakly identified regressions and prior-only
models. Chapters 11–12 cover binomial and Poisson regressions, `dbetabinom`,
`dgampois`, zero-inflated counts, custom conditional densities, ordered
categorical responses and monotonic predictors. Chapters 13–14 cover
multilevel centered and non-centered models, `gq>` blocks, `log_lik = TRUE`,
`vector[N]:` and `matrix[N,K]:` declarations, correlated effects, instrumental
variables, social networks, Gaussian processes and phylogenetic covariance.
Chapters 15–16 add measurement error, continuous and discrete missingness,
and nonlinear growth models.

Chapters 4–8 and 10 contain no `ulam()` calls in the second-edition supplement.
Their `quap()` models are not translated into new Stan programs. Direct
`stan()` calls (including the Boxes mixture and the chapter 16 ODE model),
exercises, package help examples and lecture-only models are outside this
book-call inventory. The book has ZIP models but no `ulam()` hurdle model;
`extra_hurdle_poisson` is a supplemental test of a Poisson distribution
truncated below at one, with a separate probability of zero.

### Book labels and repeated calls

- R code 9.29–9.30 still names its models `m5.8s`/`m5.8s2`; the second-edition
  leg simulation is R code 6.2. Filenames retain the supplied model names.
- R code 9.16 changes the chain count; it is `ch09_m9_1_chains4`.
- `m13.4b` uses the previous fit with a different `adapt_delta`. The public
  `ulam()` defaults apply, including its default `log_lik = FALSE`.
- R code 14.27 refits `m14.4x`/`m14.6x` before the changed simulation in 14.28
  in both published supplements. This generator preserves that order and
  therefore the original data, rather than silently moving the code box.

## Model inventory

[`inventory.tsv`](inventory.tsv) is generated with the fixtures. Code-box
numbers are **R code** numbers, not model numbers.

| File | Book model | R code |
| --- | --- | --- |
| [`ch09_m9_1.stan`](ch09_m9_1.stan) | `m9.1` | 9.14 |
| [`ch09_m9_1_chains4.stan`](ch09_m9_1_chains4.stan) | `m9.1` | 9.16 |
| [`ch09_m9_2.stan`](ch09_m9_2.stan) | `m9.2` | 9.22 |
| [`ch09_m9_3.stan`](ch09_m9_3.stan) | `m9.3` | 9.24 |
| [`ch09_m9_4.stan`](ch09_m9_4.stan) | `m9.4` | 9.26 |
| [`ch09_m9_5.stan`](ch09_m9_5.stan) | `m9.5` | 9.27 |
| [`ch09_mp.stan`](ch09_mp.stan) | `mp` | 9.28 |
| [`ch09_m5_8s.stan`](ch09_m5_8s.stan) | `m5.8s` | 9.29 |
| [`ch09_m5_8s2.stan`](ch09_m5_8s2.stan) | `m5.8s2` | 9.30 |
| [`ch11_m11_4.stan`](ch11_m11_4.stan) | `m11.4` | 11.11 |
| [`ch11_m11_5.stan`](ch11_m11_5.stan) | `m11.5` | 11.19 |
| [`ch11_m11_6.stan`](ch11_m11_6.stan) | `m11.6` | 11.25 |
| [`ch11_m11_7.stan`](ch11_m11_7.stan) | `m11.7` | 11.29 |
| [`ch11_m11_8.stan`](ch11_m11_8.stan) | `m11.8` | 11.32 |
| [`ch11_m11_9.stan`](ch11_m11_9.stan) | `m11.9` | 11.45 |
| [`ch11_m11_10.stan`](ch11_m11_10.stan) | `m11.10` | 11.45 |
| [`ch11_m11_11.stan`](ch11_m11_11.stan) | `m11.11` | 11.49 |
| [`ch11_m_pois.stan`](ch11_m_pois.stan) | `m_pois` | 11.61 |
| [`ch12_m12_1.stan`](ch12_m12_1.stan) | `m12.1` | 12.2 |
| [`ch12_m12_2.stan`](ch12_m12_2.stan) | `m12.2` | 12.6 |
| [`ch12_m12_3.stan`](ch12_m12_3.stan) | `m12.3` | 12.9 |
| [`ch12_m12_3_alt.stan`](ch12_m12_3_alt.stan) | `m12.3_alt` | 12.11 |
| [`ch12_m12_4.stan`](ch12_m12_4.stan) | `m12.4` | 12.16 |
| [`ch12_m12_5.stan`](ch12_m12_5.stan) | `m12.5` | 12.24 |
| [`ch12_m12_6.stan`](ch12_m12_6.stan) | `m12.6` | 12.34 |
| [`ch12_m12_7.stan`](ch12_m12_7.stan) | `m12.7` | 12.37 |
| [`ch13_m13_1.stan`](ch13_m13_1.stan) | `m13.1` | 13.2 |
| [`ch13_m13_2.stan`](ch13_m13_2.stan) | `m13.2` | 13.3 |
| [`ch13_m13_3.stan`](ch13_m13_3.stan) | `m13.3` | 13.13 |
| [`ch13_m13_4.stan`](ch13_m13_4.stan) | `m13.4` | 13.21 |
| [`ch13_m13_5.stan`](ch13_m13_5.stan) | `m13.5` | 13.23 |
| [`ch13_m13_6.stan`](ch13_m13_6.stan) | `m13.6` | 13.25 |
| [`ch13_m13_7.stan`](ch13_m13_7.stan) | `m13.7` | 13.26 |
| [`ch13_m13_7nc.stan`](ch13_m13_7nc.stan) | `m13.7nc` | 13.27 |
| [`ch13_m13_4b.stan`](ch13_m13_4b.stan) | `m13.4b` | 13.28 |
| [`ch13_m13_4nc.stan`](ch13_m13_4nc.stan) | `m13.4nc` | 13.29 |
| [`ch14_m14_1.stan`](ch14_m14_1.stan) | `m14.1` | 14.12 |
| [`ch14_m14_2.stan`](ch14_m14_2.stan) | `m14.2` | 14.18 |
| [`ch14_m14_3.stan`](ch14_m14_3.stan) | `m14.3` | 14.19 |
| [`ch14_m14_4.stan`](ch14_m14_4.stan) | `m14.4` | 14.24 |
| [`ch14_m14_5.stan`](ch14_m14_5.stan) | `m14.5` | 14.25 |
| [`ch14_m14_6.stan`](ch14_m14_6.stan) | `m14.6` | 14.26 |
| [`ch14_m14_4x.stan`](ch14_m14_4x.stan) | `m14.4x` | 14.27 |
| [`ch14_m14_6x.stan`](ch14_m14_6x.stan) | `m14.6x` | 14.27 |
| [`ch14_m14_7.stan`](ch14_m14_7.stan) | `m14.7` | 14.31 |
| [`ch14_m14_8.stan`](ch14_m14_8.stan) | `m14.8` | 14.39 |
| [`ch14_m14_8nc.stan`](ch14_m14_8nc.stan) | `m14.8nc` | 14.46 |
| [`ch14_m14_9.stan`](ch14_m14_9.stan) | `m14.9` | 14.49 |
| [`ch14_m14_10.stan`](ch14_m14_10.stan) | `m14.10` | 14.51 |
| [`ch14_m14_11.stan`](ch14_m14_11.stan) | `m14.11` | 14.52 |
| [`ch15_m15_1.stan`](ch15_m15_1.stan) | `m15.1` | 15.3 |
| [`ch15_m15_2.stan`](ch15_m15_2.stan) | `m15.2` | 15.5 |
| [`ch15_m15_3.stan`](ch15_m15_3.stan) | `m15.3` | 15.12 |
| [`ch15_m15_4.stan`](ch15_m15_4.stan) | `m15.4` | 15.13 |
| [`ch15_m15_5.stan`](ch15_m15_5.stan) | `m15.5` | 15.17 |
| [`ch15_m15_6.stan`](ch15_m15_6.stan) | `m15.6` | 15.19 |
| [`ch15_m15_7.stan`](ch15_m15_7.stan) | `m15.7` | 15.22 |
| [`ch15_m15_8.stan`](ch15_m15_8.stan) | `m15.8` | 15.30 |
| [`ch15_m15_9.stan`](ch15_m15_9.stan) | `m15.9` | 15.31 |
| [`ch16_m16_1.stan`](ch16_m16_1.stan) | `m16.1` | 16.2 |
| [`ch16_m16_4.stan`](ch16_m16_4.stan) | `m16.4` | 16.11 |
| [`extra_hurdle_poisson.stan`](extra_hurdle_poisson.stan) | `Supplemental hurdle Poisson` | - |

## The data and licenses

All transformations come from the book's preparation boxes: standardizing,
integer indexing, aggregation, tree pruning and covariance reordering.
The writer reads the Stan data declarations to preserve length-one arrays
and vectors. See [PROVENANCE.md](PROVENANCE.md) for package pins, original
dataset sources, licenses and every simulation seed. The imported fixtures
retain upstream GPL-3-or-later licensing; stanli's BSD license does not
replace it. No rethinking R implementation is vendored.

## Known failures

| Model | Diagnosis | Issue |
| --- | --- | --- |
| `ch14_m14_11` | The current source build times out after 300 seconds in graph-partitioning cost search, before the numerical check. Earlier builds verified its retained references; do not count that as a current pass. | [#372](https://github.com/seantalts/stanli/issues/372) |

Keep future failures here with a linked issue. Do not remove a difficult
model, alter its data, or regenerate an oracle to hide a disagreement.
Compiler fixes belong in separate changes.

The numerically verified ordered regressions also expose a performance gap:
`m12.5` takes longer than CmdStan under the shared sampling budget, and one
`m12.6` seed reaches the 900-second ceiling. Follow-up measurements and
reproduction commands are in [#373](https://github.com/seantalts/stanli/issues/373).
The timing appendix retains these cases and their diagnostic results.

## Regenerating and recording

Use the canonical CRAN R 4.6.1 ARM64 environment and package versions in
[PROVENANCE.md](PROVENANCE.md); that note explains the observed Homebrew R
rounding difference.
The exact installation commands are in the
[regeneration workflow](../../.github/workflows/rethinking.yml). No CmdStan
installation or model compilation is needed for generation.

```sh
Rscript tools/gen_rethinking_models.R
# Or check in a clean output directory:
Rscript tools/gen_rethinking_models.R /tmp/rethinking-generated
python3 tools/check_rethinking.py --generated /tmp/rethinking-generated
```

The generator downloads the supplement by immutable commit and verifies its
digest. For offline regeneration, set `RETHINKING_BOOK_CODE` to a local copy
of that exact file. The workflow runs nightly and on corpus pull requests,
comparing every generated byte against the committed artifacts. Numerical
replay runs on every push in `wheels.yml`, alongside the other corpora.

References live in `docs/corpus-refs.json.gz`, using the same pinned
CmdStan/Stan/Math/stanc toolchain and the same 1e-9 replay gate as `tests/brms`.
In this repository the recorder is `verify_sample.py`; `verify_refs.py` is
replay-only. After checking the pins under `deps/`, record a new fixture with:

```sh
python3 tools/verify_sample.py deps/cmdstan deps/posteriordb ch14_m14_8
python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check
python3 tools/check_rethinking.py
```

Review every recorder result, and commit `docs/corpus-refs.json.gz` and
`docs/verification.json` together. The inventory gate rejects missing
references; the default replay includes newly added rethinking fixtures
rather than silently skipping ones without a record. Output columns and
values are compared as well as log density and the full gradient.

The default benchmark suite includes this directory. The [version-3
protocol](../../docs/benchmark-protocol.md) measures gradients by default;
full inference is a separate opt-in phase. To measure this corpus alone:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  /tmp/rethinking-v2.tsv --corpus rethinking
python3 tools/corpus.py deps/posteriordb
python3 tools/gen_docs.py
```
