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

# The release this package was built against. NOT the package version:
# a CRAN-requested documentation fix bumps the package without cutting a
# runtime release, and a default of "latest" would silently pair a
# pinned binding with a runtime that has moved. The release workflow
# asserts this equals the tag being cut, so bumping one without the
# other fails there rather than at a user's install.
stanli_runtime_release <- "v0.8.4"

runtime_filename <- function() {
  switch(Sys.info()[["sysname"]],
         Darwin = "libstanli.dylib",
         Windows = "stanli.dll",
         "libstanli.so")
}

# The asset name is built from R's own architecture, not the kernel's.
# The library is dlopen'd into the R process, so it has to match that
# process: an x86_64 R under Rosetta on an arm64 Mac needs the x86_64
# dylib. Both names are normalized because the same machine is spelled
# three ways across platforms -- "x86-64" on Windows, "aarch64" on Linux
# arm, "arm64" on macOS arm.
runtime_os <- function() tolower(Sys.info()[["sysname"]])

runtime_arch <- function() {
  a <- tolower(R.version$arch)
  if (grepl("^(x86[-_]64|amd64)$", a)) return("x86_64")
  if (grepl("^(aarch64|arm64)$", a)) return("arm64")
  a
}

runtime_asset <- function() {
  sprintf("stanli-runtime-%s-%s.tar.gz", runtime_os(), runtime_arch())
}

#' Where the stanli runtime lives
#'
#' The path is taken from `STANLI_RUNTIME` when that is set (which is how
#' a development build is used), then from a runtime bundled into the
#' package (wasm builds bundle one at build time, since a browser cannot
#' download it), and from the user cache directory otherwise.
#'
#' @return A file path, which may not exist yet.
#' @export
stanli_runtime_path <- function() {
  from_env <- Sys.getenv("STANLI_RUNTIME", "")
  if (nzchar(from_env)) return(from_env)
  bundled <- system.file("runtime", runtime_filename(), package = "stanli")
  if (nzchar(bundled)) return(bundled)
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
#' @param version Release tag to fetch. Defaults to the release this
#'   version of the package was built against, which is the pairing its
#'   ABI check will accept; `"latest"` takes whatever the newest release
#'   is instead.
#' @param quiet Passed to [utils::download.file()].
#' @param overwrite Re-download even if the runtime is already present.
#' @return The path it was installed to, invisibly.
#' @export
stanli_install <- function(version = stanli_runtime_release, quiet = FALSE,
                           overwrite = FALSE) {
  bundled <- system.file("runtime", runtime_filename(), package = "stanli")
  if (nzchar(bundled)) {
    if (!quiet) message("this build bundles its runtime at ", bundled)
    return(invisible(bundled))
  }
  dest_dir <- tools::R_user_dir("stanli", "cache")
  dir.create(dest_dir, recursive = TRUE, showWarnings = FALSE)
  dest <- file.path(dest_dir, runtime_filename())
  if (file.exists(dest) && !overwrite) {
    if (!quiet) message("stanli runtime already at ", dest)
    return(invisible(dest))
  }
  asset <- runtime_asset()
  base <- "https://github.com/seantalts/stanli/releases"
  url <- if (identical(version, "latest")) {
    file.path(base, "latest", "download", asset)
  } else {
    file.path(base, "download", version, asset)
  }
  tmp <- tempfile(fileext = ".tar.gz")
  on.exit(unlink(tmp), add = TRUE)
  tryCatch(
    utils::download.file(url, tmp, mode = "wb", quiet = quiet),
    error = function(e) {
      stop("could not download the stanli runtime from ", url, ": ",
           conditionMessage(e),
           "\nBuild it from source and point STANLI_RUNTIME at the result ",
           "if this platform has no release asset.", call. = FALSE)
    })
  # download.file() reports some failures through a non-zero status
  # rather than a condition, and a proxy or an error page can arrive as
  # a small file with status 0. Both would reach untar() as garbage.
  if (!file.exists(tmp) || file.size(tmp) < 1e6)
    stop("the download from ", url, " is not a runtime archive (",
         if (file.exists(tmp)) file.size(tmp) else 0, " bytes). ",
         "If this platform has no release asset, build from source and ",
         "point STANLI_RUNTIME at the result.", call. = FALSE)
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
