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

test_that("a conflicting stanfit class does not prevent native APIs from loading", {
  skip_if_not_installed("rstan")
  skip_if_not_installed("callr")
  result <- callr::r(function() {
    # Reproduce a package defining its own placeholder before RStan is loaded.
    registry <- new.env(parent=globalenv())
    registry$.packageName <- "placeholder"
    methods::setClass("stanfit", slots=c(id="character"), where=registry)
    loadNamespace("stanli")
    loadNamespace("rstan")
    message <- tryCatch(stanli::as_stanfit(structure(list(),class="stanli_fit")),
                        error=conditionMessage)
    list(message=message,code=stanli::cstan_model("source")$code())
  }, libpath=.libPaths())
  expect_match(result$message,"load rstan before rethinking",fixed=TRUE)
  expect_identical(result$code,"source")
})
