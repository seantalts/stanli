#' Run rstan-style code on stanli
#'
#' Wraps Stan code as a `stanli_stanmodel`, an S4 subclass of rstan's
#' `stanmodel`. `rstan::sampling()` and `rstan::vb()` on it run stanli's
#' runtime instead of compiling C++, and return a `stanli_stanfit`. The
#' subclass is registered when rstan loads; stanli does not alter any rstan
#' object.
#'
#' @param x Stan code as a single string, the path of a `.stan` file, or an
#'   rstan `stanmodel`, of which only the Stan code is used. A `stanli_stanmodel`
#'   is returned as it is.
#' @param model_name Name stored in the model and in its fits. Defaults to the
#'   file name without extension, the name of `x` when it is a `stanmodel`, and
#'   `"anon_model"` for code.
#' @details The code is checked for syntax and type errors here. The model is
#'   built at each `sampling()` or `vb()` call, when the data is known.
#'
#'   `sampling()` takes rstan's arguments. `iter`, `warmup`, `thin`, `chains`,
#'   `seed`, `init`, `pars`, `include`, `cores`, `control` and `algorithm =
#'   "Fixed_param"` work as in rstan, and `chain_id`, `init_r`, `refresh` and
#'   `save_warmup` are accepted through `...`. `control` takes `adapt_delta`,
#'   `max_treedepth`, `stepsize`, `metric` and `adapt_engaged`; any other entry
#'   is an error naming it. `algorithm = "HMC"`, `sample_file` and
#'   `diagnostic_file` are errors. `init` may be `"random"`, a radius, `"0"`, a
#'   list for all chains, a list of lists with one per chain, or a function
#'   called as `f(chain_id = k)` when it has that argument and `f()` otherwise.
#'   Parameters a list leaves out start at random points within `init_r`, and
#'   entries that are not parameters are ignored. A start whose log density or
#'   gradient is not finite is redrawn up to 100 times.
#'
#'   `vb()` runs ADVI. `importance_resampling = TRUE`, `keep_every` other than
#'   1 and `sample_file` are errors. `optimizing()` is always an error:
#'   stanli's log density includes the change-of-variables Jacobian, so
#'   it cannot return the mode rstan reports. Use [optimize_model()].
#'
#'   The caller's random seed is advanced only by the default `seed` draw, as
#'   in rstan, and is restored afterwards.
#'
#'   Packages that ship precompiled models, such as rstanarm, RBesT,
#'   OncoBayes2 and prophet, do not call `sampling()` on a model you can
#'   replace, and are not supported.
#' @return A `stanli_stanmodel`.
#' @examples
#' \dontrun{
#' code <- "
#'   data { int<lower=0> N; vector[N] y; }
#'   parameters { real mu; real<lower=0> sigma; }
#'   model { y ~ normal(mu, sigma); }"
#' m <- as_stanmodel(code)
#' fit <- rstan::sampling(m, data = list(N = 3L, y = c(1, 2, 3)))
#' print(fit)
#' }
#' @export
as_stanmodel <- function(x, model_name = NULL) {
  if (!requireNamespace("rstan", quietly = TRUE))
    stop("as_stanmodel() needs rstan. Install a compatible binary with ",
         "install.packages('rstan', type = 'binary') on macOS/Windows; ",
         "use a provisioned binary on Linux.", call. = FALSE)
  if (!identical(methods::getClass("stanmodel")@package, "rstan"))
    stop("as_stanmodel() found a conflicting stanmodel class. In a fresh R ",
         "session, load rstan before stanli.", call. = FALSE)
  register_stanmodel_class()
  if (!is.null(model_name) &&
      (!is.character(model_name) || length(model_name) != 1L ||
       is.na(model_name) || !nzchar(model_name)))
    stop("model_name must be a single nonempty string", call. = FALSE)
  if (methods::is(x, "stanli_stanmodel") &&
      (is.null(model_name) || identical(model_name, x@model_name)))
    return(x)
  if (methods::is(x, "stanmodel")) {
    code <- x@model_code
    name <- x@model_name
    if (length(code) != 1L || is.na(code) || !nzchar(code))
      stop("the stanmodel has no model_code to run", call. = FALSE)
  } else if (is.character(x) && length(x) == 1L && !is.na(x)) {
    if (!grepl("\n", x, fixed = TRUE) && file.exists(x)) {
      code <- read_utf8_file(x)
      name <- tools::file_path_sans_ext(basename(x))
    } else {
      code <- x
      name <- "anon_model"
    }
  } else {
    stop("x must be a single string of Stan code, the path of a .stan file, ",
         "or an rstan stanmodel", call. = FALSE)
  }
  stanli_check_syntax(code)
  if (is.null(model_name)) model_name <- name
  methods::new("stanli_stanmodel", stanfit_stanmodel(model_name, code))
}

