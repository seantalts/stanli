# Finding and loading the runtime.
#
# The stanli runtime is a shared library containing stan-math and every density
# kernel. It is released separately so installing the small R bridge never
# rebuilds that C++ stack. The library is downloaded once into the user's cache
# directory, the way torch fetches libtorch.
#
# Nothing is downloaded without being asked. stanli_install() is
# explicit, and everything else fails with a message telling the user to
# run it.

# The release this package was built against. NOT the package version:
# a package-only fix can bump the package without cutting a runtime release,
# and a default of "latest" would silently pair a pinned binding with a runtime
# that has moved. The release workflow asserts this equals the tag being cut,
# so bumping one without the other fails there rather than at a user's install.
stanli_runtime_release <- "v0.30.1"

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

runtime_asset <- function(os = runtime_os(), arch = runtime_arch()) {
  sprintf("stanli-runtime-%s-%s.tar.gz", os, arch)
}

# The cache is keyed by release so a package upgrade cannot keep loading
# the runtime a previous version downloaded (#220): the new pin names a
# directory that does not exist yet, stanli_available() turns FALSE, and
# every entry point already answers that with "run stanli_install()".
runtime_cache_dir <- function(version = stanli_runtime_release) {
  file.path(tools::R_user_dir("stanli", "cache"), version)
}

#' Where the stanli runtime lives
#'
#' The path is taken from `STANLI_RUNTIME` when that is set (which is how
#' a local build is used), then from a runtime bundled into the
#' package (wasm builds bundle one at build time, since a browser cannot
#' download it), and from the user cache directory otherwise. The cache
#' path includes the release the package pins, so a runtime downloaded by
#' an older version of the package is never picked up by a newer one.
#' When that release's runtime is not in the cache but one installed by
#' `stanli_install("dev")` is, the dev runtime is used.
#'
#' @return A file path, which may not exist yet.
#' @export
stanli_runtime_path <- function() {
  from_env <- Sys.getenv("STANLI_RUNTIME", "")
  if (nzchar(from_env)) return(from_env)
  bundled <- system.file("runtime", runtime_filename(), package = "stanli")
  if (nzchar(bundled)) return(bundled)
  pinned <- file.path(runtime_cache_dir(), runtime_filename())
  dev <- file.path(runtime_cache_dir("dev"), runtime_filename())
  if (!file.exists(pinned) && file.exists(dev)) dev else pinned
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
#'   ABI check will accept. Only that release's runtime is found by
#'   [stanli_runtime_path()]; to run a different build, set
#'   `STANLI_RUNTIME`. `"dev"` fetches the runtime built from the commit
#'   that the package was installed from, for a package installed from
#'   the main branch on GitHub. A package without a recorded commit gets
#'   the newest dev runtime. The runtime for a commit appears about an
#'   hour after the commit lands on main, and only the newest ten are
#'   kept. Installing the dev runtime removes the release runtime from
#'   the cache, and installing a release removes the dev runtime, so the
#'   one installed last is used.
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
  if (identical(version, "latest"))
    stop("this package pairs with the ", stanli_runtime_release,
         " runtime and looks for no other; pass a release tag to fetch ",
         "one anyway, and point STANLI_RUNTIME at it to use it.",
         call. = FALSE)
  dev <- identical(version, "dev")
  # Every dev runtime shares one directory, and the tag file in it says
  # which commit's runtime is there.
  tag <- if (dev) dev_release_tag(quiet = quiet) else version
  dest_dir <- runtime_cache_dir(version)
  dir.create(dest_dir, recursive = TRUE, showWarnings = FALSE)
  dest <- file.path(dest_dir, runtime_filename())
  tag_file <- file.path(dest_dir, "tag")
  pinned <- file.path(runtime_cache_dir(), runtime_filename())
  if (file.exists(dest) && !overwrite &&
      (!dev || (identical(installed_dev_tag(), tag) && !file.exists(pinned)))) {
    if (!quiet) message("stanli runtime already at ", dest)
    return(invisible(dest))
  }
  # Windows cannot delete or replace a DLL that this session has loaded,
  # and loading stanli loads the cached runtime, so clearing the cache has
  # to happen before stanli is loaded.
  if (dev && on_windows() && runtime_loaded() &&
      !nzchar(Sys.getenv("STANLI_RUNTIME")) &&
      (file.exists(pinned) || file.exists(dest)))
    stop("this R session has loaded a stanli runtime, which Windows does ",
         "not let stanli_install() replace. Restart R and, before loading ",
         "stanli, run\n  unlink(tools::R_user_dir(\"stanli\", \"cache\"), ",
         "recursive = TRUE)\n  stanli::stanli_install(\"dev\")", call. = FALSE)
  asset <- runtime_asset()
  base <- "https://github.com/seantalts/stanli/releases"
  url <- file.path(base, "download", tag, asset)
  tmp <- tempfile(fileext = ".tar.gz")
  on.exit(unlink(tmp), add = TRUE)
  tryCatch(
    download_runtime(url, tmp, quiet),
    error = function(e) {
      stop(download_failure_message(url, tag, asset, conditionMessage(e),
                                    release_published(base, tag)),
           call. = FALSE)
    })
  # download.file() reports some failures through a non-zero status
  # rather than a condition, and a proxy or an error page can arrive as
  # a small file with status 0. Both would reach untar() as garbage.
  if (!file.exists(tmp) || file.size(tmp) < 1e6)
    stop("the download from ", url, " is not a runtime archive (",
         if (file.exists(tmp)) file.size(tmp) else 0, " bytes). ",
         "If this platform has no release asset, build from source and ",
         "point STANLI_RUNTIME at the result.", call. = FALSE)
  # The dev directory holds an older commit's files, which a new archive
  # may not overwrite.
  if (dev) {
    unlink(dest_dir, recursive = TRUE)
    dir.create(dest_dir, recursive = TRUE, showWarnings = FALSE)
  }
  utils::untar(tmp, exdir = dest_dir)
  if (!file.exists(dest))
    stop("the downloaded archive did not contain ", runtime_filename(),
         call. = FALSE)
  if (dev) writeLines(tag, tag_file)
  # Runtimes for other releases, and the unversioned file that packages
  # before 0.9.3 wrote, are ~16 MB each of cache nothing looks for now.
  # The pinned release's runtime goes too once dev is installed, since
  # stanli_runtime_path() would prefer it.
  root <- tools::R_user_dir("stanli", "cache")
  unlink(file.path(root, runtime_filename()))
  stale <- setdiff(list.dirs(root, recursive = FALSE),
                   c(dest_dir, if (!dev) runtime_cache_dir()))
  unlink(stale, recursive = TRUE)
  if (!quiet) message("stanli runtime installed to ", dest)
  if (identical(dest, stanli_runtime_path())) {
    # A runtime that is already loaded stays loaded until R restarts.
    if (runtime_loaded()) {
      if (!quiet) message("a stanli runtime is already loaded in this ",
                          "session; restart R to use the one just installed")
    } else {
      load_runtime()
      note <- commit_mismatch_note()
      if (!quiet && !is.null(note)) message(note)
    }
  } else if (!quiet) {
    message("this package loads the ", stanli_runtime_release,
            " runtime; set STANLI_RUNTIME to use the one just installed")
  }
  invisible(dest)
}

