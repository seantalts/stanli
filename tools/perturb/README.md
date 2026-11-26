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
changes the model or the data. Its optional `work` is how much more work
it should be, such as 16 for data rows tiled 16 times; `dose` is a label
that groups the doses of one family of variants in the report, such as a
lag of 40 or K = 8. A meta file may also name
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
   base's lp and gradient under the rules of `tools/check_compare.py`
   (relative 1e-10); other variants must give a finite lp and gradient.
   Differences in write_array, printed output or error wording do not count.
   Anything else is `invalid`, with the reason on one line, except that an
   equivalent variant whose lp and gradient are within 1e-7 relative of
   the base's is `near_equivalent` and goes on to the next stages with that
   label. The variant also runs through `stanli_check --interp` on the
   same MIR, without a gradient. When its lp or write_array rows differ
   from the compiled path's, the variant gets the label `interp_differs`
   and a line in the report; when the interpreter cannot run it, the label
   is `interp_unchecked`. This catches a variant that the compiled path
   gets wrong in the same way as its base. This stage
   takes about a second for 30 variants once the binaries are warm, so
   authors can use it as their check.
2. **Structure.** For base and variant: op counts of the log-density and
   write_array programs, islands, the decline reasons of reroll and
   partition (`STANLI_LANE_REPORT=1`), of the region map
   (`STANLI_REGION_MAP_DIAGNOSTICS=1`) and of the structured-loop lowering
   (`STANLI_STRUCTURED_LOOP_DIAGNOSTICS=1`), the region opcodes in the program (LOOP,
   REGION_MAP, ISLAND), the op count before the passes, preparation time
   and peak RSS (`bench_grad --prep`). A reason is written
   `pass/graph: reason`, such as `region_map/log_prob: assigns NAME declared
   outside the body`. The region map and structured-loop reasons come from
   the `refused` lines on stderr, with identifiers replaced by NAME and numbers
   by N so that `causes.py` can group them. `declines` lists each reason
   once and `decline_counts` says how many times it is reported (one per
   unfused op position for reroll and partition, one per refused loop for
   the others). A reason is a new decline when the base lacks it or the
   variant reports it more often; the report writes the second case as
   `reason (x5 more)`, and `new_declines` in `results.json` keeps the plain
   reason. The executor size is the estimate of
   `Executor::program_bytes`: 448 bytes per log-density op and 424 per
   write_array op, plus 16 bytes per slot element for values and adjoints.
   No tool prints this, so the constants are in `measure.py`. An equivalent
   variant whose prepared program equals the base's (`dump_ops` and
   `dump_islands` output) is `same_program` and is not timed. Otherwise
   stage 2 prints which parts differ (log_prob program, write_array program,
   islands), the final op count (the program that runs), the op count
   before the passes (which explains preparation time), and the preparation
   time. An equivalent variant that prepares 3x as long as the base, and
   100 ms longer, is a `prep_cliff`.
3. **Timing**, one process at a time: six interleaved pairs of fresh
   `bench_grad --timed` processes, base and variant, alternating which goes
   first. An equivalent variant uses the seed's committed benchmark points
   when the base files match the committed hashes, otherwise the fixed
   points. A non-equivalent variant uses the fixed points on both sides.
   The ratio is the median over the pairs. An equivalent variant is flagged
   at 1.3x slower; a variant with `work` when it is more than 1.3 times
   `work` slower; a variant without `work` always goes on to stage 4.
4. **CmdStan**, for flagged variants: the CmdStan gradient driver
   (`tools/bench_cmdstan_grad.cpp`) is built for both sources in parallel,
   cached in `OUT/cache/cmdstan`. Stanli is timed again with 24 pairs and
   CmdStan with 6. `cliff = (stanli variant/base) / (CmdStan variant/base)`;
   a cliff is reported at 1.3 or more. `--copies 12` also times 12
   concurrent copies of the variant and of the base, since a large program
   mostly hurts when several chains share the caches. Stage 3 already does
   this, flagged or not, for every variant whose base or variant executor
   is larger than 4 MiB.
5. **Report.** `OUT/results.json` keeps every record. `OUT/report.md` lists
   the cliffs ranked by `cliff`, then the cliffs with no new decline reason
   and no op growth ("unexplained"), a table of outcomes per class, and
   every variant. Outcomes: `invalid`, `same_program`, `fine`,
   `flagged_cmdstan_equal` (stanli slowed, CmdStan slowed as much), `cliff`,
   `failure` (stanli failed to prepare the variant) and `error` (the
   harness or a build failed).

Stage 3 prints a warning in the report when the 1-minute load passed 6.
Stages 3 and 4 keep each variant's results in `OUT/cache/timing`, under a
hash of its sources, data, meta and the `bench_grad` binary, so running
the same command again after an interruption resumes where it stopped.

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
scale.py accepts, and every variant whose meta names `sizes`. The default
ladder is coarse: 0 (when the lower bound allows it), 1, 8, 100 and 10^4.
Bisection (below) then finds the size at which the op count jumps, so the
coarse ladder locates a jump with far fewer points than the full one. On
ten corpus models (30 ladders) the coarse ladder took 135 s against 341 s.
It found the same cap crossings and three of the four thresholds. It missed
one threshold (radon between 431 and 432 rows, a jump that the op count
recovers from before 10^4), and six findings on five ladders, three of them
about preparation time. Use the coarse ladder to screen and the full one
for a model that you are writing up. `--ladder full` measures 0, 1, 2, 3, 7, 8, 9, 31, 32, 33, 63,
64, 65, 100, 1000, 10^4 and 10^5, which also shows changes at 8, 32 and 64
that do not move the op count. After it, the ladder adds the sizes 3% below
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
a `data_value_changes_program` finding. New declines are found as in stage 2,
by reason or by a higher count. For duplicate rows the count is
bisected to the smallest number of duplicates that changes the program.
`OUT/datavalues.json` keeps every perturbation and `OUT/datavalues.md`
lists the findings. Timing and CmdStan confirmation are left to the
variant stages.

## Findings by cause

`causes.py` gives every scaling and data-value finding its causes (a
decline reason that is new at the larger size or with the changed data,
or a new region opcode) and ranks the causes by the number of models that
show them, with the collections and two examples each. Shape changes
below size 8 are left out. A shape change that only removes a decline or a
lowering is filed under "a decline or lowering went away", so that "no new
decline or lowering switch" holds only findings with no change in declines. It also counts the size variables that
scale.py refused, by reason.

## Known cliffs

`known.json` lists understood findings under `known`. Each entry has a
`kind` (`cliff` or a scaling finding kind), a `match` regular expression
searched in the JSON of the record, and a `reason` that names the cause and
the issue. The report marks matching findings as known. Add an entry only
when the cause is found and tracked, and remove it when the fix lands.
