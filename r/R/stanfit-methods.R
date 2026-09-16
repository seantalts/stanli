# Optional S4 definitions live in a mutable package-owned registry. Unlike
# writing into a locked namespace, this supports RStan loading after stanli.
.stanfit_registry <- new.env(parent = emptyenv())

register_stanfit_hook <- function() {
  if ("rstan" %in% loadedNamespaces()) register_stanfit_class()
  setHook(packageEvent("rstan", "onLoad"), function(...) register_stanfit_class())
}

register_stanfit_class <- function() {
  registry <- .stanfit_registry
  if (exists(".__C__stanli_stanfit", envir = registry, inherits = FALSE))
    return(invisible(NULL))
  parent.env(registry) <- asNamespace("stanli")
  registry$.packageName <- "stanli"
  methods::setClass("stanli_stanfit", contains = "stanfit", where = registry)
  implementations <- list(log_prob = stanfit_log_prob,
                           grad_log_prob = stanfit_grad_log_prob,
                           constrain_pars = stanfit_constrain_pars,
                           unconstrain_pars = stanfit_unconstrain_pars,
                           get_num_upars = stanfit_get_num_upars)
  for (generic in names(implementations))
    methods::setMethod(methods::getGeneric(generic, where = asNamespace("rstan")),
                       signature = "stanli_stanfit",
                       definition = implementations[[generic]], where = registry)
  invisible(NULL)
}

stanfit_check_model <- function(model, columns, n_unconstrained) {
  if (!is.null(model) &&
      (!inherits(model, "stanli_model") ||
       !identical(model$columns, columns) ||
       !identical(as.integer(model$n_unconstrained), as.integer(n_unconstrained))))
    stop("model must match the fit's parameter dimensions and output columns",
         call. = FALSE)
}

stanfit_live_model <- function(object) {
  model <- object@.MISC$stanli_model
  if (!inherits(model, "stanli_model") ||
      !isTRUE(.Call("stanli_r_model_alive", model$ptr)))
    stop("This fit has no live stanli model (saveRDS does not preserve native ",
         "handles). Recreate stanli_model() with the original code and data, ",
         "then use as_stanfit(saved_fit, model = model).", call. = FALSE)
  model
}

stanfit_upars <- function(model, upars) {
  if (!is.numeric(upars) || length(upars) != model$n_unconstrained ||
      any(!is.finite(upars)))
    stop("upars must contain ", model$n_unconstrained,
         " finite numeric unconstrained values", call. = FALSE)
  as.double(upars)
}

stanfit_flag <- function(value, name) {
  if (!is.logical(value) || length(value) != 1L || is.na(value))
    stop(name, " must be TRUE or FALSE", call. = FALSE)
  value
}

# RStan's public density contract is propto=true with a Jacobian by default.
# The runtime's BridgeStan adapter makes the same restriction (see
# runtime/src/bridgestan_abi.cpp). Never ignore an unsupported density flag.
stanfit_density_model <- function(object, adjust_transform) {
  if (!stanfit_flag(adjust_transform, "adjust_transform"))
    stop("adjust_transform = FALSE is unsupported: stanli's compiled graph ",
         "includes the parameter-transform Jacobian", call. = FALSE)
  model <- stanfit_live_model(object)
  if (!stanli_exact_lp())
    stop("RStan density methods need an exact-lp runtime; this runtime was ",
         "built with STANLI_LITE_LP and changes parameter-independent constants",
         call. = FALSE)
  model
}

# Independently implemented from the public method contract. RStan returns a
# gradient attribute on log_prob(..., gradient=TRUE), and a log_prob attribute
# on grad_log_prob(). Slot/method semantics: stanfit-class.R and stan_fit.hpp,
# https://github.com/stan-dev/rstan/tree/f070e44ec447b5caf1657f70d6c6e629ab43442c/rstan/rstan
stanfit_log_prob <- function(object, upars, adjust_transform = TRUE, gradient = FALSE) {
  model <- stanfit_density_model(object, adjust_transform)
  q <- stanfit_upars(model, upars)
  if (!stanfit_flag(gradient, "gradient"))
    return(.Call("stanli_r_log_prob", model$ptr, q))
  evaluated <- log_prob_grad(model, q)
  structure(evaluated$lp, gradient = evaluated$grad)
}

stanfit_grad_log_prob <- function(object, upars, adjust_transform = TRUE) {
  model <- stanfit_density_model(object, adjust_transform)
  evaluated <- log_prob_grad(model, stanfit_upars(model, upars))
  structure(evaluated$grad, log_prob = evaluated$lp)
}

stanfit_constrain_pars <- function(object, upars) {
  model <- stanfit_live_model(object)
  values <- .Call("stanli_r_write_array", model$ptr, stanfit_upars(model, upars))
  parameters <- object@model_pars[object@model_pars != "lp__"]
  dimensions <- object@par_dims[parameters]
  sizes <- vapply(dimensions, prod, numeric(1))
  if (sum(sizes) != length(values))
    stop("model output dimensions do not match this fit", call. = FALSE)
  out <- vector("list", length(parameters))
  names(out) <- parameters
  end <- cumsum(sizes)
  for (i in seq_along(parameters)) {
    block <- values[end[i] - sizes[i] + seq_len(sizes[i])]
    out[[i]] <- if (length(dimensions[[i]])) array(block, dimensions[[i]]) else block
  }
  out
}

stanfit_unconstrain_pars <- function(object, pars) {
  model <- stanfit_live_model(object)
  if (!is.list(pars) || (length(pars) &&
      (is.null(names(pars)) || anyNA(names(pars)) ||
       any(!nzchar(names(pars))) || anyDuplicated(names(pars)))))
    stop("pars must be a list with unique parameter names", call. = FALSE)
  declared <- unique(sub("\\..*$", "", .Call("stanli_r_parameter_columns", model$ptr)))
  # RStan's initialization context ignores non-parameter entries, including
  # transformed parameters and GQ returned by constrain_pars().
  values <- pars[intersect(names(pars), declared)]
  # The inverse-transform interpreter accepts flat Stan-order values and owns
  # the declared shapes. This also avoids the JSON writer's rank-two limit.
  values <- lapply(values, function(value) if (is.array(value)) as.vector(value) else value)
  unconstrain(model, values)
}

stanfit_get_num_upars <- function(object) object@.MISC$stanli_n_unconstrained
