cstan_fixture <- function(warmup = 20L, samples = 80L, thin = 1L, save_warmup = TRUE) {
  set.seed(2026)
  columns <- c("mu", "theta[1]", "theta[2]", "A[1,1]", "A[2,1]", "A[1,2]", "A[2,2]",
                "log_lik[1]", "log_lik[2]")
  n <- ceiling(samples / thin) + if (save_warmup) ceiling(warmup / thin) else 0L
  draws <- array(rnorm(n*4*9), c(n,4,9), dimnames=list(NULL,NULL,columns))
  sampler <- array(rnorm(n*4*7), c(n,4,7), dimnames=list(NULL,NULL,
    c("lp__","accept_stat__","stepsize__","treedepth__","n_leapfrog__","divergent__","energy__")))
  sampler[, , "divergent__"] <- 0
  structure(list(draws=draws, sampler=sampler, columns=columns, model=NULL,
    warmup=warmup, samples=samples, thin=thin, chains=4L, save_warmup=save_warmup,
    warmup_draws=if(save_warmup) ceiling(warmup/thin) else 0L, seed=1L, delta=.8,
    max_depth=10L, report=list(available=TRUE,warmup_seconds=1:4,sampling_seconds=5:8)),
    class="stanli_fit")
}

cstan_csv_oracle <- function(raw) {
  files <- vapply(seq_len(raw$chains), function(chain) {
    file <- tempfile(fileext=".csv")
    header <- c("# stan_version_major = 2", "# stan_version_minor = 38", "# stan_version_patch = 0",
      "# model = fixture_model", "# method = sample", paste("# num_samples =",raw$samples),
      paste("# num_warmup =",raw$warmup), paste("# thin =",raw$thin),
      paste("# save_warmup =",as.integer(raw$save_warmup)), paste("# id =",chain),
      "# algorithm = hmc", "# engine = nuts", "# max_depth = 10", "# metric = diag_e")
    values <- cbind(raw$sampler[,chain,], raw$draws[,chain,])
    colnames(values) <- gsub("\\[|,", ".", sub("\\]$", "", colnames(values)))
    writeLines(header,file)
    cat(paste(colnames(values),collapse=","), "\n", file=file,append=TRUE,sep="")
    cat(paste(apply(values,1,function(row) paste(sprintf("%.17g",row),collapse=",")),
              collapse="\n"),"\n",file=file,append=TRUE,sep="")
    cat("# Elapsed Time: ",raw$report$warmup_seconds[chain]," seconds (Warm-up)\n",
        "#               ",raw$report$sampling_seconds[chain]," seconds (Sampling)\n",
        file=file,append=TRUE,sep="")
    file
  }, character(1))
  on.exit(unlink(files))
  oracle <- cmdstanr::as_cmdstan_fit(files,check_diagnostics=FALSE)
  # Eagerly retain all arrays before removing fixture CSVs.
  oracle$draws(inc_warmup=raw$save_warmup)
  oracle$sampler_diagnostics(inc_warmup=raw$save_warmup)
  oracle
}

