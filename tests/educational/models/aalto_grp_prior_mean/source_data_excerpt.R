# Extracted/adapted R data-construction code, not a complete upstream driver.
# Copyright Aki Vehtari, Markus Paasiniemi. BSD-3-Clause.
# Source: https://avehtari.github.io/BDA_R_demos/demos_rstan/cmdstanr_demo.html
# The upstream dataset is external; it is NOT included in this archive.
# Run in the root of an upstream BDA_R_demos checkout (path adapted from root(...)).
data_kilpis <- read.delim("demos_rstan/kilpisjarvi-summer-temp-2022.csv", sep = ";")
data_lin <- list(N = nrow(data_kilpis), x = data_kilpis$year,
                 xpred = 2016, y = data_kilpis[,5])
data_grp <- list(N = 3*nrow(data_kilpis[,]), K = 3,
                 x = rep(1:3, nrow(data_kilpis[,])),
                 y = c(t(data_kilpis[,2:4])))
