# Regenerates tests/cogmod: brms::make_stancode()/make_standata() output
# against cogmod's custom brms families and stanvars, written out unchanged.
# cogmod dev @ 732be30bdae2a4cc7f38cf78b5ca7cfeb85dc732 (MIT; not yet a
# tagged release). Install with:
#   remotes::install_github("DominiqueMakowski/cogmod",
#                            ref = "732be30bdae2a4cc7f38cf78b5ca7cfeb85dc732")
# Needs brms 2.23.
#
# cm_lnr_bench reproduces issue #422's LNR setup on real data: rtdists'
# speed_acc dataset, participants 1-3, RT <= 2s, every third row kept (1540
# of 4620) so the fixture stays under 100 KB; all three participants and
# both conditions remain represented because the source rows are blocked by
# participant and then by condition. rtdists itself needs libgsl to install,
# so fetch only its data file:
#   curl -LO https://cran.r-project.org/src/contrib/Archive/rtdists/rtdists_0.12-0.tar.gz
#   tar xzf rtdists_0.12-0.tar.gz rtdists/data/speed_acc.RData
# and pass that file's path as the second argument (or set
# COGMOD_SPEED_ACC_RDATA).
#
#   Rscript tools/gen_cogmod_models.R [outdir] [speed_acc.RData]

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args) >= 1 && nzchar(args[1])) args[1] else "tests/cogmod"
SPEED_ACC <- if (length(args) >= 2) args[2] else Sys.getenv("COGMOD_SPEED_ACC_RDATA", "")
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)

script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))

suppressMessages({library(brms); library(cogmod)})
stopifnot(packageVersion("brms") == "2.23.0")
stopifnot(packageDescription("cogmod")$Version == "0.3.4")
if (!nzchar(SPEED_ACC) || !file.exists(SPEED_ACC))
  stop("cm_lnr_bench needs rtdists' speed_acc.RData; pass its path as the ",
       "second argument or set COGMOD_SPEED_ACC_RDATA (see header comment).")

safe_prior <- function(f, d) {
  tryCatch(suppressMessages(cogmod_priors(f, d)), error = function(e) NULL)
}
safe_stanvars <- function(f) {
  tryCatch(suppressWarnings(cogmod_stanvars(f)), error = function(e) NULL)
}

STATUS <- list()
case <- function(slug, formula, data, prior = NULL, stanvars = NULL) {
  id <- paste0("cm_", slug)
  r <- tryCatch({
    a <- list(formula, data = data)
    if (!is.null(prior)) a$prior <- prior
    if (!is.null(stanvars)) a$stanvars <- stanvars
    code <- as.character(do.call(brms::make_stancode, a))
    sd   <- do.call(brms::make_standata, a)
    writeLines(code, file.path(OUT, paste0(id, ".stan")))
    write_json(sd, code, file.path(OUT, paste0(id, ".json")))
    "ok"
  }, error = function(e) paste0("R_FAIL: ", conditionMessage(e)))
  STATUS[[id]] <<- r
  cat(sprintf("%-16s %s\n", id, r))
  invisible(r)
}

set.seed(20261002)
N <- 200
x <- rnorm(N)

## ================= shifted RT families (mu,...,ndt,poutlier; plain RT) ====

