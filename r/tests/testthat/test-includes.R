include_fixture <- function() {
  root <- tempfile("stanli includes π ")
  dir.create(root)
  dir.create(file.path(root, "nested"))
  writeLines('#include <nested/center.stanfunctions>',
             file.path(root, "helpers.stanfunctions"), useBytes = TRUE)
  writeLines("real center() { return 2.0; }",
             file.path(root, "nested", "center.stanfunctions"), useBytes = TRUE)
  prior <- "theta ~ normal(shift + center(), 1);"
  writeLines(prior, file.path(root, "prior.stan"), useBytes = TRUE)
  tail <- paste0("transformed data { real shift = normal_rng(0, 1); }\n",
                 "parameters { real theta; }\nmodel {\n")
  code <- paste0('functions {\n#include "helpers.stanfunctions"\n',
                 '#include helpers.stanfunctions\n}\n', tail,
                 '#include prior.stan\n}\n')
  expanded <- paste0("functions { real center() { return 2.0; } }\n",
                     tail, prior, "\n}\n")
  file <- file.path(root, "model.stan")
  writeBin(charToRaw(enc2utf8(code)), file)
  list(root = root, file = file, code = code, expanded = expanded)
}

test_that("file and inline includes preserve model values and path precedence", {
  skip_without_runtime()
  f <- include_fixture()
  on.exit(unlink(f$root, recursive = TRUE), add = TRUE)
  expected <- log_prob_grad(stanli_model(code = f$expanded, seed = 41), 0.3)
  expect_false(identical(getwd(), f$root))
  expect_identical(log_prob_grad(stanli_model(file = f$file, seed = 41), 0.3),
                   expected)
  expect_identical(log_prob_grad(stanli_model(code = f$code, seed = 41,
                                             include_paths = f$root), 0.3), expected)
  dirs <- file.path(f$root, c("first", "second"))
  for (i in seq_along(dirs)) {
    dir.create(dirs[[i]])
    writeLines(sprintf("theta ~ normal(shift + center(), %d);", i + 1L),
               file.path(dirs[[i]], "prior.stan"))
  }
  for (paths in list(dirs, rev(dirs))) {
    scale <- if (identical(paths, dirs)) 2 else 3
    ref <- stanli_model(code = sub("center(), 1", paste0("center(), ", scale),
                                   f$expanded, fixed = TRUE), seed = 41)
    model <- stanli_model(file = f$file, include_paths = paths, seed = 41)
    expect_identical(log_prob_grad(model, 0.3), log_prob_grad(ref, 0.3))
  }
})

test_that("included models retain their source for seed rebuilds", {
  skip_without_runtime()
  f <- include_fixture()
  on.exit(unlink(f$root, recursive = TRUE), add = TRUE)
  model <- stanli_model(file = f$file)
  ref <- stanli_model(code = f$expanded)
  unlink(f$root, recursive = TRUE)
  opts <- list(seed = 7, chains = 1, warmup = 20, samples = 21,
               init_radius = 0, refresh = 0)
  got <- do.call(sample_model, c(list(model), opts))
  want <- do.call(sample_model, c(list(ref), opts))
  expect_identical(got$draws, want$draws)
  expect_identical(got$sampler, want$sampler)
  expect_identical(got$model$model_code, f$code)
})

test_that("include errors retain filenames and paths do not leak", {
  skip_without_runtime()
  f <- include_fixture()
  on.exit(unlink(f$root, recursive = TRUE), add = TRUE)
  writeLines("#include cycle.stan", file.path(f$root, "cycle.stan"))
  writeLines("parameters { real theta } model {}", file.path(f$root, "bad.stan"))
  expect_error(stanli_model(code = "#include missing.stan\n", include_paths = f$root),
                "missing.stan", fixed = TRUE)
  expect_error(stanli_model(code = "#include cycle.stan\n", include_paths = f$root),
                "recursively included", fixed = TRUE)
  expect_error(stanli_model(code = "#include bad.stan\n", include_paths = f$root),
                "bad.stan", fixed = TRUE)
  expect_error(stanli_model(code = f$code), "helpers.stanfunctions", fixed = TRUE)
  expect_s3_class(stanli_model(file = f$file), "stanli_model")
  for (paths in list(1, NA_character_, "", file.path(f$root, "absent"), f$file))
    expect_error(stanli_model(code = "model {}", include_paths = paths), "include_paths")
})

