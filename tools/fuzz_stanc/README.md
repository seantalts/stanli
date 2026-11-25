# Differential fuzzer for stanc3's optimizer

`gen.py` writes random Stan programs with data. `fuzz.py` compiles each one
with `stanli-compile --passes none` and `--passes o1`, runs both MIRs in
`stanli_check` at three points, and compares the log density, the gradient,
the write_array rows, printed output and error messages
(`differential.py`). When the answers differ, it adds the O1 passes one at a
time in stanc3's order and blames the first pass that moves the answer. The
blamed pass is where the difference first shows. The fault can be in that
pass or in how stanli runs either MIR.

## Running

```sh
python3 tools/fuzz_stanc/fuzz.py --seeds 0:20000 --jobs 24 --out DIR
```

`--compiler`, `--check` and `--bench` default to `deps/stanc3/stanli-compile`
and `build-rel/{stanli_check,bench_grad}`; copy them elsewhere if you rebuild
during a run. `DIR/summary.txt` has outcome counts, stanli's refusals and the
buckets, `DIR/results.json` one record per seed, and `DIR/failures/` up to
five programs per bucket with data, both MIRs and the differences.

Reproduce one seed with `--seeds N,N --jobs 1`, or by hand with
`stanli-compile --passes none|o1|PASS,PASS m.stan > x.mir` and
`stanli_check m.stan d.json --mir x.mir --point 1`. Shrink a program within
its bucket with `reduce.py --seed N --out DIR` and the same binary options.

## Buckets

A bucket is `PASS:KIND`. When a program differs in several ways, the first
kind in this list names it:

| kind | meaning |
| --- | --- |
| `compile_fail_o1`, `compile_fail_o0` | stanli refuses only one side |
| `crash`, `timeout` | `stanli_check` dies or runs out of time on one side |
| `one_side_threw` | one side throws at a point and the other returns values |
| `lp`, `grad` | log density or gradient differ beyond `1e-10` relative |
| `write_array` | write_array names or values differ |
| `error_message` | both sides throw with different messages |
| `output` | printed output differs |

Programs that use `~`, or `_lupdf` and `_lupmf` calls in `target +=`,
agree when lp differs by the same amount at every point and the gradients
agree, because these drop terms that the optimizer can make constant. The
summary counts them.

Performance checks use the bucket prefix `o1`: `slow_compile`,
`mir_growth` and `slow_gradient`. The thresholds are in the docstring of
`fuzz.py`. `slow_compile` times the compile that users get, without
`--passes`. After the parallel run, fuzz.py times the candidates for `slow_compile` and `slow_gradient` again one at a time.
`--retime-seconds S` stops that after S seconds, and the summary counts the
candidates left untimed.

## Known buckets

`known.json` lists differences that are understood and tracked. Each entry
has a `bucket`, a `match` regular expression searched in the JSON of the
program's differences, and a `reason` that names the cause and the issue.
A program whose bucket and differences match an entry is listed under
"known buckets" in the summary. Add an entry only when the cause is found
and tracked, and remove it when the fix lands.

## Nightly job

`.github/workflows/fuzz-nightly.yml` runs the fuzzer every night on one
Linux runner. It builds `stanli-compile` from the pinned fork and
`build-rel/{stanli_check,bench_grad}` with the shipped options, restoring
the caches that `clang-ccache-seed.yml` and the wheels build save. It never
saves a cache.

Each night runs 20,000 seeds starting at `1,000,000 + 50,000 * N`, where N
counts days since 2026-11-01, so every night covers new programs. Seeds
`0:200` also run every night as a regression check. On a 4-core runner the
fuzzing takes about 20 minutes, plus at most 5 minutes for the serial
timing of performance candidates (`--retime-seconds 300`). A manual run
(`workflow_dispatch`) takes a `seeds` list in fuzz.py's syntax and can skip
filing issues.

Only correctness buckets matter for the result. The performance buckets
(`slow_compile`, `slow_gradient`, `mir_growth`) depend on timings on a
shared runner or on fixed size ratios, so the job summary
lists them and nothing else happens.

`nightly.py` holds the steps:

- `seeds` prints the night's seed list.
- `reduce` runs `reduce.py` on the first seed of up to three new buckets,
  stopping each after five minutes. Results go in `RUN/reduced/`.
- `summary` writes the job summary.
- `file` opens one issue per new correctness bucket, titled with the bucket
  name and labeled `fuzzer`. The body has the seed, the commands to
  reproduce it, the differences, the reduced program when the reducer
  finished, and the program. When an open issue already has that title, it
  comments with the new seeds instead, at most once a week, and stays quiet
  when only regression seeds hit the bucket. Buckets in `known.json` never
  open issues. It writes the buckets it opened issues for to
  `RUN/filed.json`.
- `gate` fails the run on harness errors, or when fewer than half the
  programs agree, which means the tools are broken. It also fails on new
  correctness buckets: after `file`, only those that got a new issue in
  this run, so a bucket with an open issue does not keep every night red.
  Without `filed.json` every new bucket counts.

The fuzzing job uploads its run directory as the `fuzz-run` artifact. A
second job, which holds the only token that can write issues, downloads it
and runs `file` and `gate`.

To try the steps locally without touching GitHub, point `file` at a fake
`gh`, as `tests/test_fuzz_stanc_nightly.py` does:

```sh
python3 tools/fuzz_stanc/fuzz.py --seeds 0:200 --jobs 4 --out run
python3 tools/fuzz_stanc/nightly.py reduce run --time-limit 60
python3 tools/fuzz_stanc/nightly.py summary run
python3 tools/fuzz_stanc/nightly.py file run --repo OWNER/NAME --gh FAKE_GH
python3 tools/fuzz_stanc/nightly.py gate run
```
