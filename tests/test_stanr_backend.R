# Run the downstream package's own Stanli backend suite against the installed
# stanr built by test_stanr.sh. Keeping the tests in stanr means this gate
# follows the real consumer contract rather than duplicating a partial facade.

args <- commandArgs(trailingOnly = TRUE)
if (length(args) != 1L || !dir.exists(args[[1L]]))
  stop("usage: test_stanr_backend.R STANR_SOURCE_DIR", call. = FALSE)

suppressPackageStartupMessages({
  library(stanr)
  library(testthat)
})

# stanr's suite runs in full under either backend and reads the choice from
# STANR_BACKEND (tests/testthat/helpers.R, "Backend selection"). The default
# filter keeps the contract file, which names its backend itself; a wider
# STANR_TEST_FILTER runs other files under the stanli backend too.
# TESTTHAT_PARALLEL is honored by testthat and, with the installed package
# named here, runs each file in its own worker process, which is where a
# teardown crash surfaces as "R session crashed" rather than as a failure.
Sys.setenv(STANR_BACKEND = "stanli")
testthat::test_dir(
  file.path(args[[1L]], "tests", "testthat"),
  filter = Sys.getenv("STANR_TEST_FILTER", "stanli-backend"),
  package = "stanr",
  load_package = "installed",
  reporter = "summary",
  stop_on_failure = TRUE
)

message("stanr's Stanli backend suite passed")
