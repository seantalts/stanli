test_that("optional S4 methods register with either namespace load order", {
  skip_if_not_installed("rstan")
  skip_if_not_installed("callr")
  for (first in c(TRUE, FALSE)) {
    result <- callr::r(function(first) {
      if (first) loadNamespace("rstan")
      loadNamespace("stanli")
      deferred <- first || !"rstan" %in% loadedNamespaces()
      loadNamespace("rstan")
      generics <- c("log_prob", "grad_log_prob", "constrain_pars", "unconstrain_pars",
                    "get_num_upars")
      c(deferred, vapply(generics, function(name) {
        generic <- methods::getGeneric(name, where = asNamespace("rstan"))
        native <- methods::selectMethod(generic, "stanli_stanfit")
        original <- methods::selectMethod(generic, "stanfit")
        identical(unname(as.character(native@defined)), "stanli_stanfit") &&
          identical(unname(as.character(original@defined)), "stanfit")
      }, logical(1)))
    }, list(first), libpath = .libPaths())
    expect_true(all(result))
  }
})
