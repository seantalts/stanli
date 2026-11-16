# Exercise the real Windows subprocess path in r/R/stanc.R. The compiler and
# the staged Stan source live below paths containing spaces; the compiler path
# also has a Unicode component, catching quoting and path conversion at both
# ends of system2().
main <- function() {
  args <- commandArgs(trailingOnly = TRUE)
  if (length(args) != 1L)
    stop("usage: test_windows_compiler.R STANLI-COMPILE.EXE")
  if (!identical(Sys.info()[["sysname"]], "Windows"))
    stop("test_windows_compiler.R must run on Windows")

  source("r/R/stanc.R", encoding = "UTF-8")

  unicode <- intToUtf8(c(0x03c0, 0x2603, 0x00e9, 0x1f44b))
  compiler_dir <- file.path(tempdir(), paste0("compiler path ", unicode))
  if (!dir.create(compiler_dir))
    stop("could not create the compiler test directory")
  on.exit(unlink(compiler_dir, recursive = TRUE, force = TRUE), add = TRUE)

  # mir_from_binary intentionally owns its temporary work directory. Give just
  # that function a test-local tempfile() so the real model path contains spaces
  # without relying on whether R itself accepts a spaced R_TempDir at startup.
  source_parent <- file.path(tempdir(), "Stan source path with spaces")
  if (!dir.create(source_parent))
    stop("could not create the source test directory")
  on.exit(unlink(source_parent, recursive = TRUE, force = TRUE), add = TRUE)
  compiler_env <- new.env(parent = environment(mir_from_binary))
  compiler_env$tempfile <- local({
    parent <- source_parent
    function(pattern = "file", fileext = "")
      base::tempfile(pattern, tmpdir = parent, fileext = fileext)
  })
  mir_from_binary_test <- mir_from_binary
  environment(mir_from_binary_test) <- compiler_env

  portable_compiler <- file.path(compiler_dir, "stanli-compile.exe")
  if (!file.copy(normalizePath(args[[1L]], mustWork = TRUE),
                 portable_compiler))
    stop("could not copy the Windows compiler into the test directory")

  # Deliberate CRLF input exercises byte-preserving source staging on Windows;
  # the non-ASCII string must survive the process boundary and output capture.
  model <- paste0(
    "transformed data {\r\n",
    "  print(\"", unicode, "\");\r\n",
    "}\r\n",
    "parameters { real x; }\r\n",
    "model { x ~ std_normal(); }\r\n")
  Encoding(model) <- "UTF-8"

  portable <- mir_from_binary_test(portable_compiler, model)
  if (!startsWith(portable, "STANLI4:"))
    stop("the portable compiler did not emit the versioned envelope")
  if (endsWith(portable, "\n") || endsWith(portable, "\r"))
    stop("the portable compiler emitted a final newline")
  payload <- jsonlite::base64_dec(substring(portable, 9L))
  unicode_bytes <- charToRaw(enc2utf8(unicode))
  positions <- seq_len(length(payload) - length(unicode_bytes) + 1L)
  has_unicode <- any(vapply(positions, function(i) {
    identical(payload[i:(i + length(unicode_bytes) - 1L)], unicode_bytes)
  }, logical(1)))
  if (!has_unicode)
    stop("the portable compiler did not preserve Unicode source bytes")

  message("test_windows_compiler OK")
}

main()
