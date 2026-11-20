# Models from hBayesDM

[hBayesDM](https://cran.r-project.org/package=hBayesDM) fits hierarchical
Bayesian models of decision making tasks: reinforcement learning, delay
discounting, risky choice and others. Each of its models is one Stan program
for a group of subjects, with subject-level parameters built from group means
and scales (the "Matt trick"). The trial loops are written by hand and carry
state from one trial to the next, such as action values, that depends on the
parameters, and the updates branch on the data of each trial. The rest of the
corpus has these loops only in brms-generated and ctsem models. This directory
has four of the package's 66 models:

| Entry | Model | Task | Data | What it adds |
| --- | --- | --- | --- | --- |
| `hbayesdm_gng_m4` | `gng_m4.stan` | go/no-go task | `gng_exampleData.txt`: 10 subjects, 240 trials | six subject-level parameters, four value vectors updated in a trial loop, branches on key press and on the sign of the outcome |
| `hbayesdm_prl_rp` | `prl_rp.stan` | probabilistic reversal learning | `prl_exampleData.txt`: 20 subjects, 100 trials | Q-value updates with separate reward and punishment learning rates, `categorical_logit` choice |
| `hbayesdm_igt_pvl_delta` | `igt_pvl_delta.stan` | Iowa gambling task | `igt_exampleData.txt`: 4 subjects, 100 trials | prospect-valence utility with a power and a loss-aversion parameter, a branch on the sign of the outcome |
| `hbayesdm_dd_hyperbolic` | `dd_hyperbolic.stan` | delay discounting | `dd_exampleData.txt`: 20 subjects, 108 trials | hyperbolic discounting inside a `bernoulli_logit` loop |

The models come from the CRAN release 2.0.1:

- URL `https://cran.r-project.org/src/contrib/hBayesDM_2.0.1.tar.gz`, SHA-256
  `794ce041d576532c6b1ec1263d9333ff6020549d038aa8e79cf5d198d2a0988f`
- `inst/stan_files/<model>.stan`, SHA-256:

        c7c805de439c42438fa1b95f2eee3e120f3c0944cfe357049f4c81b056bf8bcb  gng_m4.stan
        b41c7641de03d42c961e729ec2d19ac48f3396eefec87d29921a57393d074872  prl_rp.stan
        4f7aad1c967f3b87d3885b77d5241ff9cab2207f8a8f25d4c96cc3231b75bc6a  igt_pvl_delta.stan
        efa6626ea5b026b234055e10a2a135e219806660287fff5523c0b35b4b41ca72  dd_hyperbolic.stan

- By Woo-Young Ahn, Nate Haines and Lei Zhang, with contributions from Jinwoo
  Jeong, Harhim Park, Jaeyeong Yang and Jethro Lee, license GPL-3, text in
  [`licenses/GPL-3`](licenses/GPL-3) and
  [`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md)

None of the files has an `#include`, and the checked-in `.stan` files are the
files as published in the tarball.

## Data

The data are the package's own example data, turned into Stan data by the
package's own `<task>_preprocess_func` functions in `R/preprocess_funcs.R`.
[`tools/gen_hbayesdm_data.R`](../../tools/gen_hbayesdm_data.R) reads the
unpacked tarball, sources `preprocess_funcs.R`, repeats the two steps that
`hBayesDM_model()` does before calling a preprocess function (lower-casing the
column names and building `general_info` from the trial counts), and writes the
JSON. It needs `data.table` and no compiler. Rerunning it reproduces the four
`.json` files byte for byte.

## References

The references are CmdStan 2.40.0 values at the three shared points, recorded
as for the other imported collections (see
[`tests/examplemodels/README.md`](../examplemodels/README.md)). All four models
match within the 1e-9 gate at every point, in default and fast mode; the
largest distance is 11 ULP. Each reference includes the `write_array` row,
which is the largest part of the 0.58 MB that these four entries add to the
compressed reference file (`gng_m4` 350 kB, `prl_rp` 207 kB).
