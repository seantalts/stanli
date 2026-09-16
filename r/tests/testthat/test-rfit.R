rfit_fixture <- function() {
  set.seed(2026)
  columns <- c("mu", "theta[1]", "theta[2]", "A[1,1]", "A[2,1]", "A[1,2]", "A[2,2]")
  draws <- array(rnorm(100*4*7),c(100,4,7),dimnames=list(NULL,NULL,columns))
  sampler <- array(rnorm(100*4*7),c(100,4,7),dimnames=list(NULL,NULL,
    c("lp__","accept_stat__","stepsize__","treedepth__","n_leapfrog__","divergent__","energy__")))
  structure(list(draws=draws,sampler=sampler,columns=columns,model=NULL,
    unconstrained=array(0,c(100,4,7)),warmup=20L,samples=80L,thin=1L,chains=4L,
    save_warmup=TRUE,warmup_draws=20L,seed=1L,delta=0.8,max_depth=10L,
    report=list(available=TRUE,warmup_seconds=1:4,sampling_seconds=5:8)),class="stanli_fit")
}

test_that("native extraction preserves shapes, warmup and joint draws", {
  raw <- rfit_fixture()
  fit <- as_rfit(raw)
  expect_false(inherits(fit,"stanfit"))
  expect_identical(as_rfit(fit),fit)
  expect_identical(unname(extract(fit,permuted=FALSE)[,,"mu"]),unname(raw$draws[21:100,,"mu"]))
  expect_equal(dim(extract(fit,permuted=FALSE,inc_warmup=TRUE)),c(100,4,8))
  p <- extract(fit)
  expect_equal(dim(p$mu),320)
  expect_equal(dim(p$theta),c(320,2))
  expect_equal(dim(p$A),c(320,2,2))
  expect_identical(extract(fit,inc_warmup=TRUE),p)
  expect_identical(extract(fit),p)
  expect_identical(as.numeric(extract(fit,pars="theta[2]")[[1]]),as.numeric(p$theta[,2]))
  expect_false("theta" %in% names(extract(fit,pars="theta",include=FALSE)))
  # A common permutation must select the same complete rows across variables.
  original <- cbind(as.numeric(raw$draws[21:100,,"mu"]),as.numeric(raw$draws[21:100,,"theta[1]"]))
  expect_identical(p$theta[,1],original[match(as.numeric(p$mu),original[,1]),2])
  expect_error(extract(fit,pars="missing"),"unknown variable")
  expect_error(extract(fit,permuted=NA),"permuted")
})

test_that("native metadata, summaries and serialization need no live model", {
  fit <- as_rfit(rfit_fixture())
  expect_equal(get_elapsed_time(fit),matrix(1:8,4,dimnames=list(paste0("chain:",1:4),c("warmup","sample"))))
  sp <- get_sampler_params(fit,inc_warmup=FALSE)
  expect_equal(length(sp),4)
  expect_equal(dim(sp[[1]]),c(80,6))
  expect_identical(sp[[2]][,"energy__"],fit$sampler[21:100,2,"energy__"])
  skip_if_not_installed("posterior")
  s <- summary(fit,pars="mu",probs=c(.1,.9))
  expect_equal(dim(s$summary),c(1,7))
  expect_equal(dim(s$c_summary),c(1,4,4))
  expect_equal(s$summary[1,"mean"],mean(fit$draws[21:100,,"mu"]))
  restored <- unserialize(serialize(fit,NULL))
  expect_identical(extract(restored),extract(fit))
  expect_identical(summary(restored),summary(fit))
  expect_identical(get_sampler_params(restored),get_sampler_params(fit))
})

test_that("native extraction and moment summaries agree with RStan", {
  skip_if_not_installed("rstan")
  skip_if_not_installed("posterior")
  raw <- rfit_fixture()
  set.seed(31)
  native <- as_rfit(raw)
  set.seed(31)
  oracle <- as_stanfit(raw,model=NULL)
  expect_equal(extract(native),rstan::extract(oracle),tolerance=0)
  expect_equal(extract(native,pars=c("A","mu"),permuted=FALSE,inc_warmup=TRUE),
               rstan::extract(oracle,pars=c("A","mu"),permuted=FALSE,inc_warmup=TRUE),tolerance=0)
  expect_equal(get_sampler_params(native),rstan::get_sampler_params(oracle),tolerance=0)
  expect_equal(get_elapsed_time(native),rstan::get_elapsed_time(oracle),tolerance=0)
  a <- summary(native,probs=c(.1,.9))
  b <- rstan::summary(oracle,probs=c(.1,.9))
  # ESS/MCSE deliberately use posterior's current algorithm, not RStan's
  # legacy estimator. Compare the moments, quantiles, ordering, and R-hat.
  shared <- setdiff(colnames(a$summary),c("n_eff","se_mean"))
  expect_equal(a$summary[,shared],b$summary[,shared],tolerance=1e-10)
  expect_true(all(is.finite(a$summary[,"n_eff"]) & a$summary[,"n_eff"] > 0))
  expect_equal(unname(a$c_summary),unname(b$c_summary),tolerance=1e-10)
})

test_that("native fit methods never load RStan or require a model runtime", {
  skip_if_not_installed("callr")
  skip_if_not_installed("posterior")
  result <- callr::r(function(raw) {
    stopifnot(!"rstan" %in% loadedNamespaces())
    Sys.setenv(STANLI_RUNTIME = tempfile("missing-runtime"))
    fit <- stanli::as_rfit(raw)
    before <- summary(fit)
    fit <- unserialize(serialize(fit,NULL))
    stopifnot(identical(summary(fit),before))
    stopifnot(!"rstan" %in% loadedNamespaces())
    c(length(stanli::extract(fit)), length(stanli::get_sampler_params(fit)))
  }, args=list(raw=rfit_fixture()), libpath=.libPaths())
  expect_identical(result,c(4L,4L))
})

test_that("native views retain the existing Stanli ecosystem methods", {
  skip_without_runtime()
  skip_if_not_installed("posterior")
  raw <- sample_model(log_lik_model(), chains=2, samples=40, warmup=40,
                      save_warmup=TRUE, refresh=0)
  native <- as_rfit(raw)
  expect_identical(as_draws_array(native),as_draws_array(raw))
  expect_identical(log_lik(native),log_lik(raw))
  if (requireNamespace("bayesplot",quietly=TRUE)) {
    expect_equal(bayesplot::rhat(native),bayesplot::rhat(raw))
    expect_equal(bayesplot::neff_ratio(native),bayesplot::neff_ratio(raw))
  }
})

test_that("native conversion refuses inconsistent saved-fit metadata", {
  raw <- rfit_fixture()
  raw$warmup_draws <- NULL
  expect_error(as_rfit(raw),"lacks warmup metadata")
  raw$warmup_draws <- 101
  expect_error(as_rfit(raw),"do not agree")
  raw$warmup_draws <- 20
  raw$columns <- rev(raw$columns)
  expect_error(as_rfit(raw),"do not agree")
})
