#!/usr/bin/env Rscript
# Compatibility entry point for commands saved with historical reports.
script <- sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1])
source(file.path(dirname(normalizePath(script)), "summarize_corpus_bench.R"))
