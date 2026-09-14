# Extracted/adapted R data-construction code, not a complete upstream driver.
# Source lesson authors: Jonah Gabry and Aki Vehtari. Repository code license: BSD-3-Clause.
# JSON-loading lines are newly written for this corpus.
# Source: https://avehtari.github.io/BDA_R_demos/demos_rstan/ppc/poisson-ppc.html
# Upstream: source(root("demos_rstan/ppc", "count-data.R"))
# That external R script was not downloaded. data.json instead contains
# all 500 integers printed by the published lesson (section 1).
# Newly written JSON loading code:
d <- jsonlite::fromJSON("data.json")
N <- d$N
y <- d$y
