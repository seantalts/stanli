# Model from rbioacc

[rbioacc](https://cran.r-project.org/package=rbioacc) fits toxicokinetic
models of how organisms take up and eliminate a chemical. Its `odeTK.stan`
model is in the corpus because it solves a real ODE with the older
`integrate_ode_rk45` interface (packed `theta`, `x_r` and `x_int` arrays), and
the right-hand side linearly interpolates the measured exposure concentration
with a `while` loop search on every call.

The model comes from the CRAN release 1.3.1
(`https://cran.r-project.org/src/contrib/rbioacc_1.3.1.tar.gz`):

    aaf4b97de57c497347a3f1a8388f8d69f33f38d7f3935930ff9a9c8d1d643a12  odeTK.stan
    df05d55278c9d557bb4b9b536bfc5fba8a7fadadd6b9ccbe082aaf11454eec40  include/linear_interpolation.stan
    404da553b5c33e9948bc6b9d7af7fc6fdb5c78847b3472a500d1766c29ff67f2  include/license.stan

Copyright (c) 2021 UMR5558 lbbe MEPS Aurélie Siberchicot, license MIT, text
in [`licenses/MIT`](licenses/MIT). The package's `inst/stan/include/license.stan`,
which rstantools puts at the top of the generated C++ and which is the first
block of comments in the checked-in model, is the rstantools GPL-3
boilerplate; the package's DESCRIPTION declares MIT. The file is copied
verbatim and the discrepancy is the package's.

The checked-in model is `include/license.stan`, then `odeTK.stan` with its
`#include /include/linear_interpolation.stan` line replaced by the text of
that file. A second include line in `odeTK.stan` is commented out and stays
as it is. Nothing else is changed.

| Entry | Data | Parameters | Reaches |
| --- | --- | --- | --- |
| `rbioacc_odetk` | Sialis lutaria: 15 exposure measurements, 39 internal concentrations, accumulation phase ends at day 2.17 | 3 | one `integrate_ode_rk45` solve over 39 output times, exposure interpolation inside the ODE, no metabolites (`n_met = 0`, so `Cmet` is an empty `array[39, 0, 1]` written as `[]`) |

The transformed parameter `Cexp` is declared and never assigned, so its 39
values are NaN in every `write_array` row, in CmdStan as well.

`fitTK()` on the object `modelData_ode()` returns passes it unchanged to
`rstan::sampling()` with the `odeTK` model. The data is that object, made by
the same call as the package's `tests/testthat/test-fitODE.R`, restricted to
the data block's fields.
[`tools/gen_rbioacc_data.R`](../../tools/gen_rbioacc_data.R) writes both files
from the installed package after checking the hashes above. Rerunning

    Rscript tools/gen_rbioacc_data.R tests/rbioacc

reproduces them byte for byte.

The model goes through the same oracle as the rest of the corpus: CmdStan's
recorded log density, gradient and `write_array` rows in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py). The references were
recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan `a6806ef8` and Math `5252d51d`
on Darwin arm64, and the entry carries that as its own provenance block.
