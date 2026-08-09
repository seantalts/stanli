# Finding and loading the runtime.
#
# The stanli runtime is a ~16 MB shared library. CRAN builds its own
# binaries from source and would have to compile stan-math and every
# density kernel to produce one, which is neither fast enough for their
# check farm nor possible for the embedded OCaml compiler. So the library
# is not part of the package: it is downloaded once into the user's cache
# directory, the way torch fetches libtorch.
#
# Nothing is downloaded without being asked. stanli_install() is
# explicit, and everything else fails with a message telling the user to
# run it.

runtime_filename <- function() {
  switch(Sys.info()[["sysname"]],
         Darwin = "libstanli.dylib",
         Windows = "stanli.dll",
         "libstanli.so")
}

#' Where the stanli runtime lives
#'
#' The path is taken from `STANLI_RUNTIME` when that is set (which is how
#' a development build is used), and from the user cache directory
#' otherwise.
#'
#' @return A file path, which may not exist yet.
#' @export
stanli_runtime_path <- function() {
  from_env <- Sys.getenv("STANLI_RUNTIME", "")
  if (nzchar(from_env)) return(from_env)
  file.path(tools::R_user_dir("stanli", "cache"), runtime_filename())
}

#' Is the stanli runtime available?
#' @return `TRUE` when the runtime library is present and loadable.
#' @export
stanli_available <- function() {
  file.exists(stanli_runtime_path())
}

#' Download the stanli runtime
#'
#' Fetches the prebuilt runtime for this platform into the user cache
#' directory. This is a one-time step; the library is about 16 MB.
#'
#' @param version Release tag to fetch, or `"latest"`.
#' @param quiet Passed to [utils::download.file()].
#' @param overwrite Re-download even if the runtime is already present.
#' @return The path it was installed to, invisibly.
#' @export
stanli_install <- function(version = "latest", quiet = FALSE,
                           overwrite = FALSE) {
  dest_dir <- tools::R_user_dir("stanli", "cache")
  dir.create(dest_dir, recursive = TRUE, showWarnings = FALSE)
  dest <- file.path(dest_dir, runtime_filename())
  if (file.exists(dest) && !overwrite) {
    if (!quiet) message("stanli runtime already at ", dest)
    return(invisible(dest))
  }
  asset <- sprintf("stanli-runtime-%s-%s.tar.gz",
                   tolower(Sys.info()[["sysname"]]), Sys.info()[["machine"]])
  base <- "https://github.com/seantalts/stanli/releases"
  url <- if (identical(version, "latest")) {
    file.path(base, "latest", "download", asset)
  } else {
    file.path(base, "download", version, asset)
  }
  tmp <- tempfile(fileext = ".tar.gz")
  on.exit(unlink(tmp), add = TRUE)
  ok <- tryCatch({
    utils::download.file(url, tmp, mode = "wb", quiet = quiet)
    TRUE
  }, error = function(e) {
    stop("could not download the stanli runtime from ", url, ": ",
         conditionMessage(e),
         "\nBuild it from source and point STANLI_RUNTIME at the result ",
         "if this platform has no release asset.", call. = FALSE)
  })
  utils::untar(tmp, exdir = dest_dir)
  if (!file.exists(dest))
    stop("the downloaded archive did not contain ", runtime_filename(),
         call. = FALSE)
  if (!quiet) message("stanli runtime installed to ", dest)
  load_runtime()
  invisible(dest)
}

# Bind the C ABI. Idempotent: the bridge returns immediately once loaded.
load_runtime <- function(path = stanli_runtime_path()) {
  if (.Call("stanli_bridge_loaded")) return(invisible(TRUE))
  if (!file.exists(path)) {
    stop("the stanli runtime is not installed. Run stanli_install() once, ",
         "or set STANLI_RUNTIME to a locally built library.", call. = FALSE)
  }
  err <- .Call("stanli_bridge_load", path.expand(path))
  if (nzchar(err)) stop("could not load the stanli runtime: ", err,
                        call. = FALSE)
  invisible(TRUE)
}

.onLoad <- function(libname, pkgname) {
  # Load quietly when the runtime happens to be there; never download.
  p <- stanli_runtime_path()
  if (file.exists(p)) try(load_runtime(p), silent = TRUE)
}

#' Does this build report CmdStan's `lp__` exactly?
#' @return `TRUE` unless the runtime was built with `STANLI_LITE_LP`.
#' @export
stanli_exact_lp <- function() {
  load_runtime()
  .Call("stanli_r_exact_lp")
}

#' Can this build run chains in parallel threads?
#' @return `TRUE` when the runtime was built with thread support.
#' @export
stanli_thread_safe <- function() {
  load_runtime()
  .Call("stanli_r_thread_safe")
}