register_stanmodel_class <- function() {
  registry <- .stanfit_registry
  if (exists(".__C__stanli_stanmodel", envir = registry, inherits = FALSE))
    return(invisible(NULL))
  if (!identical(methods::getClass("stanmodel")@package, "rstan"))
    return(invisible(NULL))
  parent.env(registry) <- asNamespace("stanli")
  registry$.packageName <- "stanli"
  methods::setClass("stanli_stanmodel", contains = "stanmodel", where = registry)
  implementations <- list(
    sampling = function(object, ...) {
      caller <- parent.frame()
      stanmodel_sampling(object, ..., .caller = caller)
    },
    vb = function(object, ...) {
      caller <- parent.frame()
      stanmodel_vb(object, ..., .caller = caller)
    },
    optimizing = function(object, ...) stanmodel_optimizing())
  for (generic in names(implementations))
    methods::setMethod(methods::getGeneric(generic, where = asNamespace("rstan")),
                       signature = "stanli_stanmodel",
                       definition = implementations[[generic]], where = registry)
  invisible(NULL)
}

stanmodel_optimizing <- function() {
  stop("optimizing() is not available on a stanli model. stanli's log density ",
       "always includes the change-of-variables Jacobian, so the optimum is ",
       "not the mode rstan's optimizing() reports. Use optimize_model() on ",
       "stanli_model(), which returns the mode of the density stanli ",
       "evaluates.", call. = FALSE)
}

stanmodel_integer <- function(x, name, minimum) {
  if (length(x) != 1L || !is.numeric(x) || is.na(x) || !is.finite(x) ||
      x < minimum || x != floor(x) || x > .Machine$integer.max)
    stop(name, " must be a single integer of at least ", minimum, call. = FALSE)
  as.integer(x)
}

stanmodel_data <- function(data, caller) {
  if (is.null(data) || (is.list(data) && length(data) == 0L)) return(NULL)
  if (is.character(data)) {
    if (anyNA(data))
      stop("data must not contain missing names", call. = FALSE)
    found <- vapply(data, exists, logical(1), envir = caller)
    if (!all(found))
      stop("data variables not found: ", paste(data[!found], collapse = ", "),
           call. = FALSE)
    return(stats::setNames(mget(data, envir = caller, inherits = TRUE), data))
  }
  if (is.environment(data)) return(as.list(data))
  if (is.list(data)) return(data)
  stop("data must be a list, an environment or a character vector of ",
       "variable names", call. = FALSE)
}

stanmodel_build <- function(object, data, seed, caller) {
  model <- stanli_model(code = object@model_code,
                        data = stanmodel_data(data, caller), seed = seed)
  model$model_name <- object@model_name
  model
}

stanmodel_seed <- function(seed) {
  if (length(seed) != 1L || !is.numeric(seed) || is.na(seed) ||
      !is.finite(seed) || seed < 0 || seed != floor(seed) ||
      seed > .Machine$integer.max)
    stop("seed must be a single integer from 0 to ", .Machine$integer.max,
         call. = FALSE)
  as.integer(seed)
}

