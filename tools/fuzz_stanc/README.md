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

Programs that use `~` agree when lp differs by the same amount at every
point and the gradients agree, because `~` drops terms that the optimizer
can make constant. The summary counts them.

Performance checks use the bucket prefix `o1`: `slow_compile`,
`inline_growth`, `mir_growth` and `slow_gradient`. The thresholds are in
the docstring of `fuzz.py`.

## Known buckets

`known.json` lists differences that are understood and tracked. Each entry
has a `bucket`, a `match` regular expression searched in the JSON of the
program's differences, and a `reason` that names the cause and the issue.
A program whose bucket and differences match an entry is listed under
"known buckets" in the summary. Add an entry only when the cause is found
and tracked, and remove it when the fix lands.
