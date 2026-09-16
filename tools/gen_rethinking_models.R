#!/usr/bin/env Rscript
# Statistical Rethinking, second edition: every ulam call in chapters 4–16.
# rethinking 2.42, commit ac1b3b2cda83f3e14096e2d997a6e30ad109eeee.
# Formulas and preparation are evaluated from the pinned book supplement;
# no rethinking implementation code is vendored here. See PROVENANCE.md.
# Rscript tools/gen_rethinking_models.R [outdir]

args <- commandArgs(trailingOnly = TRUE)
OUT <- if (length(args)) args[1] else "tests/rethinking"
script <- sub("^--file=", "", grep("^--file=", commandArgs(), value = TRUE)[1])
source(file.path(dirname(script), "stan_data_json.R"))
suppressPackageStartupMessages({library(rethinking); library(MASS); library(ape)})
commit <- "ac1b3b2cda83f3e14096e2d997a6e30ad109eeee"
desc <- packageDescription("rethinking")
stopifnot(desc$Version == "2.42")
if (!identical(desc$RemoteSha, commit))
  stop("Install rethinking from rmcelreath/rethinking@", commit,
       " with remotes::install_github() (the source pin must be recorded).")
book <- Sys.getenv("RETHINKING_BOOK_CODE", "")
if (!nzchar(book)) {
  book <- tempfile(fileext = ".txt")
  download.file(paste0("https://raw.githubusercontent.com/rmcelreath/rethinking/",
                       commit, "/book_code_boxes.txt"), book, quiet = TRUE)
}
# The digest checks an immutable source, including when an offline file is used.
stopifnot(unname(tools::md5sum(book)) == "92595f4b5a6c445ace6f5a9d0e24342f")
lines <- readLines(book, warn = FALSE)
starts <- grep("^## R code [0-9]+\\.[0-9]+$", lines)
ids <- sub("^## R code ", "", lines[starts])
ends <- c(starts[-1] - 1L, length(lines))
boxes <- setNames(lapply(seq_along(starts), function(i) {
  if (ends[i] <= starts[i]) "" else
    paste(lines[seq.int(starts[i] + 1L, ends[i])], collapse = "\n")
}), ids)

# The whitelist contains the data preparation and model boxes, in their book
# order. Only assignments, data(), library(), and set.seed() are evaluated.
# Plotting, posterior summaries, and non-ulam fits are excluded. A few boxes
# need explicit expression indices, noted below. This is an inventory, not a
# transcription of the book's formulas or of the package's generator code.
steps <- c(
  "9.11", "9.13", "9.14", "9.16", "9.22", "9.24", "9.25", "9.26",
  "9.27", "9.28", "6.2", "9.29", "9.30",
  "11.1", "11.2", "11.10", "11.11", "11.18", "11.19", "11.24",
  "11.25", "11.28", "11.29", "11.32", "11.36", "11.37", "11.45",
  "11.49", "11.60", "11.61",
  "12.2", "12.6", "12.7", "12.9", "12.11", "12.12", "12.16", "12.24",
  "12.30", "12.31", "12.34", "12.37",
  "13.1", "13.2", "13.3", "13.7", "13.8", "13.9", "13.11", "13.13",
  "13.21", "13.23", "13.25", "13.26", "13.27", "13.28", "13.29",
  "14.1", "14.2", "14.3", "14.5", "14.6", "14.7", "14.8", "14.10",
  "14.12", "14.18", "14.19", "14.23", "14.24", "14.25", "14.26",
  "14.27", "14.30", "14.31", "14.37", "14.39", "14.46", "14.47",
  "14.48", "14.49", "14.50", "14.51", "14.52",
  "15.2", "15.3", "15.5", "15.11", "15.12", "15.13", "15.16", "15.17",
  "15.19", "15.22", "15.29", "15.30", "15.31",
  "16.1", "16.2", "16.9", "16.11"
)
RNGkind("Mersenne-Twister", "Inversion", "Rejection")
set.seed(20260915)
options(OutDec = ".", width = 80)
Sys.setenv(LANGUAGE = "en")
env <- new.env(parent = globalenv())
dir.create(OUT, showWarnings = FALSE, recursive = TRUE)
inventory <- list()
call_name <- function(x) if (is.call(x)) as.character(x[[1]])[1] else ""