stanmodel_exclude <- function(model, pars, include) {
  if (!is.logical(include) || length(include) != 1L || is.na(include))
    stop("include must be TRUE or FALSE", call. = FALSE)
  names <- unique(sub("\\[.*$", "", model$columns))
  pars <- unlist(pars)
  if (is.null(pars) || (length(pars) == 1L && is.na(pars)))
    return(if (include) character() else names)
  if (!is.character(pars))
    stop("pars must be a character vector of variable names", call. = FALSE)
  unknown <- setdiff(pars, c(names, "lp__"))
  if (length(unknown))
    stop("pars not in the model: ", paste(unknown, collapse = ", "),
         call. = FALSE)
  if (include) setdiff(names, pars) else intersect(names, pars)
}

stanmodel_dots <- function(dots, allowed, what) {
  unknown <- if (length(dots))
    setdiff(if (is.null(names(dots))) rep("", length(dots)) else names(dots),
            allowed) else character()
  if (length(dots) && (is.null(names(dots)) || any(!nzchar(names(dots)))))
    unknown <- c(unknown, "(unnamed)")
  if (length(unknown))
    stop(what, " does not support the argument(s): ",
         paste(unique(unknown), collapse = ", "), call. = FALSE)
  invisible(NULL)
}

stanmodel_control <- function(control) {
  if (is.null(control)) control <- list()
  if (!is.list(control) || (length(control) &&
      (is.null(names(control)) || any(!nzchar(names(control))))))
    stop("control must be a named list", call. = FALSE)
  supported <- c("adapt_delta", "max_treedepth", "stepsize", "metric",
                 "adapt_engaged")
  unsupported <- setdiff(names(control), supported)
  if (length(unsupported))
    stop("control entries stanli does not support: ",
         paste(unsupported, collapse = ", "), call. = FALSE)
  metric <- if (is.null(control$metric)) "diag_e" else control$metric
  if (!is.character(metric) || length(metric) != 1L ||
      !metric %in% c("diag_e", "dense_e", "unit_e"))
    stop("control$metric must be \"diag_e\", \"dense_e\" or \"unit_e\"",
         call. = FALSE)
  list(delta = if (is.null(control$adapt_delta)) 0.8 else control$adapt_delta,
       max_depth = if (is.null(control$max_treedepth)) 10L else
         control$max_treedepth,
       step_size = control$stepsize, metric = metric,
       adapt_engaged = if (is.null(control$adapt_engaged)) TRUE else
         control$adapt_engaged)
}

