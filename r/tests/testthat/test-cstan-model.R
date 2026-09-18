test_that("the model delegates sampling options without introducing new defaults", {
  testthat::local_mocked_bindings(sample_cstan=function(...) list(...), .package="stanli")
  code <- "parameters { real a; } model { a ~ normal(0,1); }"
  model <- cstan_model(code)
  expect_s3_class(model,"stanli_cstanmodel")
  expect_identical(model$code(),code)
  expect_identical(model$model_name(),"stanli_model")
  expect_output(print(model),"stanli_model")
  expect_identical(model$sample(),list(model_code=code))
  args <- list(data=list(y=1),chains=4,parallel_chains=2,iter_warmup=30,
    iter_sampling=31,init=list(a=.2),adapt_delta=.9,max_treedepth=8,
    seed=42,thin=3,refresh=0,save_warmup=TRUE)
  expect_identical(do.call(model$sample,c(args,list(threads_per_chain=1))),
                   do.call(sample_cstan,c(list(model_code=code),args,list(threads_per_chain=1))))
  path <- tempfile()
  on.exit(unlink(path), add=TRUE)
  saveRDS(model,path)
  expect_identical(readRDS(path)$code(),code)
  expect_identical(readRDS(path)$sample(),model$sample())
})

test_that("invalid model and sampler options fail before preparation", {
  testthat::local_mocked_bindings(stanli_model=function(...) stop("unexpected preparation"),
                                 .package="stanli")
  for (code in list(NULL,NA_character_,character(),c("a","b"),1,"", "  "))
    expect_error(cstan_model(code),"model_code must")
  model <- cstan_model("invalid")
  for (threads in list(0,NA_real_,1.5,"1"))
    expect_error(model$sample(threads_per_chain=threads),"threads_per_chain")
  expect_error(model$sample(threads_per_chain=2),"unexpected preparation")
  expect_error(model$sample(cpp_options=list()),"unsupported.*cpp_options")
  expect_error(model$sample(iter_sampling=0),"iter_sampling must")
  expect_error(model$sample(save_warmup=NULL),"save_warmup")
  expect_error(model$sample(init=0),"complete constrained")
})

test_that("model sampling exactly preserves draws, diagnostics, RNG and starts", {
  skip_without_runtime()
  code <- "transformed data { real shift = normal_rng(0,1); }
           parameters { real a; real<lower=0> sigma; }
           model { a ~ normal(shift,1); sigma ~ exponential(1); }"
  model <- cstan_model(code)
  init <- list(a=.2,sigma=.8)
  args <- list(chains=2,parallel_chains=2,iter_warmup=30,iter_sampling=31,
    adapt_delta=.9,max_treedepth=8,seed=11,thin=3,save_warmup=TRUE,refresh=0)
  reference <- do.call(sample_cstan,c(list(model_code=code,init=init),args))
  for (start in list(init,list(init,init),function(chain_id) init)) {
    fit <- do.call(model$sample,c(args,list(init=start)))
    expect_identical(fit$stanli_fit()$draws,reference$stanli_fit()$draws)
    expect_identical(fit$stanli_fit()$sampler,reference$stanli_fit()$sampler)
    expect_identical(fit$metadata()$model_name,model$model_name())
  }
  args$save_warmup <- FALSE
  unsaved <- do.call(model$sample,c(args,list(init=init)))
  expect_identical(unsaved$draws(),reference$draws())
  expect_equal(unsaved$stanli_fit()$warmup_draws,0)
  set.seed(27)
  first <- model$sample(chains=1,iter_warmup=20,iter_sampling=20,refresh=0)
  set.seed(27)
  second <- sample_cstan(code,chains=1,iter_warmup=20,iter_sampling=20,refresh=0)
  expect_identical(first$draws(),second$draws())
  expect_identical(first$sampler_diagnostics(),second$sampler_diagnostics())
})