test_that("the R JavaScript compiler uses Stan's nested include semantics", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")
  f <- include_fixture()
  on.exit(unlink(f$root, recursive = TRUE), add = TRUE)
  compile <- function(code) stanli:::mir_from_js(code, include_paths = f$root)
  mir <- compile(f$code)
  expect_true(startsWith(mir, "STANLI2:"))
  # Comments and string literals must never become filesystem dependencies.
  harmless <- paste0('/* #include absent.stan */\n// #include absent.stan\n',
                      'model { print("#include absent.stan"); }')
  expect_identical(stanli:::stan_include_sources(harmless, f$root), list())
  expect_true(startsWith(compile(harmless), "STANLI2:"))
  expect_error(compile("#include absent.stan\n"), "absent.stan", fixed = TRUE)
  writeLines("#include cycle.stan", file.path(f$root, "cycle.stan"))
  expect_error(compile("#include cycle.stan\n"), "recursively included", fixed = TRUE)
  writeLines("parameters { real theta } model {}", file.path(f$root, "bad.stan"))
  expect_error(compile("#include bad.stan\n"), "bad.stan", fixed = TRUE)
  expect_error(stanli:::mir_from_js(f$code), "helpers.stanfunctions", fixed = TRUE)
  writeLines(f$expanded, file.path(f$root, "spaced π.stan"), useBytes = TRUE)
  for (directive in c('#include "spaced π.stan"\r\n',
                       '#include\n<spaced π.stan>\n',
                       '#include\t"spaced π.stan" // comment\n'))
    expect_true(startsWith(compile(directive), "STANLI2:"))
  # The bare alternative wins when it consumes more than the delimited form.
  # These filename characters are valid on Unix, but forbidden on Windows.
  if (.Platform$OS.type != "windows") {
    for (name in c('"body"tail', '<body>tail>')) {
      writeLines(f$expanded, file.path(f$root, name), useBytes = TRUE)
      expect_true(startsWith(compile(paste0("#include ", name, "\n")), "STANLI2:"))
    }
  }
  if (stanli_available())
    expect_identical(log_prob_grad(stanli_model(mir = mir, seed = 41), 0.3),
                     log_prob_grad(stanli_model(code = f$expanded, seed = 41), 0.3))
})

test_that("webR transports nested include files to the real JavaScript compiler", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")
  f <- include_fixture()
  on.exit(unlink(f$root, recursive = TRUE), add = TRUE)
  state <- stanli:::stanc_js_ctx
  old_loaded <- state$webr_loaded
  on.exit(state$webr_loaded <- old_loaded, add = TRUE)
  state$webr_loaded <- TRUE
  ctx <- V8::v8()
  ctx$source(stanli:::stanc_js_path())
  ctx$eval("globalThis.Module = {FS: {
    readFile: function(path) { return __files[path]; },
    writeFile: function(path, value) { globalThis.__written = {path: path, value: value}; }
  }};")
  eval_js <- function(script) {
    matches <- regmatches(script, gregexpr("Module\\.FS\\.readFile\\([^,]+,", script))[[1L]]
    files <- list()
    for (match in matches) {
      path <- ctx$eval(sub(",$", "", sub("^Module\\.FS\\.readFile\\(", "", match)))
      files[[path]] <- stanli:::read_utf8_file(path)
    }
    ctx$assign("__files", files)
    status <- ctx$eval(script)
    if (identical(status, "ok")) {
      written <- ctx$get("__written")
      stanli:::write_utf8_file(written$path, written$value)
    }
    status
  }
  mir <- stanli:::mir_from_webr(eval_js, f$code, include_paths = f$root)
  expect_identical(mir, stanli:::mir_from_js(f$code, include_paths = f$root))
  expect_error(stanli:::mir_from_webr(eval_js, "#include absent.stan\n",
                                     include_paths = f$root), "absent.stan", fixed = TRUE)
})

test_that("native subprocess compilers receive include paths without changing cwd", {
  f <- include_fixture()
  on.exit(unlink(f$root, recursive = TRUE), add = TRUE)
  portable <- Sys.getenv("STANLI_TEST_PORTABLE_COMPILER")
  stock <- Sys.getenv("STANLI_STANC")
  for (compiler in list(list(path = portable, portable = TRUE),
                        list(path = stock, portable = FALSE))) {
    if (!nzchar(compiler$path) || !file.exists(compiler$path)) next
    wd <- getwd()
    mir <- stanli:::mir_from_binary(compiler$path, f$code,
                                    portable = compiler$portable,
                                    include_paths = f$root)
    expect_identical(getwd(), wd)
    expect_error(stanli:::mir_from_binary(compiler$path, "#include absent.stan\n",
                                          portable = compiler$portable,
                                          include_paths = f$root),
                  "absent.stan", fixed = TRUE)
    if (stanli_available())
      expect_identical(log_prob_grad(stanli_model(mir = mir, seed = 41), 0.3),
                       log_prob_grad(stanli_model(code = f$expanded, seed = 41), 0.3))
  }
})

test_that("CmdStanR-style sampling forwards include paths", {
  skip_without_runtime()
  f <- include_fixture()
  on.exit(unlink(f$root, recursive = TRUE), add = TRUE)
  opts <- list(seed = 7, chains = 1, iter_warmup = 20, iter_sampling = 21,
               init_radius = 0, refresh = 0)
  got <- do.call(sample_cstan, c(list(f$code, include_paths = f$root), opts))
  model <- cstan_model(f$code, include_paths = f$root)
  want <- do.call(sample_cstan, c(list(f$expanded), opts))
  expect_identical(got$draws(), want$draws())
  expect_identical(do.call(model$sample, opts)$draws(), want$draws())
})