download_runtime <- function(url, dest, quiet) {
  utils::download.file(url, dest, mode = "wb", quiet = quiet)
}

runtime_loaded <- function() .Call("stanli_bridge_loaded")

on_windows <- function() .Platform$OS.type == "windows"

installed_dev_tag <- function() {
  path <- file.path(runtime_cache_dir("dev"), "tag")
  if (file.exists(path)) readLines(path, n = 1, warn = FALSE) else NA_character_
}

github_releases_json <- function() {
  api <- "https://api.github.com/repos/seantalts/stanli/releases?per_page=100"
  paste(readLines(api, warn = FALSE), collapse = "\n")
}

# The dev release for the commit the package was installed from. Without a
# recorded commit, the newest one: GitHub lists releases newest first, and
# lists drafts only to the repository's owners.
dev_release_tag <- function(sha = installed_commit(), quiet = FALSE) {
  if (!is.na(sha)) return(paste0("dev-", substr(sha, 1, 12)))
  json <- tryCatch(github_releases_json(), error = function(e)
    stop("could not list stanli's releases on GitHub to find a development ",
         "runtime: ", conditionMessage(e), call. = FALSE))
  found <- regmatches(json, regexec('"tag_name"\\s*:\\s*"(dev-[0-9a-f]{12})"',
                                    json))[[1]]
  if (length(found) != 2)
    stop("GitHub lists no development runtime for stanli. Install a release ",
         "with stanli_install() instead.", call. = FALSE)
  if (!quiet)
    message("this package does not record the commit it was installed from; ",
            "using the newest development runtime, ", found[2])
  found[2]
}

# TRUE when the release page exists, FALSE when GitHub answers 404 for it,
# NA when that cannot be told (offline, a proxy, an old R).
release_published <- function(base, version) {
  status <- tryCatch(
    attr(curlGetHeaders(paste0(base, "/tag/", version)), "status"),
    error = function(e) NA_integer_)
  if (identical(status, 200L)) TRUE else if (identical(status, 404L)) FALSE else NA
}

