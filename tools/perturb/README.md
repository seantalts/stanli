# Perturbation fuzzer for performance cliffs

Several slow models (issues #455, #571 and #657) were ordinary ways to
write a model that one of stanli's lane passes could not prove, so the
program kept thousands of scalar ops. This harness takes corpus models that
stanli runs well and asks two questions:

- Does a rewrite of the source that a user might write make the gradient
  much slower than the rewrite justifies?
- Does the gradient, or the program, grow faster than the data when a size
  in the data grows?

`run.py` judges hand-written variants. `run.py --stage scaling` sweeps the
data sizes of a model with `scale.py`, which needs no variants.

## Variants

```
VARIANTS/<seed>/base.stan, base.json          the corpus model and data, unchanged
VARIANTS/<seed>/<variant>.stan                the rewritten model
VARIANTS/<seed>/<variant>.json                optional other data; default base.json
VARIANTS/<seed>/<variant>.meta.json           {"class", "dose", "equivalent", "description"}
```

`<seed>` is the corpus model name. `run.py init VARIANTS MODEL,MODEL` copies
corpus models in as seeds. `equivalent: true` says that the parameters block
is unchanged and the log density and gradient equal the base's at the same
unconstrained points. When either source has `~`, `_lupdf` or `_lupmf`, lp
may differ by the same constant at every point. A non-equivalent variant
changes the model or the data; `dose` is how much more work it should be,
such as 16 for data rows tiled 16 times. A meta file may also name
`"sizes": {"N": [...]}` to sweep that variant in the scaling stage.

## Stages

```sh
python3 tools/perturb/run.py VARIANTS --out OUT --stage validity    # stage 1
python3 tools/perturb/run.py VARIANTS --out OUT --stage structure   # 1-2
python3 tools/perturb/run.py VARIANTS --out OUT --stage timing      # 1-3
python3 tools/perturb/run.py VARIANTS --out OUT [--copies 12]       # 1-5
python3 tools/perturb/run.py VARIANTS --out OUT --stage scaling     # size ladders
python3 tools/perturb/run.py VARIANTS --out OUT --stage datavalues  # same sizes, other values
python3 tools/perturb/causes.py SCALING_OUT DATAVALUES_OUT           # findings by cause
```

`--only TEXT` keeps the variants whose `seed/variant` contains TEXT, and
`--jobs` (default 28) sets how many run at once in stages 1 and 2.

1. **Validity.** Base and variant are compiled as the corpus benchmark does
   (the embedded compiler with loop vectorization on) and run in
   `stanli_check` at three points. An equivalent variant must give the
   base's lp and gradient under `tools/fuzz_stanc/differential.py`'s rules
   (relative 1e-10); other variants must give a finite lp and gradient.
   Differences in write_array, printed output or error wording do not count.
   Anything else is `invalid`, with the reason on one line. This stage
   takes about a second for 30 variants once the binaries are warm, so
   authors can use it as their check.
2. **Structure.** For base and variant: op counts of the log-density and
   write_array programs, islands, the reroll and partition decline reasons
   (`STANLI_LANE_REPORT=1`), the region opcodes in the program (LOOP,
   REGION_MAP, ISLAND), the op count before the passes, preparation time
   and peak RSS (`bench_grad --prep`). The executor size is the estimate of
   `Executor::program_bytes`: 448 bytes per log-density op and 424 per
   write_array op, plus 16 bytes per slot element for values and adjoints.
   No tool prints this, so the constants are in `measure.py`. An equivalent
   variant whose prepared program equals the base's (`dump_ops` and
   `dump_islands` output) is `same_program` and is not timed.
3. **Timing**, one process at a time: six interleaved pairs of fresh
   `bench_grad --timed` processes, base and variant, alternating which goes
   first. An equivalent variant uses the seed's committed benchmark points
   when the base files match the committed hashes, otherwise the fixed
   points. A non-equivalent variant uses the fixed points on both sides.
   The ratio is the median over the pairs. An equivalent variant is flagged
   at 1.3x slower; a variant with a dose when it is more than 1.3 times the
   dose slower; a variant without a dose always goes on to stage 4.
4. **CmdStan**, for flagged variants: the CmdStan gradient driver
   (`tools/bench_cmdstan_grad.cpp`) is built for both sources in parallel,
   cached in `OUT/cache/cmdstan`. Stanli is timed again with 24 pairs and
   CmdStan with 6. `cliff = (stanli variant/base) / (CmdStan variant/base)`;
   a cliff is reported at 1.3 or more. `--copies 12` also times 12
   concurrent copies of the variant and of the base, since a large program
   mostly hurts when several chains share the caches. It runs only for
   variants that stage 3 flagged, so a variant that is fast alone and slow
   with 12 chains is missed unless it is also flagged.
5. **Report.** `OUT/results.json` keeps every record. `OUT/report.md` lists
   the cliffs ranked by `cliff`, then the cliffs with no new decline reason
   and no op growth ("unexplained"), a table of outcomes per class, and
   every variant. Outcomes: `invalid`, `same_program`, `fine`,
   `flagged_cmdstan_equal` (stanli slowed, CmdStan slowed as much), `cliff`,
   `failure` (stanli failed to prepare the variant) and `error` (the
   harness or a build failed).

Stage 3 prints a warning in the report when the 1-minute load passed 6.

## Scaling

`scale.py` reads the data block and finds the size variables: int scalars
used as a dimension or as the `upper=` of an index array. It grows rows
and columns along every axis declared with that variable, and shrinks
them by truncating. Groups grow by replicating the data set with the index
values offset per copy, and shrink by keeping the first groups and their
rows. There are two ways to grow:

- `resample` (the ladder's default) draws the new rows with replacement
  from the original ones and moves each real value by 1% of its column's
  spread, inside the declared bounds and without changing its sign, so
  the new rows are distinct. A constant column, such as an intercept,
  stays constant. Integer and index values are copied as they are. Group
  copies get the same noise.
- `tile` repeats the rows cyclically, which makes exact duplicates.
  Duplicate rows change what stanli's passes prove (see the data-value
  stage), so a tiled ladder mixes two effects.

scale.py refuses what it cannot prove from the declarations: a variable
inside `N + 1`, a container with two scaled axes, a constrained type along
the axis, an if-condition that compares the variable with another one
(usually a check with `reject`), or ragged sizes: an int array that sums
to the variable with values other than 0 and 1, or that the program uses
to cut slices (`segment` or `a:b`). Equal values alone do not count as a
relation.

```sh
python3 tools/perturb/scale.py MODEL.stan DATA.json --list
python3 tools/perturb/scale.py MODEL.stan DATA.json --set N=1000 [--mode tile] --out OUT.json
```

`--stage scaling` sweeps every seed's base over every size variable that
scale.py accepts, and every variant whose meta names `sizes`. The ladder is
0 (when the lower bound allows it), 1, 2, 3, 7, 8, 9, 31, 32, 33, 63, 64,
65, 100, 1000, 10^4 and 10^5. After it, the ladder adds the sizes 3% below
and above where the program would cross the 8 MiB program budget, the
32 MiB loop budget (`runtime/src/lower.cpp`) and the 2^20-op unroll
ceiling, estimated from the growth of the last two points under each cap.
A ladder stops before a size whose preparation is predicted to take more
than `--max-seconds` (60) or to need more than `--max-rss-gb` (6), or whose
data passes 400 MB, and once its share of `--model-seconds` (180 s for
all of a model's ladders) is spent. Every tool process is killed when it
passes `--timeout` seconds or `--memory-gb` of resident memory (6); a size
killed for memory is recorded as `capped` and ends its ladder.
`--growth tile` grows by tiling. When the log-density op count more than doubles
between two adjacent sizes, the ladder bisects between them to find the
size where it jumps. A size that the model rejects is recorded as
`rejected` and is not a finding: a failed data or model check, a Stan Math
error while transformed data runs (often a relation the declarations do
not state, such as brms's `Kc == K - 1`), or a non-finite lp. Only a
crash, a timeout, or stanli refusing to lower is a `failure`.

Each point is checked at one point and prepared in parallel; then each is
timed once (`bench_grad --timed`, fixed points), one process at a time.
A size whose executor is larger than 4 MiB is also timed as 12 concurrent
processes, as is any variant in stage 3 whose base or variant executor is.
`--no-timing` stops after the structure and deletes the scaled data files.
The findings:

| kind | meaning |
| --- | --- |
| `ops_grow` | the log-density or write_array op count grows with the size: log-log slope 0.5 or more over sizes 8 and up |
| `prep_superlinear` | preparation time, or peak RSS above the smallest size's, grows with an exponent above 1.15 between the two largest sizes |
| `per_element_jump` | ns per gradient divided by the size rises 1.3x or more between adjacent sizes of 2 or more; confirmed with 6 interleaved pairs |
| `program_changes_shape` | the decline reasons or the region opcodes change between adjacent sizes |
| `failure` | a crash, timeout, bad_alloc or refusal at some sizes only |

For `ops_grow` and `per_element_jump`, CmdStan is timed at the same two
sizes, and the finding shows both slowdowns and their ratio, so a cache
effect that both engines share is visible as such. `OUT/scaling.json` keeps
every point and `OUT/scaling.md` has the findings and one row per ladder.

## Data values

`--stage datavalues` keeps each seed's source and sizes and changes the
values of its base data, along every rows variable whose containers all
have it as their first axis:

| kind | values |
| --- | --- |
| `duplicate_rows` | 1%, 10% and 50% of the rows replaced by copies of other rows, and the first half tiled |
| `discrete_covariate` | every real column rounded to its nearest of 2, 5 or 20 quantiles |
| `constant_column`, `zero_column` | one real column set to its first value, or to 0 when the bounds allow it (up to 3 columns) |
| `intercept_column` | a column of ones added to a matrix whose column count scale.py can grow |
| `index_order` | rows sorted by a group index, reversed or shuffled; the index made constant; half the rows moved to group 1 |
| `unbalanced_groups` | every group but one holds one row, and group 1 holds the rest |

A perturbation whose log-density program grows 1.5x (and by 20 ops or
more), or whose loops are lowered another way, than with the base data is
a `data_value_changes_program` finding. For duplicate rows the count is
bisected to the smallest number of duplicates that changes the program.
`OUT/datavalues.json` keeps every perturbation and `OUT/datavalues.md`
lists the findings. Timing and CmdStan confirmation are left to the
variant stages.

## Findings by cause

`causes.py` gives every scaling and data-value finding its causes (a
decline reason that is new at the larger size or with the changed data,
or a new region opcode) and ranks the causes by the number of models that
show them, with the collections and two examples each. Shape changes
below size 8 are left out. It also counts the size variables that
scale.py refused, by reason.

## Known cliffs

`known.json` lists understood findings under `known`. Each entry has a
`kind` (`cliff` or a scaling finding kind), a `match` regular expression
searched in the JSON of the record, and a `reason` that names the cause and
the issue. The report marks matching findings as known. Add an entry only
when the cause is found and tracked, and remove it when the fix lands.