stanmodel_sampling <- function(object, data = list(), pars = NA, chains = 4,
                               iter = 2000, warmup = floor(iter / 2),
                               thin = 1,
                               seed = sample.int(.Machine$integer.max, 1),
                               init = "random", check_data = TRUE,
                               sample_file = NULL, diagnostic_file = NULL,
                               verbose = FALSE,
                               algorithm = c("NUTS", "HMC", "Fixed_param"),
                               control = NULL, include = TRUE,
                               cores = getOption("mc.cores", 1L),
                               open_progress = FALSE, show_messages = TRUE,
                               ..., .caller = parent.frame()) {
  force(seed)
  algorithm <- match.arg(algorithm)
  if (algorithm == "HMC")
    stop("algorithm = \"HMC\" is not available; stanli runs NUTS and ",
         "Fixed_param", call. = FALSE)
  if (!is.null(sample_file))
    stop("sample_file is not supported; fit objects hold the draws",
         call. = FALSE)
  if (!is.null(diagnostic_file))
    stop("diagnostic_file is not supported", call. = FALSE)
  dots <- list(...)
  stanmodel_dots(dots, c("chain_id", "init_r", "refresh", "save_warmup"),
                 "sampling()")
  iter <- stanmodel_integer(iter, "iter", 1L)
  warmup <- stanmodel_integer(warmup, "warmup", 0L)
  thin <- stanmodel_integer(thin, "thin", 1L)
  if (warmup >= iter)
    stop("warmup must be less than iter; stanli needs at least one ",
         "post-warmup iteration", call. = FALSE)
  if (thin > iter)
    stop("thin must not exceed iter", call. = FALSE)
  if (length(chains) != 1L || !is.numeric(chains) || is.na(chains) ||
      chains != floor(chains) || chains > .Machine$integer.max)
    stop("chains must be a single integer", call. = FALSE)
  chains <- as.integer(chains)
  cores <- stanmodel_integer(cores, "cores", 1L)
  seed <- stanmodel_seed(seed)
  chain_id <- if (is.null(dots$chain_id)) 1L else dots$chain_id
  if (chains >= 1L) check_chain_id(chain_id, chains)
  chain_id <- as.integer(chain_id)
  init_r <- if (is.null(dots$init_r)) 2 else dots$init_r
  refresh <- if (is.null(dots$refresh)) max(iter %/% 10L, 1L) else dots$refresh
  save_warmup <- if (is.null(dots$save_warmup)) TRUE else dots$save_warmup
  settings <- if (algorithm == "NUTS") stanmodel_control(control)
  stanfit_flag(save_warmup, "save_warmup")

  preserve_random_seed({
    model <- stanmodel_build(object, data, seed, .caller)
    exclude <- stanmodel_exclude(model, pars, include)
    if (chains < 1L) {
      message("the number of chains is less than 1; sampling not done")
      stanfit_empty(model)
    } else {
      start <- rstan_init(model, init, chains = chains, seed = seed,
                          init_r = init_r, chain_id = chain_id)
      fit <- if (algorithm == "Fixed_param") {
        sample_model(model, chains = chains, seed = seed, warmup = 0L,
                     samples = iter - warmup, thin = thin, init = start$init,
                     init_radius = start$init_radius, refresh = refresh,
                     fixed_param = TRUE, chain_id = chain_id)
      } else {
        sample_model(model, chains = chains, seed = seed, warmup = warmup,
                     samples = iter - warmup, thin = thin,
                     delta = settings$delta, max_depth = settings$max_depth,
                     save_warmup = save_warmup, init = start$init,
                     init_radius = start$init_radius,
                     parallel_chains = min(cores, chains), refresh = refresh,
                     metric = settings$metric, step_size = settings$step_size,
                     adapt_engaged = settings$adapt_engaged,
                     chain_id = chain_id)
      }
      as_stanfit(fit, exclude = exclude)
    }
  })
}

stanmodel_vb <- function(object, data = list(), pars = NA, include = TRUE,
                         seed = sample.int(.Machine$integer.max, 1),
                         init = "random", check_data = TRUE,
                         sample_file = NULL,
                         algorithm = c("meanfield", "fullrank"),
                         importance_resampling = FALSE, keep_every = 1, ...,
                         .caller = parent.frame()) {
  force(seed)
  algorithm <- match.arg(algorithm)
  if (!is.null(sample_file))
    stop("sample_file is not supported; fit objects hold the draws",
         call. = FALSE)
  if (!identical(importance_resampling, FALSE))
    stop("importance_resampling is not supported", call. = FALSE)
  if (!identical(as.numeric(keep_every), 1))
    stop("keep_every other than 1 is not supported", call. = FALSE)
  dots <- list(...)
  stanmodel_dots(dots, c("output_samples", "iter", "grad_samples",
                         "elbo_samples", "eta", "adapt_engaged", "adapt_iter",
                         "tol_rel_obj", "eval_elbo", "refresh", "init_r"),
                 "vb()")
  option <- function(name, default)
    if (is.null(dots[[name]])) default else dots[[name]]
  iter <- option("iter", 10000)
  seed <- stanmodel_seed(seed)
  preserve_random_seed({
    model <- stanmodel_build(object, data, seed, .caller)
    exclude <- stanmodel_exclude(model, pars, include)
    start <- rstan_init(model, init, chains = 1L, seed = seed,
                        init_r = option("init_r", 2))
    fit <- variational_model(
      model, algorithm = algorithm, seed = seed, iter = iter,
      grad_samples = option("grad_samples", 1),
      elbo_samples = option("elbo_samples", 100), eta = option("eta", 1),
      adapt_engaged = option("adapt_engaged", TRUE),
      adapt_iter = option("adapt_iter", 50),
      tol_rel_obj = option("tol_rel_obj", 0.01),
      eval_elbo = option("eval_elbo", 100),
      draws = option("output_samples", 1000), init = start$init,
      init_radius = start$init_radius,
      refresh = option("refresh", max(iter %/% 10, 1)))
    as_stanfit(fit, exclude = exclude)
  })
}