test_that("CmdStan-style methods agree with CmdStanR on identical CSV draws", {
  skip_if_not_installed("posterior")
  skip_if_not_installed("cmdstanr")
  raw <- cstan_fixture(warmup=20L,samples=80L,thin=3L)
  rng <- .Random.seed
  fit <- as_cstanfit(raw)
  expect_identical(.Random.seed,rng)
  expect_identical(as_cstanfit(fit),fit)
  expect_identical(fit$stanli_fit(),raw)
  expect_false(inherits(fit,"CmdStanMCMC"))
  oracle <- cstan_csv_oracle(raw)
  # Capture the reference arrays before format conversion: CmdStanR cannot
  # bind warmup along iteration after converting its cache to draws_matrix.
  all_draws <- oracle$draws(c("A","mu"),inc_warmup=TRUE)
  post_draws <- oracle$draws()
  diagnostics <- oracle$sampler_diagnostics()
  diagnostic_names <- fit$metadata()$sampler_diagnostics
  expect_identical(diagnostic_names,sort(dimnames(diagnostics)[[3L]]))
  diagnostics <- posterior::subset_draws(diagnostics,variable=diagnostic_names)
  for (format in c("draws_array","draws_matrix","draws_df","draws_list","draws_rvars")) {
    convert <- getExportedValue("posterior",paste0("as_",format))
    expect_equal(fit$draws(format=format),convert(post_draws),tolerance=1e-12)
    expect_equal(fit$draws(c("A","mu"),inc_warmup=TRUE,format=format),
                 convert(all_draws),tolerance=1e-12)
    expect_equal(fit$sampler_diagnostics(format=format),convert(diagnostics),tolerance=1e-12)
  }
  expect_equal(fit$summary(),oracle$summary(),tolerance=1e-10)
  expect_equal(summary(fit),fit$summary())
  expect_output(print(fit),"mu")
  expect_output(print(fit,variables="theta",n=1),"theta\\[1\\]")
  expect_equal(fit$summary("mu","mean","sd",~quantile(.x,probs=c(.1,.9)),"rhat","ess_bulk"),
               oracle$summary("mu","mean","sd",~quantile(.x,probs=c(.1,.9)),"rhat","ess_bulk"),tolerance=1e-10)
  expect_equal(fit$draws("A[2,1]"),oracle$draws("A[2,1]"),tolerance=1e-12)
  expect_equal(fit$draws(c("theta","lp__","mu")),
               oracle$draws(c("theta","lp__","mu")),tolerance=1e-12)
  fields <- c("iter_warmup","iter_sampling","thin","id","variables","stan_variables","stan_variable_sizes")
  expect_equal(fit$metadata()[fields],oracle$metadata()[fields])
  expect_identical(fit$metadata()$save_warmup,as.logical(oracle$metadata()$save_warmup))
  expect_equal(fit$time()$chains[,c("warmup","sampling")],oracle$time()$chains[,c("warmup","sampling")])
  expect_true(is.na(fit$time()$total))
  expect_equal(fit$num_chains(),oracle$num_chains())
})

test_that("native CmdStan-style LOO uses the requested likelihood and efficiency", {
  skip_if_not_installed("posterior")
  skip_if_not_installed("loo")
  fit <- as_cstanfit(cstan_fixture())
  ll <- fit$draws("log_lik")
  expect_equal(suppressWarnings(fit$loo(r_eff=FALSE)),suppressWarnings(loo::loo(ll)))
  eff <- loo::relative_eff(exp(ll))
  expect_equal(suppressWarnings(fit$loo(r_eff=eff)),suppressWarnings(loo::loo(ll,r_eff=eff)))
  expect_equal(suppressWarnings(fit$loo()),suppressWarnings(loo::loo(ll,r_eff=eff)))
  expect_error(fit$loo(moment_match=TRUE),"moment matching")
  expect_error(fit$loo(variables=c("mu","theta")),"one log-likelihood")
})

test_that("CmdStan-style views preserve warmup metadata and reject invalid requests", {
  fit <- as_cstanfit(cstan_fixture(warmup=5L,samples=8L,thin=3L,save_warmup=FALSE))
  expect_equal(fit$metadata()$iter_sampling,8)
  expect_equal(fit$metadata()$thin,3)
  expect_equal(dim(fit$draws())[1],3)
  expect_error(fit$draws(inc_warmup=TRUE),"not saved")
  expect_error(fit$sampler_diagnostics(inc_warmup=TRUE),"not saved")
  expect_error(fit$draws(inc_warmup=NA),"inc_warmup")
  expect_error(fit$draws("absent"),"unknown variable")
  expect_error(fit$draws(NA_character_),"character names")
  expect_error(fit$draws(format="csv"),"unsupported draws format")
  raw <- cstan_fixture(); raw$warmup_draws <- 99
  expect_error(as_cstanfit(raw),"metadata")
  raw <- cstan_fixture(); raw$columns <- rev(raw$columns)
  expect_error(as_cstanfit(raw),"metadata")
  raw <- cstan_fixture(); raw$thin <- NULL
  expect_error(as_cstanfit(raw),"lacks sampling metadata")
  raw <- cstan_fixture(); raw$report$available <- FALSE
  expect_true(all(is.na(as_cstanfit(raw)$time()$chains$warmup)))
})

