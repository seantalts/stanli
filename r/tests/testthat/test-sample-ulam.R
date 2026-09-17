test_that("ulam arguments delegate with the established defaults and overrides", {
  testthat::local_mocked_bindings(sample_cstan=function(...) list(...), .package="stanli")
  defaults <- sample_ulam("code", data=list(y=1))
  expect_identical(defaults, list(model_code="code", data=list(y=1), chains=1,
    parallel_chains=1, iter_warmup=500, iter_sampling=500, save_warmup=TRUE, adapt_delta=.95))
  expect_equal(sample_ulam("code",control=list())$adapt_delta,.95)
  init <- function(chain_id) list(a=chain_id)
  args <- sample_ulam("code",chains=4,cores=2,iter=101,warmup=30,
    control=list(adapt_delta=.9,max_treedepth=8),start=init,
    seed=42,thin=3,refresh=0,save_warmup=FALSE)
  expect_equal(args[c("chains","parallel_chains","iter_warmup","iter_sampling")],
               list(chains=4,parallel_chains=2,iter_warmup=30,iter_sampling=71))
  expect_identical(args$init,init)
  expect_equal(args[c("adapt_delta","max_treedepth","seed","thin","refresh","save_warmup")],
               list(adapt_delta=.9,max_treedepth=8,seed=42,thin=3,refresh=0,save_warmup=FALSE))
  odd <- sample_ulam("code",iter=101)
  expect_equal(c(odd$iter_warmup,odd$iter_sampling),c(50,51))
  aliases <- sample_ulam("code",start=NULL,init=list(a=.2),max_treedepth=7)
  expect_equal(aliases$init,list(a=.2))
  expect_equal(aliases$max_treedepth,7)
  expect_equal(sample_ulam("code",control=list(adapt_delta=NULL))$adapt_delta,.95)
})

test_that("malformed, conflicting, and unsupported options fail before preparing a model", {
  testthat::local_mocked_bindings(stanli_model=function(...) stop("unexpected preparation"),
                                 .package="stanli")
  for (iter in list(0,-1,2.5,NA_real_,Inf,"100",c(10,20)))
    expect_error(sample_ulam("invalid",iter=iter),"iter must")
  for (warmup in list(-1,.5,NA_real_,"10"))
    expect_error(sample_ulam("invalid",warmup=warmup),"warmup must")
  expect_error(sample_ulam("invalid",iter=10,warmup=10),"iter_sampling must")
  expect_error(sample_ulam("invalid",iter=10,warmup=11),"iter_sampling must")
  for (control in list(.9, list(.9), list(metric="dense_e"),
                       list(adapt_delta=.9,adapt_delta=.8), setNames(list(.9),NA_character_)))
    expect_error(sample_ulam("invalid",control=control),"Unsupported Stanli control")
  expect_error(sample_ulam("invalid",adapt_delta=.8),"Conflicting")
  expect_error(sample_ulam("invalid",iter_sampling=20),"Conflicting")
  expect_error(sample_ulam("invalid",parallel_chains=2),"Conflicting")
  expect_error(sample_ulam("invalid",start=list(a=0),init=list(a=1)),"Conflicting")
  expect_error(sample_ulam("invalid",control=list(max_treedepth=8),max_treedepth=9),"Conflicting")
  expect_error(sample_ulam("invalid",seed=1,seed=2),"uniquely named")
  fixed <- list(model_code="invalid",data=list(),chains=1,cores=1,iter=1000,
                warmup=500,control=list(),start=list())
  expect_error(do.call(sample_ulam,c(fixed,setNames(list(1),""))),"uniquely named")
  expect_error(do.call(sample_ulam,c(list(model_code="invalid"),setNames(list(1),NA_character_))),"unsupported.*NA")
  expect_error(sample_ulam("invalid",threads_per_chain=2),"unsupported.*threads_per_chain")
  expect_error(sample_ulam("invalid",save_warmup=NULL),"save_warmup")
  expect_error(sample_ulam("invalid",cores=0),"parallel_chains")
  expect_error(sample_ulam("invalid",start=list(a=0),pathfinder_init=list()),"cannot be combined")
})

test_that("ulam translation preserves native draws, diagnostics, and initialization", {
  skip_without_runtime()
  code <- "transformed data { real shift = normal_rng(0, 1); }
           parameters { real a; real<lower=0> sigma; }
           model { a ~ normal(shift,1); sigma ~ exponential(1); }"
  init <- list(a=.2,sigma=.8)
  reference <- sample_cstan(code,chains=2,parallel_chains=2,iter_warmup=30,iter_sampling=31,
    init=init,adapt_delta=.9,max_treedepth=8,seed=11,thin=3,save_warmup=TRUE,refresh=0)
  args <- list(model_code=code,chains=2,cores=2,iter=61,warmup=30,
    control=list(adapt_delta=.9,max_treedepth=8),seed=11,thin=3,refresh=0)
  for (start in list(init,list(init,init),function(chain_id) init)) {
    fit <- do.call(sample_ulam,c(args,list(start=start)))
    expect_identical(fit$stanli_fit()$draws,reference$stanli_fit()$draws)
    expect_identical(fit$stanli_fit()$sampler,reference$stanli_fit()$sampler)
  }
  unsaved <- do.call(sample_ulam,c(args,list(start=init,save_warmup=FALSE)))
  expect_identical(unsaved$draws(),reference$draws())
  expect_equal(unsaved$stanli_fit()$warmup_draws,0)
  expect_error(do.call(sample_ulam,c(args,list(start=list(a=.2)))),"sigma|missing|parameter")
  set.seed(27)
  first <- sample_ulam(code,iter=40,refresh=0)
  set.seed(27)
  second <- sample_cstan(code,chains=1,parallel_chains=1,iter_warmup=20,iter_sampling=20,
                         adapt_delta=.95,save_warmup=TRUE,refresh=0)
  expect_identical(first$draws(),second$draws())
  expect_identical(first$sampler_diagnostics(),second$sampler_diagnostics())
})