rstan_init_radius <- function(value, what) {
  if (length(value) != 1L || !is.numeric(value) || is.na(value) ||
      !is.finite(value) || value < 0)
    stop(what, " must be a single nonnegative number", call. = FALSE)
  as.double(value)
}

rstan_init_start <- function(model, start, attempts, radius, seed, chain) {
  if (!is.list(start) || (length(start) && is.null(names(start))))
    stop("init must supply a named list of parameter values for each chain",
         call. = FALSE)
  reason <- NULL
  for (attempt in seq_len(attempts)) {
    draw_seed <- as.integer((seed + 1000003 * (chain - 1) + attempt - 1) %%
                              .Machine$integer.max)
    outcome <- tryCatch({
      q <- unconstrain(model, start, partial = TRUE, init_radius = radius,
                       seed = draw_seed, ignore_unknown = TRUE)
      value <- log_prob_grad(model, q)
      if (is.finite(value$lp) && all(is.finite(value$grad))) q else
        "the log density or its gradient is not finite"
    }, error = function(e) conditionMessage(e))
    if (is.numeric(outcome)) return(outcome)
    reason <- outcome
  }
  stop("No usable starting values in ", attempts, " attempt",
       if (attempts != 1L) "s", " for chain ", chain, ": ", reason,
       call. = FALSE)
}

rstan_init <- function(model, init, chains, seed = 0L, init_r = 2,
                       chain_id = 1L) {
  radius <- rstan_init_radius(init_r, "init_r")
  if (is.character(init) && length(init) == 1L && !is.na(init)) {
    if (identical(init, "random"))
      return(list(init = NULL, init_radius = radius))
    number <- suppressWarnings(as.numeric(init))
    if (is.na(number))
      stop("init must be \"random\", a nonnegative radius, a list or a ",
           "function", call. = FALSE)
    init <- number
  }
  if (is.numeric(init)) {
    return(list(init = NULL,
                init_radius = rstan_init_radius(init, "a numeric init")))
  }
  if (!is.list(init) && !is.function(init))
    stop("init must be \"random\", a nonnegative radius, a list or a ",
         "function", call. = FALSE)
  n <- as.integer(model$n_unconstrained)
  if (n == 0L) return(list(init = NULL, init_radius = radius))
  ids <- as.integer(chain_id) + seq_len(chains) - 1L
  if (is.function(init)) {
    takes_id <- "chain_id" %in% names(formals(init))
    supply <- function(k) if (takes_id) init(chain_id = ids[k]) else init()
  } else if (length(init) && is.null(names(init)) &&
             all(vapply(init, is.list, logical(1)))) {
    if (length(init) != chains)
      stop("init has ", length(init), " sets of initial values for ", chains,
           " chains", call. = FALSE)
    supply <- function(k) init[[k]]
  } else {
    supply <- function(k) init
  }
  declared <- parameter_names(model)
  starts <- matrix(0, nrow = chains, ncol = n)
  for (k in seq_len(chains)) {
    start <- supply(k)
    if (!is.list(start) || (length(start) && is.null(names(start))))
      stop("init must supply a named list of parameter values for each ",
           "chain", call. = FALSE)
    complete <- all(declared %in% names(start))
    attempts <- if (complete || radius == 0) 1L else 100L
    starts[k, ] <- rstan_init_start(model, start, attempts, radius, seed, k)
  }
  list(init = starts, init_radius = radius)
}