for (box in steps) {
  exprs <- parse(text = boxes[[box]])
  # These contain unrelated posterior/plot preparation in the same code box.
  if (box == "14.47") exprs <- exprs[1:3] # data only, no tree plot
  if (box == "15.2") exprs <- exprs[1:3]  # data only, no error-bar loop
  for (expr in exprs) {
    kind <- call_name(expr)
    if (!kind %in% c("<-", "=", "data", "library", "set.seed")) next
    if (kind %in% c("<-", "=") && call_name(expr[[3]]) %in% c("quap", "stan")) next
    if (kind %in% c("<-", "=") && call_name(expr[[3]]) == "ulam") {
      name <- as.character(expr[[2]])
      call <- expr[[3]]
      call$sample <- FALSE
      result <- eval(call, env)
      # sample=FALSE returns a list in 2.42. A data-only ulam shell makes its
      # public stancode() method and the book's ulam(previous_fit, ...) calls
      # usable without creating a posterior or touching either Stan backend.
      fit <- methods::new("ulam", formula = result$formula, model = result$model,
                          data = result$data, formula_parsed = result$formula_parsed)
      assign(name, fit, env)
      invisible(capture.output(code <- stancode(fit)))
      stopifnot(identical(code, result$model))
      slug <- sprintf("ch%02d_%s", as.integer(strsplit(box, ".", fixed = TRUE)[[1]][1]),
                      gsub(".", "_", name, fixed = TRUE))
      if (box == "9.16") slug <- paste0(slug, "_chains4")
      writeChar(code, file.path(OUT, paste0(slug, ".stan")), eos = NULL,
                useBytes = TRUE)
      write_json(result$data, code, file.path(OUT, paste0(slug, ".json")))
      inventory[[length(inventory) + 1L]] <- data.frame(
        file = slug, model = name, code_box = box, stringsAsFactors = FALSE)
      cat(slug, " R code", box, "\n")
    } else eval(expr, env)
  }
}
inventory <- do.call(rbind, inventory)
# Audit the complete pinned supplement, independently of the whitelist.
# A missing call or a newly added one must fail regeneration, not disappear.
all_calls <- unlist(lapply(names(boxes), function(box) {
  chapter <- as.integer(strsplit(box, ".", fixed = TRUE)[[1]][1])
  if (chapter < 4L || chapter > 16L) return(character())
  hits <- regmatches(boxes[[box]], gregexpr("[A-Za-z0-9_.]+\\s*<-\\s*ulam\\s*\\(",
                                          boxes[[box]], perl = TRUE))[[1]]
  if (!length(hits)) return(character())
  paste(box, sub("\\s*<-.*$", "", hits))
}), use.names = FALSE)
stopifnot(identical(sort(all_calls), sort(paste(inventory$code_box, inventory$model))),
          !anyDuplicated(inventory$file))

# Supplemental coverage requested alongside the book inventory. The book has
# ZIP models but no ulam hurdle call. This independently authored example has
# a Bernoulli zero hurdle and a Poisson distribution truncated below at one.
# Keep it explicitly separate from the book's 61 calls.
hurdle <- ulam(alist(
  y | y == 0 ~ custom(bernoulli_lpmf(1 | p)),
  y | y > 0 ~ custom(log1m(p) + poisson_lpmf(y | lambda) - log1m_exp(-lambda)),
  p ~ beta(2, 2),
  lambda ~ exponential(1)
), data = list(y = c(0L, 0L, 1L, 1L, 2L, 3L, 5L)), sample = FALSE)
fit <- methods::new("ulam", formula = hurdle$formula, model = hurdle$model,
                    data = hurdle$data, formula_parsed = hurdle$formula_parsed)
invisible(capture.output(code <- stancode(fit)))
writeChar(code, file.path(OUT, "extra_hurdle_poisson.stan"), eos = NULL, useBytes = TRUE)
write_json(hurdle$data, code, file.path(OUT, "extra_hurdle_poisson.json"))
inventory <- rbind(inventory, data.frame(file = "extra_hurdle_poisson",
                                       model = "Supplemental hurdle Poisson",
                                       code_box = "-"))
write.table(inventory, file.path(OUT, "inventory.tsv"), sep = "\t", quote = FALSE,
            row.names = FALSE)
cat(length(all_calls), "book calls and one supplemental fixture generated.\n")
