sha <- "0123456789abcdef0123456789abcdef01234567"

# GitHub pretty-prints the list, newest first.
releases_json <- '[
  {
    "url": "https://api.github.com/repos/seantalts/stanli/releases/3",
    "tag_name": "dev-bbbbbbbbbbbb",
    "prerelease": true
  },
  {
    "tag_name": "v0.30.0",
    "prerelease": false
  },
  {
    "tag_name": "dev-aaaaaaaaaaaa",
    "prerelease": true
  }
]'

# A cache under a temporary directory, and no STANLI_RUNTIME, so the user's
# own runtime is never touched and the cache is what stanli_runtime_path()
# finds. Returns the function that undoes it.
use_temp_cache <- function() {
  root <- tempfile("stanli-cache-")
  dir.create(root)
  old <- Sys.getenv(c("R_USER_CACHE_DIR", "STANLI_RUNTIME"), unset = NA)
  Sys.setenv(R_USER_CACHE_DIR = root)
  Sys.unsetenv("STANLI_RUNTIME")
  function() {
    for (name in names(old)) {
      if (is.na(old[[name]])) Sys.unsetenv(name)
      else do.call(Sys.setenv, as.list(old[name]))
    }
    unlink(root, recursive = TRUE)
  }
}

put_runtime <- function(version) {
  dir <- stanli:::runtime_cache_dir(version)
  dir.create(dir, recursive = TRUE, showWarnings = FALSE)
  path <- file.path(dir, stanli:::runtime_filename())
  writeLines("runtime", path)
  path
}

test_that("a package installed from a commit asks for that commit's runtime", {
  local_mocked_bindings(installed_commit = function() sha,
                        github_releases_json = function() stop("no network"),
                        .package = "stanli")
  expect_identical(stanli:::dev_release_tag(quiet = TRUE), "dev-0123456789ab")
})

test_that("without a recorded commit the newest dev release is used", {
  local_mocked_bindings(installed_commit = function() NA_character_,
                        github_releases_json = function() releases_json,
                        .package = "stanli")
  expect_message(tag <- stanli:::dev_release_tag(), "dev-bbbbbbbbbbbb")
  expect_identical(tag, "dev-bbbbbbbbbbbb")
})

test_that("no published dev release is an error", {
  local_mocked_bindings(installed_commit = function() NA_character_,
                        github_releases_json = function() '[{"tag_name": "v0.30.0"}]',
                        .package = "stanli")
  expect_error(stanli:::dev_release_tag(quiet = TRUE), "no development runtime")
})

test_that("a dev runtime that is not published explains why", {
  url <- "https://github.com/seantalts/stanli/releases/download/dev-0123456789ab/a.tar.gz"
  msg <- stanli:::download_failure_message(url, "dev-0123456789ab", "a.tar.gz",
    "HTTP status was '404 Not Found'", release_found = FALSE)
  expect_match(msg, "commit 0123456789ab", fixed = TRUE)
  expect_match(msg, "about an hour", fixed = TRUE)
  expect_match(msg, "10 newer", fixed = TRUE)
  expect_match(msg, "subdir = \"r\")", fixed = TRUE)
  expect_match(msg, "stanli_install()", fixed = TRUE)
  expect_no_match(msg, "pairs with", fixed = TRUE)
})

test_that("the pinned runtime is found first, then the dev one", {
  restore <- use_temp_cache()
  on.exit(restore(), add = TRUE)
  pinned <- file.path(stanli:::runtime_cache_dir(), stanli:::runtime_filename())
  expect_identical(stanli_runtime_path(), pinned)
  dev <- put_runtime("dev")
  expect_identical(stanli_runtime_path(), dev)
  put_runtime(stanli:::stanli_runtime_release)
  expect_identical(stanli_runtime_path(), pinned)
})

# An archive holding a fake runtime large enough to pass the size check.
fake_download <- function(log) {
  function(url, dest, quiet) {
    cat(url, "\n", file = log, append = TRUE)
    src <- tempfile("stanli-archive-")
    dir.create(src)
    on.exit(unlink(src, recursive = TRUE))
    writeBin(as.raw(sample.int(256, 2e6, replace = TRUE) - 1L),
             file.path(src, stanli:::runtime_filename()))
    old <- setwd(src)
    on.exit(setwd(old), add = TRUE)
    utils::tar(dest, stanli:::runtime_filename(), compression = "gzip")
    0L
  }
}

test_that("installing dev replaces the pinned runtime and records its tag", {
  restore <- use_temp_cache()
  on.exit(restore(), add = TRUE)
  log <- tempfile()
  tag <- "dev-0123456789ab"
  local_mocked_bindings(dev_release_tag = function(...) tag,
                        download_runtime = fake_download(log),
                        load_runtime = function(...) invisible(TRUE),
                        runtime_loaded = function() FALSE,
                        commit_mismatch_note = function(...) NULL,
                        .package = "stanli")
  pinned <- put_runtime(stanli:::stanli_runtime_release)
  dest <- stanli_install("dev", quiet = TRUE)
  expect_identical(dest, file.path(stanli:::runtime_cache_dir("dev"),
                                   stanli:::runtime_filename()))
  expect_false(file.exists(pinned))
  expect_identical(stanli_runtime_path(), dest)
  expect_match(readLines(log), "/releases/download/dev-0123456789ab/")

  # The same commit again is already installed.
  expect_message(stanli_install("dev"), "already at")
  expect_length(readLines(log), 1)

  # A different commit downloads again.
  tag <- "dev-bbbbbbbbbbbb"
  stanli_install("dev", quiet = TRUE)
  expect_length(readLines(log), 2)
  expect_match(readLines(log)[2], "/download/dev-bbbbbbbbbbbb/")

  # A release install then removes the dev runtime.
  stanli_install(quiet = TRUE)
  expect_false(dir.exists(stanli:::runtime_cache_dir("dev")))
  expect_identical(stanli_runtime_path(),
                   file.path(stanli:::runtime_cache_dir(),
                             stanli:::runtime_filename()))
})

test_that("installing into a session that has a runtime loaded says to restart", {
  restore <- use_temp_cache()
  on.exit(restore(), add = TRUE)
  local_mocked_bindings(dev_release_tag = function(...) "dev-0123456789ab",
                        download_runtime = fake_download(tempfile()),
                        load_runtime = function(...) invisible(TRUE),
                        runtime_loaded = function() TRUE,
                        commit_mismatch_note = function(...) NULL,
                        .package = "stanli")
  expect_message(stanli_install("dev"), "restart R")
})

test_that("\"latest\" is still refused", {
  restore <- use_temp_cache()
  on.exit(restore(), add = TRUE)
  expect_error(stanli_install("latest"), "pairs with")
})