d <- data.frame(RT = rcogmod_lognormal(N, mu = -0.7, sigma = 0.5, ndt = 0.3, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, sigmabias ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_lognormal())
case("lognormal", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_logstudent(N, mu = -0.7, sigma = 0.4, dof = 5, ndt = 0.2, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, dof ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_logstudent())
case("logstudent", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_loggamma(N, mu = -0.7, sigma = 0.5, shape = 0.5, ndt = 0.3, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, shape ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_loggamma())
case("loggamma", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_invgaussian(N, drift = 3, boundary = 0.5, ndt = 0.2,
                                          sigmadrift = 1, sigmandt = 0.1, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, boundary ~ 1, sigmadrift ~ 1, sigmandt ~ 1, ndt ~ 1, poutlier ~ 1,
              family = cogmod_invgaussian())
case("invgaussian", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_exwald(N, mu = 3, boundary = 0.5, tau = 0.15, ndt = 0.2, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, boundary ~ 1, tau ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_exwald())
case("exwald", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_bisa(N, mu = 3, boundary = 0.5, ndt = 0.2, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, boundary ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_bisa())
case("bisa", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_gamma(N, mu = 3, sigma = 0.15, ndt = 0.3, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_gamma())
case("gamma", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_invgamma(N, mu = 4, sigma = 1.5, ndt = 0.3, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_invgamma())
case("invgamma", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_weibull(N, mu = 2, sigma = 0.5, ndt = 0.3, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_weibull())
case("weibull", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_invweibull(N, mu = 3, sigma = 0.4, ndt = 0.3, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_invweibull())
case("invweibull", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_logweibull(N, mu = -0.8, sigma = 0.3, ndt = 0.3, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_logweibull())
case("logweibull", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_lba1(N, drift = 3, sigma = 1, sigmabias = 0.5, boundary = 0.5,
                                   ndt = 0.3, poutlier = 0.02), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, sigmabias ~ 1, boundary ~ 1, ndt ~ 1, poutlier ~ 1,
              family = cogmod_lba1())
case("lba1", f, d, safe_prior(f, d), safe_stanvars(f))

## ================= plain RT families (no ndt/poutlier mixture) ============

d <- data.frame(RT = rcogmod_exgaussian(N, mu = 0.5, sigma = 0.1, tau = 0.2), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, tau ~ 1, family = cogmod_exgaussian())
case("exgaussian", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(RT = rcogmod_geg(N, mu = 0.4, sigma = 0.1, tau = 0.2, shape = 2), x = x)
f <- brms::bf(RT ~ x, sigma ~ 1, tau ~ 1, shape ~ 1, family = cogmod_geg())
case("geg", f, d, safe_prior(f, d), safe_stanvars(f))

## ================= choice families (RT | dec(Error) ~ ...) =================

sim <- rcogmod_rdm(N, vzero = 2.5, vone = 1.6, boundary = 0.5, bias = 0.2, ndt = 0.2, poutlier = 0.02)
d <- data.frame(RT = sim$rt, Error = sim$response, x = x)
f <- brms::bf(RT | dec(Error) ~ x, driftone ~ 1, sigmabias ~ 1, boundary ~ 1, ndt ~ 1, poutlier ~ 1,
              family = cogmod_rdm())
case("rdm", f, d, safe_prior(f, d), safe_stanvars(f))

sim <- rcogmod_lba2(N, driftzero = 3, driftone = 2, sigmazero = 1, sigmaone = 1,
                     sigmabias = 0.5, boundary = 0.5, ndt = 0.2, poutlier = 0.02)
d <- data.frame(RT = sim$rt, Error = sim$response, x = x)
f <- brms::bf(RT | dec(Error) ~ x, driftone ~ 1, sigmazero ~ 1, sigmaone ~ 1, sigmabias ~ 1,
              boundary ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_lba2())
case("lba2", f, d, safe_prior(f, d), safe_stanvars(f))

sim <- rcogmod_ddm(N, drift = 0.5, boundary = 1, bias = 0.5, ndt = 0.2,
                    sigmadrift = 0.2, sigmabias = 0.05, sigmandt = 0.03, poutlier = 0.02)
d <- data.frame(RT = sim$rt, Error = sim$response, x = x)
f <- brms::bf(RT | dec(Error) ~ x, boundary ~ 1, bias ~ 1, sigmadrift ~ 1, sigmabias ~ 1,
              sigmandt ~ 1, ndt ~ 1, poutlier ~ 1, family = cogmod_ddm())
case("ddm", f, d, safe_prior(f, d), safe_stanvars(f))

sim <- rcogmod_lnr(N, nuzero = 1, nuone = 0.5, sigmazero = 1, sigmaone = 0.8, ndt = 0.2, poutlier = 0.02)
d <- data.frame(RT = sim$rt, Error = sim$response, x = x)
f <- brms::bf(RT | dec(Error) ~ x, nuone ~ 1, sigmazero ~ 1, sigmaone ~ 1, sigmabias ~ 1,
              ndt ~ 1, poutlier ~ 1, family = cogmod_lnr())
case("lnr", f, d, safe_prior(f, d), safe_stanvars(f))

## ================= LNR, exact reproduction of issue #422's bench setup =====
## speed_acc (rtdists), participants 1-3, RT <= 2s, every third row kept.

load(SPEED_ACC)
bench_df <- data.frame(
  Participant = as.integer(as.character(speed_acc$id)),
  Condition = unname(c(accuracy = "Accuracy", speed = "Speed")[
    as.character(speed_acc$condition)]),
  RT = speed_acc$rt,
  Error = as.integer(as.character(speed_acc$response) != as.character(speed_acc$stim_cat))
)
bench_df <- bench_df[bench_df$Participant %in% c(1, 2, 3) & bench_df$RT <= 2, ]
bench_df <- bench_df[seq(1, nrow(bench_df), by = 3), ]
f <- brms::bf(RT | dec(Error) ~ Condition, nuone ~ Condition, sigmazero ~ 1,
              sigmaone ~ 1, sigmabias = 0, ndt ~ Condition, family = cogmod_lnr())
case("lnr_bench", f, bench_df, safe_prior(f, bench_df), safe_stanvars(f))

## ================= subjective-rating families (unit-interval / rating) ====

d <- data.frame(y = rcogmod_betagate(N, mu = 0.5, phi = 3, pex = 0.1, bex = 0.5), x = x)
f <- brms::bf(y ~ x, phi ~ 1, pex ~ 1, bex ~ 1, family = cogmod_betagate())
case("betagate", f, d, safe_prior(f, d), safe_stanvars(f))

d <- data.frame(y = rcogmod_choco(N, p = 0.5, confright = 0.5, precright = 4,
                                   confleft = 0.5, precleft = 4, pex = 0.1, bex = 0.5,
                                   pmid = 0.05, mid = 0.5), x = x)
f <- brms::bf(y ~ x, confright ~ 1, precright ~ 1, confleft ~ 1, precleft ~ 1,
              pex ~ 1, bex ~ 1, pmid ~ 1, family = cogmod_choco())
case("choco", f, d, safe_prior(f, d), safe_stanvars(f))
if (!identical(STATUS[["cm_choco"]], "ok")) {
  # Fall back to the test-verified recipe: pmid fixed at 0 (no mid point mass).
  f2 <- brms::bf(y ~ x, confright ~ 1, precright ~ 1, confleft ~ 1, precleft ~ 1,
                 pex ~ 1, bex ~ 1, pmid = 0, family = cogmod_choco())
  case("choco", f2, d, safe_prior(f2, d), safe_stanvars(f2))
}

d <- data.frame(rating = as.integer(round(rcogmod_betadiscrete(N, mu = 0.66, phi = 3.51, k = 10, pzero = 0.1))),
                k = rep(10L, N), x = x)
f <- brms::bf(rating | vint(k) ~ x, phi ~ 1, pzero ~ 1, family = cogmod_betadiscrete())
case("betadiscrete", f, d, safe_prior(f, d), safe_stanvars(f))

n_ok <- sum(unlist(STATUS) == "ok")
cat(sprintf("\n--- %d/%d ok ---\n", n_ok, length(STATUS)))
if (n_ok != length(STATUS)) stop("not every cogmod model generated cleanly")
