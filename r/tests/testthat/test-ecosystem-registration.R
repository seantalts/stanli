test_that("optional generics dispatch with either namespace load order", {
  skip_if_not_installed("callr")
  for (package in c("bayesplot", "loo", "tidybayes")) {
    skip_if_not_installed(package)
    for (first in c(TRUE, FALSE)) {
      result <- callr::r(function(package, first) {
        if (first) loadNamespace(package)
        loadNamespace("stanli")
        deferred <- first || !package %in% loadedNamespaces()
        loadNamespace(package)
        generics <- switch(package,
          bayesplot = c("nuts_params", "log_posterior", "rhat", "neff_ratio"),
          loo = c("loo", "loo_compare"), tidybayes = "tidy_draws")
        c(deferred, vapply(generics, function(generic) {
          identical(getS3method(generic, "stanli_fit", envir = asNamespace(package)),
                    get(paste0(generic, ".stanli_fit"), asNamespace("stanli")))
        }, logical(1)))
      }, list(package, first), libpath = .libPaths())
      expect_true(all(result))
    }
  }
})