test_that("serialized CmdStan-style methods load without either Stan frontend or runtime", {
  skip_if_not_installed("callr")
  skip_if_not_installed("posterior")
  path <- tempfile(fileext=".rds"); on.exit(unlink(path))
  fit <- as_cstanfit(cstan_fixture())
  saveRDS(fit,path)
  result <- callr::r(function(path) {
    stopifnot(!any(c("stanli","rstan","cmdstanr") %in% loadedNamespaces()))
    Sys.setenv(STANLI_RUNTIME="/nonexistent")
    fit <- readRDS(path)
    value <- list(draws=fit$draws(),summary=fit$summary(),metadata=fit$metadata(),time=fit$time())
    stopifnot(!any(c("rstan","cmdstanr") %in% loadedNamespaces()))
    value
  },args=list(path=path),libpath=.libPaths())
  expect_equal(result,list(draws=fit$draws(),summary=fit$summary(),metadata=fit$metadata(),time=fit$time()))
})

test_that("sample_cstan translates initialization and sampling options", {
  skip_without_runtime()
  code <- "parameters { real a; real<lower=0> sigma; } model { a ~ normal(0,1); sigma ~ exponential(1); }"
  init <- list(a=.2,sigma=.8)
  args <- list(model_code=code,chains=2,parallel_chains=2,iter_warmup=20,iter_sampling=40,
               seed=11,thin=3,save_warmup=TRUE,adapt_delta=.9,max_treedepth=8,refresh=0)
  fit <- do.call(sample_cstan,c(args,list(init=init)))
  raw <- sample_model(stanli_model(code=code,seed=11),chains=2,parallel_chains=2,
    warmup=20,samples=40,seed=11,thin=3,save_warmup=TRUE,delta=.9,max_depth=8,
    refresh=0,init=unconstrain(stanli_model(code=code),init))
  expect_identical(fit$stanli_fit()$draws,raw$draws)
  expect_identical(fit$stanli_fit()$sampler,raw$sampler)
  with_extra <- do.call(sample_cstan,c(args,list(init=c(init,list(transformed=123)))))
  expect_identical(with_extra$draws(),fit$draws())
  by_function <- do.call(sample_cstan,c(args,list(init=function(chain_id) init)))
  by_chain <- do.call(sample_cstan,c(args,list(init=list(init,init))))
  expect_identical(by_function$draws(),fit$draws())
  expect_identical(by_chain$draws(),fit$draws())
  expect_error(do.call(sample_cstan,c(args,list(init=list(a=.2)))),"sigma|missing|parameter")
  expect_error(do.call(sample_cstan,c(args,list(init=list(init)))),"per chain")
})

test_that("sample_cstan refuses unsupported options before preparing a model", {
  expect_error(sample_cstan("invalid",threads_per_chain=0),"threads_per_chain")
  expect_error(sample_cstan("invalid",chains=0),"chains")
  expect_error(sample_cstan("invalid",iter_sampling=0),"iter_sampling")
  expect_error(sample_cstan("invalid",thin=1.5),"thin")
  expect_error(sample_cstan("invalid",adapt_delta=1),"adapt_delta")
  expect_error(sample_cstan("invalid",init=0),"constrained")
  expect_error(sample_cstan("invalid",init=list(a=0),pathfinder_init=list()),"cannot be combined")
})

test_that("zero-output models expose lp and diagnostics through native adapters", {
  skip_without_runtime()
  fit <- sample_cstan("model { target += 0; }",chains=1,iter_warmup=0,
                      iter_sampling=5,seed=10,refresh=0)
  expect_identical(posterior::variables(fit$draws()),"lp__")
  expect_equal(dim(fit$draws()),c(5,1,1))
  expect_equal(dim(fit$sampler_diagnostics()),c(5,1,6))
  rfit <- as_rfit(fit$stanli_fit())
  expect_equal(dim(extract(rfit,permuted=FALSE)),c(5,1,1))
})

test_that("stored method delegates use the installed package implementation", {
  path <- tempfile(fileext=".rds"); on.exit(unlink(path))
  saveRDS(as_cstanfit(cstan_fixture()),path)
  fit <- readRDS(path)
  testthat::local_mocked_bindings(cstan_draws=function(...) "updated implementation", .package="stanli")
  expect_identical(fit$draws(),"updated implementation")
})
