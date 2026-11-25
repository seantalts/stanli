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
   mostly hurts when several chains share the caches.
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
used as a dimension or as the `upper=` of an index array. It scales rows
and columns by tiling or truncating every axis declared with that variable,
and groups by replicating the data set with the index values offset per
copy (or by keeping the first groups and their rows). It refuses what it
cannot prove from the declarations, such as a variable inside `N + 1`, a
container with two scaled axes, or ragged sizes that sum to N:

```sh
python3 tools/perturb/scale.py MODEL.stan DATA.json --list
python3 tools/perturb/scale.py MODEL.stan DATA.json --set N=1000 --out OUT.json
```

`--stage scaling` sweeps every seed's base over every size variable that
scale.py accepts, and every variant whose meta names `sizes`. The ladder is
0 (when the lower bound allows it), 1, 2, 3, 7, 8, 9, 31, 32, 33, 63, 64,
65, 100, 1000, 10^4 and 10^5. After it, the ladder adds the sizes 3% below
and above where the program would cross the 8 MiB program budget, the
32 MiB loop budget (`runtime/src/lower.cpp`) and the 2^20-op unroll
ceiling, estimated from the growth of the last two points under each cap.
A ladder stops before a size whose preparation is predicted to take more
than `--max-seconds` (60) or to need more than `--max-rss-gb` (4), or whose
data passes 400 MB. A size that the model or its data checks reject is
recorded as `rejected` and is not a finding.

Each point is checked at one point and prepared in parallel; then each is
timed once (`bench_grad --timed`, fixed points), one process at a time.
`--no-timing` stops after the structure. The findings:

| kind | meaning |
| --- | --- |
| `ops_grow` | the log-density or write_array op count grows with the size: log-log slope 0.5 or more over sizes 8 and up |
| `prep_superlinear` | preparation time, or peak RSS above the smallest size's, grows with an exponent above 1.15 between the two largest sizes |
| `per_element_jump` | ns per gradient divided by the size rises 1.3x or more between adjacent sizes; confirmed with 6 interleaved pairs |
| `program_changes_shape` | the decline reasons or the region opcodes change between adjacent sizes |
| `failure` | a crash, timeout, bad_alloc or refusal at some sizes only |

For `ops_grow` and `per_element_jump`, CmdStan is timed at the same two
sizes, and the finding shows both slowdowns and their ratio, so a cache
effect that both engines share is visible as such. `OUT/scaling.json` keeps
every point and `OUT/scaling.md` has the findings and one row per ladder.

## Known cliffs

`known.json` lists understood findings under `known`. Each entry has a
`kind` (`cliff` or a scaling finding kind), a `match` regular expression
searched in the JSON of the record, and a `reason` that names the cause and
the issue. The report marks matching findings as known. Add an entry only
when the cause is found and tracked, and remove it when the fix lands.
