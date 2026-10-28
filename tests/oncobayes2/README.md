# Model from OncoBayes2

[OncoBayes2](https://cran.r-project.org/package=OncoBayes2) fits Bayesian
logistic regression models for dose escalation in oncology trials. Its one
Stan model, `inst/stan/blrm_exnex.stan`, handles single agents and drug
combinations with an exchangeable/non-exchangeable (EXNEX) prior across
trial groups. [Issue #523](https://github.com/seantalts/stanli/issues/523)
came from this model: its transformed data fills a 2-D int array by rows,
and stanli returned INT_MIN when reading it.

The model comes from the CRAN release 0.10-0:

- URL `https://cran.r-project.org/src/contrib/OncoBayes2_0.10-0.tar.gz`
- `inst/stan/blrm_exnex.stan`, SHA-256
  `018a44f4a830410f08e1a9ef9beef45fd6419cec27f5703f7c46860c44e56bc0`
- `inst/stan/include/utils.stan`, SHA-256
  `ee3512fada223fd2f3135fadf3a2a72767c118e13ab2bc0a5da5037933b7f945`
- `inst/stan/include/copyright_novartis.stan`, SHA-256
  `f846dfd08671fdbd1e0b7e71ec5879bf84a67f4aab0d5d6d97d2f2656f9ea464`
- `inst/stan/include/license.stan`, SHA-256
  `49db4c73c23bfadf9c2b9f55773fd944d1a8a64aada3897afdfeb91eeee7f405`
- Copyright (C) 2025 Novartis Pharma AG, license GPL (>= 3), text in
  [`licenses/GPL-3`](licenses/GPL-3) and
  [`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md)

`blrm_exnex.stan` has one `#include /include/utils.stan` line, and the corpus
pairs one self-contained `.stan` file with one `.json` file. The checked-in
model is `blrm_exnex.stan` with that line replaced by the text of `utils.stan`,
preceded by the package's two header files, `copyright_novartis.stan` and
`license.stan`, which hold only comments. Nothing else is changed. The
generated C++ from stanc3 is identical to the original's apart from source
locations and the `--include-paths` flag.

There are three entries, one per example model in the package's
`example_model()`:

| Entry | Example | Observations | Compounds | Interactions | Groups | EXNEX |
| --- | --- | --- | --- | --- | --- | --- |
| `oncobayes2_blrm_exnex_sa` | `single_agent` | 5 | 1 | 0 | 1 | no |
| `oncobayes2_blrm_exnex_combo2` | `combo2` | 27 | 2 | 1 | 4 | no |
| `oncobayes2_blrm_exnex_combo3` | `combo3` | 18 | 3 | 4 | 3 | compounds |

The three `.stan` files are byte-identical, because the corpus inventory pairs
each model file with one data file. Each `.json` file is the `$standata` of the
`blrmfit` object the example creates, restricted to the data block's fields.
[`tools/gen_oncobayes2_data.R`](../../tools/gen_oncobayes2_data.R) writes all
six files from the installed package, after checking the four hashes above.
Rerunning

    Rscript tools/gen_oncobayes2_data.R tests/oncobayes2

reproduces them byte for byte.

The models go through the same oracle as the rest of the corpus: CmdStan's
recorded log density, gradient and `write_array` rows in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py). The references were
recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan `a6806ef8` and Math `5252d51d`
on Darwin arm64, and each entry carries that as its own provenance block.
