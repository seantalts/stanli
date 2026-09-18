# Run against an installed package (R CMD INSTALL --install-tests r).
# Package-only checks may skip these tests; runtime CI must never do so.
library(stanli)
stopifnot(stanli_available())
results <- testthat::test_package(
  "stanli", filter = "bayesplot|loo|posterior-ecosystem|ecosystem-registration|stanfit",
  reporter = "summary"
)
counts <- as.data.frame(results)
stopifnot(nrow(counts) > 0L, !any(counts$failed), !any(counts$error),
          !any(counts$skipped), !any(counts$warning))
