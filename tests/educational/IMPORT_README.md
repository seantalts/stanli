# Stan teaching-model corpus for Stanli

**Curated pilot • inspected 13 September 2026**

This archive contains **13 complete-form Stan programs** transcribed from public
Aalto-associated teaching demonstrations, paired JSON data, source/license records,
and reference runners. It is **not an exhaustive course/repository inventory**,
a raw repository mirror, or a Stanli compatibility certification.

**No Stan program was compiled or executed here.** All 13 passed local lexical,
file-hash, and hand-specified fixture-shape checks. Those checks do not establish
Stan syntax/type correctness, sampling quality, or Stanli support.

## What is included

| Component | Contents |
|---|---|
| `models/` | 13 `model.stan` files and 13 matching `data.json` files |
| `manifest.csv`, `manifest.json` | Source page/section, original filename, authors, interface, family, features, fixture provenance, license, local hashes, validation status |
| `references.csv`, `references.json` | 29 additional records: complete source programs held back, wrapper recipes, fragments, assignment prompts and unresolved leads |
| `source_inventory.csv`, `source_inventory.json` | Scope-limited course comparison; unknowns are not reported as zero |
| `feature_priority.csv` | Suggested feature-testing order, not measured Stanli failures |
| `scripts/audit.py` | Standard-library-only integrity and fixture audit |
| `scripts/run_cmdstanpy.py`, `scripts/run_cmdstanr.R` | Newly written reference runners; require an existing CmdStan installation |
| `reports/` | Audit results, provenance/limitations, source comparison and suggested testing plan |
| `licenses/` | Upstream BSD-3 notice and applicable supplemental notice |
| `SHA256SUMS` | Checksums of delivered files, excluding the checksum file itself |

Twelve model folders also include a small `source_data_excerpt.R`. These are
attributed data-construction excerpts/adaptations, **not complete upstream driver
scripts**. The generalized-Pareto folder has no upstream driver excerpt.

### Data provenance

**Six models** have data values transcribed from the source lesson. The two
Poisson models share the same complete, course-generated 500-count dataset.
**Seven models** have newly constructed, explicitly labelled synthetic smoke-test
fixtures because the external original datasets were not downloaded. The seven
synthetic pairs are the three regressions, three grouped/hierarchical examples,
and generalized Pareto example. These must not be described as the original
course datasets or used to reproduce the lesson's published numerical results.

## Bundled model inventory

| ID suffix (`aalto_…`) | Model | Data |
|---|---|---|
| `bern` | Bernoulli with generated pointwise log likelihood | Original inline |
| `binom` | Binomial with beta prior | Original inline |
| `binomb` | Binomial with normal prior on logit | Original inline |
| `binom2` | Two-group binomial with odds ratio | Original inline |
| `lin` | Gaussian regression with data-supplied priors | New synthetic |
| `lin_std` | Standardized Gaussian regression | New synthetic |
| `lin_std_t` | Standardized robust Student-t regression | New synthetic |
| `grp_aov` | Group means with shared variance | New synthetic |
| `grp_prior_mean` | Hierarchical group means | New synthetic |
| `grp_prior_mean_var` | Hierarchical group means and variances | New synthetic |
| `poisson_simple` | Poisson with replicated counts and log likelihood | Original inline, 500 counts |
| `poisson_hurdle` | Hurdle count model with upper-truncated Poisson | Same original 500 counts |
| `gpareto` | User-defined generalized Pareto probability functions | New synthetic |

