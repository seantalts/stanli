# Provenance and licensing

## Source pin

Richard McElreath, *Statistical Rethinking: A Bayesian Course with Examples
in R and Stan*, second edition, CRC Press, 2020.

- Package: rethinking **2.42**, commit
  [`ac1b3b2cda83f3e14096e2d997a6e30ad109eeee`](https://github.com/rmcelreath/rethinking/tree/ac1b3b2cda83f3e14096e2d997a6e30ad109eeee).
- Formulas and preparation: the package's
  [`book_code_boxes.txt`](https://github.com/rmcelreath/rethinking/blob/ac1b3b2cda83f3e14096e2d997a6e30ad109eeee/book_code_boxes.txt),
  MD5 `92595f4b5a6c445ace6f5a9d0e24342f`. The generator checks this before
  evaluation and checks the installed package's `RemoteSha`.
- The author's [online code supplement](https://xcelab.net/rmpubs/sr2/code.txt)
  has the same 61 model names and code-box numbers (checked September 15, 2026).
- Generator environment: R 4.6.1, rethinking 2.42, cmdstanr 0.9.0,
  MASS 7.3-65, ape 5.8-1, nlme 3.1-169, mvtnorm 1.4-2; Darwin arm64.
  The regeneration workflow pins these versions. The phylogeny calculation
  uses ape's tree operations and Brownian covariance exactly as in the book.

The generator downloads the pinned supplement, evaluates the selected data
preparation and `ulam(..., sample = FALSE)` calls, and writes the returned
Stan string without formatting or syntax changes. No rethinking R
implementation or book code boxes are vendored. `sample = FALSE` returns a
list in 2.42; the generator puts its formula, data and model in an empty
`ulam` S4 shell for public `stancode()` dispatch and the book's repeated-fit
calls. It never samples or builds a CmdStan/RStan model.

## License record

The pinned package's [DESCRIPTION](https://github.com/rmcelreath/rethinking/blob/ac1b3b2cda83f3e14096e2d997a6e30ad109eeee/DESCRIPTION)
declares **GPL (>= 3)**, with Richard McElreath as author. Its data directory
and dataset documentation contain no separate dataset license declarations.
We retain that upstream license for these imported data fixtures and generated
Stan outputs; see [GPL-3](licenses/GPL-3). They are not relicensed under
stanli's BSD license. The independently authored generator and checking tools
remain under the repository's BSD license.

The author's book page, code supplement and
[sample chapters](https://xcelab.net/rmpubs/sr2/statisticalrethinking2_chapters1and2.pdf)
provide no separate dataset-license notice. This corpus uses
the data distributed in the GPL package and records their original sources
below; it does not claim that the book's prose is GPL or copy that prose.
Generated output can contain package-provided Stan helpers, so being output
of a program is not treated as a reason to discard upstream licensing.

## Data sources

Dataset names below refer to `data(..., package = "rethinking")` at the pin.
The citations are those supplied by its dataset documentation where present.

| Data | Source / use |
| --- | --- |
| `rugged` | Country ruggedness and GDP; Nunn and Puga (2012), *Ruggedness: The Blessing of Bad Geography in Africa*. The pinned package supplies `data/rugged.csv` without a dataset help page. |
| `chimpanzees` | Silk et al. (2005), Nature 437:1357–1359; prosocial choice experiment. |
| `UCBadmit` | 1973 UC Berkeley admissions counts; package `data/UCBadmit.csv`, without a dataset help page. |
| `Kline`, `Kline2`, `islandsDistMatrix` | Kline and Boyd (2010), Proc. R. Soc. B 277:2559–2564; tool counts, island covariates and distances. |
| `Trolley` | Cushman et al. (2006), Psychological Science 17:1082–1089. |
| `reedfrogs` | Vonesh and Bolker (2005), Ecology 86:1580–1591. |
| `KosterLeckie` (`kl_dyads`) | Koster and Leckie (2014), *Food sharing networks in lowland Nicaragua*, Social Networks. |
| `Primates301`, `Primates301_nex` | Street et al. (2017), [PNAS](https://doi.org/10.1073/pnas.1620734114); phylogeny: Arnold, Matthews and Nunn (2010), Evolutionary Anthropology 19:114–118. |
| `WaffleDivorce` | 2009 American Community Survey marriage/divorce rates, 1860 census and Waffle House locations; source details in `man/WaffleDivorce.Rd`. |
| `milk` | Hinde and Milligan (2011), Evolutionary Anthropology 20:9–23. |
| `Howell1` | Nancy Howell's !Kung demography data; [University of Toronto archive](https://tspace.library.utoronto.ca/handle/1807/10395). |
| `Panda_nuts` | Boesch et al. (2019), [Scientific Reports](https://doi.org/10.1038/s41598-018-38392-8). |

JSON contains `ulam`'s processed Stan data, including dimension constants,
integer coercions, missing-data sentinels and imputation indices. Standardizing,
indexing and phylogenetic reordering use the book's R expressions. Nothing
is rounded to make regeneration or numerical comparison pass.

## Simulations and seeds

RNG kinds are Mersenne-Twister, Inversion, Rejection. The generator starts
at seed 20260915; each book `set.seed()` in the selected boxes takes precedence.
All random *data* used by the book fixtures have explicit book seeds:

| Fixtures | Preparation box | Seed |
| --- | --- | ---: |
| `ch09_m5_8s`, `ch09_m5_8s2` | 6.2, correlated legs | 909 |
| `ch09_m9_4`, `ch09_m9_5` | 9.25, normal observations | 41 |
| `ch12_m12_3`, `ch12_m12_3_alt` | 12.7, zero-inflated counts | 365 |
| `ch13_m13_3` | 13.8–13.11, pond effects and survivors | 5005 |
| `ch14_m14_1` | 14.7 effects; 14.10 waiting times | 5; 22 |
| `ch14_m14_4`–`ch14_m14_6`, `ch14_m14_4x`, `ch14_m14_6x` | 14.23, instrumental variables | 73 |
| `ch15_m15_3`, `ch15_m15_4` | 15.11, missingness simulation | 501 |
| `ch15_m15_8`, `ch15_m15_9` | 15.29, missing cats | 9 |

`extra_hurdle_poisson` is an independently authored supplemental example
with fixed integer data, not a book model. Its Stan is likewise unmodified
`ulam` output. The book's sampler seeds do not affect these fixtures because
sampling is disabled.
