# Diagnostic surrogate for #657, not the reporter's missing model/data.
#
# Rscript harnesses/issue657_mixed_model.R emit OUT [OBS_PER_SUBJECT=20]
# Rscript harnesses/issue657_mixed_model.R probe OUT
#
# emit needs brms and jsonlite. probe needs current source R bindings and an
# explicitly selected STANLI_RUNTIME; it never downloads or chooses a runtime.
# ISSUE657_SAMPLE=1 adds a short, non-convergence sampling diagnostic.
# Full matched inference/ESS benchmarking belongs after the original model
# and sampler configuration have been recovered.

args <- commandArgs(trailingOnly = TRUE)
stopifnot(length(args) >= 2L, args[1L] %in% c("emit", "probe"))
mode <- args[1L]
out <- args[2L]
dir.create(out, recursive = TRUE, showWarnings = FALSE)
write_json <- function(value, path) {
  jsonlite::write_json(value, path, auto_unbox = TRUE, digits = 17,
                       pretty = TRUE, null = "null", na = "null")
}
subjects <- c(10L, 50L, 500L)
if (mode == "emit") {
  stopifnot(requireNamespace("brms"), requireNamespace("jsonlite"))
  n_per <- if (length(args) >= 3L) as.integer(args[3L]) else 20L
  stopifnot(length(n_per) == 1L, !is.na(n_per), n_per >= 2L)
  for (j in subjects) {
    set.seed(657000L + j)
    subject <- factor(rep(seq_len(j), each = n_per))
    x <- rnorm(j * n_per)
    a <- rnorm(j, sd = 0.5)
    b <- 0.1 * a + rnorm(j, sd = 0.25)
    d <- data.frame(subject, x,
                    y = 0.2 + a[subject] + (0.5 + b[subject]) * x +
                      rnorm(length(x), sd = 0.7))
    prior <- c(brms::prior(normal(0, 1), class = "b"),
               brms::prior(normal(0, 1), class = "Intercept"),
               brms::prior(student_t(3, 0, 1), class = "sd"),
               brms::prior(student_t(3, 0, 1), class = "sigma"),
               brms::prior(lkj(2), class = "cor"))
    f <- stats::as.formula("y ~ x + (1 + x | subject)")
    code <- brms::make_stancode(f, data = d, prior = prior)
    data <- brms::make_standata(f, data = d, prior = prior)
    writeLines(code, file.path(out, "proxy.stan"))
    write_json(data, file.path(out, paste0("j", j, ".json")))
  }
  write_json(list(issue = 657L, surrogate = TRUE, formula = deparse(f),
                   family = "gaussian", subjects = subjects,
                   observations_per_subject = n_per,
                   brms = as.character(utils::packageVersion("brms")),
                   r = R.version.string, seed_rule = "657000 + subjects"),
               file.path(out, "generator.json"))
  quit(status = 0L)
}

stopifnot(nzchar(Sys.getenv("STANLI_RUNTIME")), requireNamespace("stanli"),
          requireNamespace("jsonlite"))
stanli:::load_runtime()
code <- paste(readLines(file.path(out, "proxy.stan")), collapse = "\n")
# proc.time()["elapsed"] has millisecond resolution on some R builds; that
# rounds a batch of small-model gradients to zero. Sys.time has finer wall
# resolution here; retain raw batch times rather than claiming nanosecond
# kernel precision.
clock <- function() as.numeric(Sys.time())
t0 <- clock()
mir <- stanli:::embedded_mir(code, character(), FALSE, NULL)
compile_s <- clock() - t0
writeLines(mir, file.path(out, "proxy.mir"))
results <- list()
for (j in subjects) {
  data <- file.path(out, paste0("j", j, ".json"))
  t0 <- clock()
  m <- stanli::stanli_model(mir = mir, data = data,
                            allow_interpreter = FALSE)
  prep_s <- clock() - t0
  n <- m$n_unconstrained
  points <- list(rep(0, n), 0.05 * ((seq_len(n) - 1L) %% 5L - 2L))
  write_json(points, file.path(out, paste0("j", j, ".points.json")))
  t0 <- clock()
  first <- stanli::log_prob_grad(m, points[[1L]])
  first_s <- clock() - t0
  values <- lapply(points, function(q) {
    z <- stanli::log_prob_grad(m, q)
    stopifnot(is.finite(z$lp), all(is.finite(z$grad)))
    c(z$lp, z$grad)
  })
  # These are R-call-inclusive latencies, not standalone kernel timings.
  warm <- vapply(seq_len(5L), function(round) {
    t0 <- clock()
    for (i in seq_len(100L)) stanli::log_prob_grad(m, points[[1L]])
    (clock() - t0) / 100
  }, numeric(1))
  row <- list(subjects = j, parameters = n, preparation_s = prep_s,
               first_gradient_s = first_s, r_gradient_seconds = warm,
               values = values, reduce_sum_count = m$reduce_sum_count)
  if (Sys.getenv("ISSUE657_SAMPLE") == "1") {
    t0 <- clock()
    fit <- stanli::sample_model(m, chains = 1, seed = 657, warmup = 20,
                                samples = 20, max_depth = 5, init_radius = 0,
                                refresh = 0, show_messages = FALSE,
                                save_warmup = TRUE)
    row$sampling <- list(wall_s = clock() - t0, report = fit$report,
                          total_leapfrogs = sum(fit$sampler[, , "n_leapfrog__"]),
                          total_divergent = sum(fit$sampler[, , "divergent__"]),
                          finite_output = all(is.finite(fit$draws)),
                          convergence_claim = FALSE)
  }
  results[[paste0("j", j)]] <- row
  cat(sprintf("J=%d parameters=%d prep=%.3fs median R gradient=%.6fs\n",
                j, n, prep_s, stats::median(warm)))
}
write_json(list(issue = 657L, surrogate = TRUE,
                 build_id = stanli::stanli_build_id(),
                 runtime = Sys.getenv("STANLI_RUNTIME"),
                 r = R.version.string, compilation_s = compile_s,
                 results = results), file.path(out, "stanli.json"))
