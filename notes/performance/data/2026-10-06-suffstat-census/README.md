Outputs and scripts for
[the sufficient-statistic collapse design](../../2026-10-06-sufficient-statistic-collapse.md).
Census of the shared corpus on `origin/fastmath/mode` `73a344cd`, i9-13900K,
clang 18.1.3, 2026-10-06. The machine was shared with another benchmark;
timings are shares, not wall-clock claims.

Results:

- `summary-grouping.txt`: first census. Models with a likelihood term that
  collapses to per-group statistics or distinct rows, near-miss buckets, and
  the per-model table with estimated ceilings.
- `summary-linear-gaussian-and-families.txt`: second census. The affine-mean
  normal form, the other exponential families, and the ranked family list.
- `numerics.txt`, `numerics_stress.json`: grouped normal against a 70-digit
  mpmath reference; the shifted-data stress.
- `lg_numerics.txt`, `lg_cond.txt`: linear-Gaussian forms against the
  reference, and design conditioning per model.
- `handcollapse.txt`, `handcheck.txt`, `handcollapse_lg.txt`: three models
  collapsed by hand in Stan and timed with `bench_grad`, with gradient
  agreement.
- `datarows.txt`: repeated raw data rows in models whose likelihood the graph
  census could not see.
- `dry-run.txt`: output of `harnesses/collapse_census.py`, the analysis half
  of the pass run over the corpus in the shipped pipeline (the design note's
  step 0).

Scripts (throwaway; they expect a `scratch/suffstat/` directory in a checkout
with a Release build in `build-spike/`, the pinned
`deps/stanc3/stanli-vectorize-probe`, and a virtualenv with numpy and mpmath):

- `graph_dump.cpp`: dumps a model's bound log-prob graph and data values.
- `stage1.py`, `stage2.py`, `stage2b.py`: MIR and graph dumps, per-opcode
  profiles, default and fast-mode timings.
- `analyze.py`, `summarize.py`: element value numbering and the first census.
- `analyze2.py`, `summarize2.py`, `lg_scan.py`: affine forms and the second
  census.
- `numerics.py`, `lg_numerics.py`: the reference comparisons.
- `handcollapse.py`, `handcollapse_lg.py`, `handcheck.py`, `datarows.py`.

The per-model graph dumps (about 300 MB) and the per-model JSON are not kept
here.