The first ten programs come from the [CmdStanR demo](https://avehtari.github.io/BDA_R_demos/demos_rstan/cmdstanr_demo.html).
The two count models come from the [posterior predictive checking lesson](https://avehtari.github.io/BDA_R_demos/demos_rstan/ppc/poisson-ppc.html).
The Pareto program is an [official Stan case study](https://mc-stan.org/learn-stan/case-studies/gpareto_functions.html)
linked from the [Aalto demo index](https://avehtari.github.io/BDA_R_demos/demos_rstan/).
The link establishes a connection to the demo collection, not that every course
cohort was assigned this optional case study.

Original teaching drivers are **R**: CmdStanR for the core set and RStan for the
PPC and Pareto lessons. The Python runner in this archive is new, not harvested
from a Python class. The separate Aalto Python repository was only screened.

## Running local checks

From the extracted archive directory:

```sh
python scripts/audit.py
```

This requires only Python 3.10+ and does not run any Stan backend. The saved
`reports/audit.json` records the checks performed during assembly.

On a machine with a working CmdStanPy/CmdStan installation:

```sh
# Default is compilation only; no dependencies are installed automatically.
python scripts/run_cmdstanpy.py --mode compile

# Explicitly request a small reference sampling run.
python scripts/run_cmdstanpy.py --mode sample --models aalto_bern aalto_binom
```

Or, with CmdStanR, jsonlite and CmdStan already installed:

```sh
Rscript scripts/run_cmdstanr.R compile
Rscript scripts/run_cmdstanr.R sample aalto_bern aalto_binom
```

Both runners preserve a new results directory for each invocation and report
individual errors rather than silently dropping failed models. Sampling defaults
are small smoke-test settings, not a recommendation for production inference.
Compilation happens next to each model source, so use a writable checkout.
The reference runners were **not backend-tested here**; Python syntax and the
missing-dependency error path were checked. The R script was not executed.
No Stanli adapter is invented: integrate the manifest's model/data paths with the
actual API of the Stanli checkout being tested.

## Extraction and deduplication

Bulk repository downloads were unsuccessful in this environment. The code was
therefore transcribed from fully displayed source blocks, with most teaching
comments removed, formatting normalized, and attribution added. There were no
intended changes to executable code. Source commits are **not pinned** and these
are **not byte-for-byte repository downloads**. Local SHA256 values identify the
specific delivered files, not a verified upstream commit. Source sections and
filenames are recorded to support a future raw-source comparison.

There are no duplicate token-normalized programs within the 13-file bundle.
Normalization ignores whitespace and comments, retains token boundaries, and
does not rename variables, fold constants, or prove semantic equivalence.
Related variants are intentionally retained when priors, parameterizations,
constraints, generated quantities, or model dimensions differ. For example,
normal priors on logit parameters are not equivalent to uniform probability
priors. Standardized regression variants are not assumed to have identical priors.

The nine `model_family` buckets are hand-assigned organizational labels, not an
objective measure of statistical novelty. ANOVA/grouped Gaussian versus
hierarchical Gaussian, for example, are split by model structure. Do not use raw
file counts or those buckets as an exhaustive course-variety ranking.

## Important source findings

The earlier blanket ranking of UBC first and HEC second is **not established by
this inventory**. Aalto is the best verified, permissively licensed starting
bundle from this pass. UBC has complementary censoring, beta-proportion,
logistic, and marginalized-mixture programs, but no clearly applicable reuse
license was located; their code is not redistributed here.

Some earlier examples were overcounted: UBC sunspots is an assignment prompt,
not supplied Stan source; several Aalto advanced examples are wrapper recipes,
not captured generated Stan; and the previously linked HEC assignment returned
404. See `reports/source_comparison.md` and the reference manifest for details.

## License scope

Bundled source code follows the declared BSD-3 terms of the R demo repository
or the Pareto case-study appendix. Copyright notices and author attribution are
retained. No entire teaching articles, slide decks, or lecture prose are bundled.
The HEC website's MIT **template** license is not the license for its course
content. The separate Aalto Python repository advertises GPL-3.0, so the R
repository's BSD declaration must not be applied to it indiscriminately.

New metadata, hand-built smoke fixtures and helper scripts are additions to this
collection; they are not endorsed by any course instructor. Nothing in this
archive changes the upstream authors' rights. See `reports/provenance.md`.