download_failure_message <- function(url, version, asset, reason,
                                     release_found,
                                     paired = identical(version,
                                                        stanli_runtime_release)) {
  if (isFALSE(release_found) && startsWith(version, "dev-"))
    return(paste0(
      "the stanli runtime for commit ", substring(version, 5), " is not ",
      "published. A runtime appears about an hour after a commit lands on ",
      "main, and only commits on main get one. When several commits land ",
      "close together, some of them get no runtime. A runtime is removed ",
      "once 10 newer commits on main have one. Reinstall the package from ",
      "main with remotes::install_github(\"seantalts/stanli\", subdir = \"r\") ",
      "to get a newer commit, or install a release with ",
      "remotes::install_github(\"seantalts/stanli@*release\", subdir = \"r\") ",
      "and run stanli_install()."))
  if (isFALSE(release_found))
    return(paste0(
      "stanli release ", version, " is not published on GitHub, so its ",
      "runtime cannot be downloaded. ",
      if (paired) paste0(
        "This R package pairs with it, which happens when the package was ",
        "installed from a commit while that release was being built. "),
      "Install the latest published release with ",
      "remotes::install_github(\"seantalts/stanli@*release\", subdir = \"r\") ",
      "and run stanli_install() again, or wait until ", version,
      " is published."))
  if (isTRUE(release_found))
    return(paste0(
      "stanli release ", version, " has no ", asset, " for this platform. ",
      "Build the runtime from source and point STANLI_RUNTIME at the result."))
  paste0("could not download the stanli runtime from ", url, ": ", reason,
         "\nCheck the network connection and try again. If this platform ",
         "has no release asset, build the runtime from source and point ",
         "STANLI_RUNTIME at the result.")
}

# remotes and pak record the commit that install_github() installed.
installed_commit <- function() {
  sha <- utils::packageDescription("stanli", fields = "RemoteSha")
  if (is.character(sha) && grepl("^[0-9a-f]{40}$", sha)) sha else NA_character_
}

runtime_commit <- function(build_id = tryCatch(stanli_build_id(),
                                               error = function(e) "")) {
  m <- regmatches(build_id, regexec("^abi[0-9]+-([0-9a-f]{12})-", build_id))[[1]]
  if (length(m) == 2) m[2] else NA_character_
}

# The runtime compiles the model as well as running it. A package installed
# from a commit after a release still gets that release's runtime, which
# lacks anything merged since.
commit_mismatch_note <- function(package = installed_commit(),
                                 runtime = runtime_commit()) {
  if (is.na(package) || is.na(runtime) || startsWith(package, runtime))
    return(NULL)
  paste0("This R package was installed from commit ", substr(package, 1, 12),
         ", but the runtime, which compiles and runs the model, was built ",
         "from commit ", runtime, ". Changes merged after that runtime are ",
         "not in it. To get a package and runtime from the same release, run ",
         "remotes::install_github(\"seantalts/stanli@*release\", ",
         "subdir = \"r\") and then stanli_install(overwrite = TRUE).")
}

with_commit_note <- function(expr, note = commit_mismatch_note()) {
  if (is.null(note)) return(expr)
  tryCatch(expr, error = function(e)
    stop(conditionMessage(e), "\n", note, call. = FALSE))
}

native_state <- new.env(parent = emptyenv())

# The .Call routines named anywhere in this package's R code.
native_routines_used <- function() {
  ns <- asNamespace("stanli")
  names <- lapply(ls(ns, all.names = TRUE), function(name) {
    f <- get(name, envir = ns)
    if (!is.function(f)) return(NULL)
    text <- paste(deparse(f), collapse = "\n")
    found <- regmatches(text, gregexpr('\\.Call\\("[A-Za-z0-9_]+"', text))[[1]]
    sub('^\\.Call\\("', "", sub('"$', "", found))
  })
  sort(unique(unlist(names)))
}

native_routines_registered <- function() {
  names(getDLLRegisteredRoutines(getLoadedDLLs()[["stanli"]])$.Call)
}

missing_native_routines <- function(registered = native_routines_registered()) {
  setdiff(native_routines_used(), registered)
}

# Updating stanli while it is loaded can leave R code from the new install
# calling compiled code from the old one, which fails with "not resolved
# from current namespace". Say what happened instead.
check_native_routines <- function(registered = native_routines_registered()) {
  missing <- missing_native_routines(registered)
  if (length(missing))
    stop("stanli's R code and its compiled code come from different ",
         "installs (the compiled code lacks ", paste(missing, collapse = ", "),
         "). This happens when stanli is updated while it is loaded. ",
         "Please restart R; if the error persists, reinstall stanli.",
         call. = FALSE)
  invisible(TRUE)
}

# Bind the C ABI. Idempotent: the bridge returns immediately once loaded.
load_runtime <- function(path = stanli_runtime_path()) {
  if (!isTRUE(native_state$checked)) {
    check_native_routines()
    native_state$checked <- TRUE
  }
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
  register_ecosystem_methods()
  register_stanfit_hook()
  # Load quietly when the runtime happens to be there; never download.
  p <- stanli_runtime_path()
  if (file.exists(p)) try(load_runtime(p), silent = TRUE)
}

#' Does this build report CmdStan's `lp__` exactly?
#' @return Always `TRUE`: every build reproduces CmdStan's `lp__` exactly.
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
