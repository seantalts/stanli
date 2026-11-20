# 2026-11-20 · Why reroll and partition decline lanes

Tree: `fix/audit3-passes` at f94c05155 plus the decline-reason commit, a
Release build with debug switches. Command:

```
python3 tools/lane_declines.py deps/posteriordb OUT --build build-rel --jobs 6
```

It compiles the 402 models of `corpus_cases(..., "all")` with the embedded
compiler and prepares each one with `dump_ops` under `STANLI_LANE_REPORT=1`.
`bayesdfa_dfa_gp` fails to prepare (`gp_exp_quad_cov` unsupported, the same
on the integration build), so the table covers 401 models.

A row counts declined groups of matching lanes. Reroll tries several
periods at one position, so the tool counts a position once, under the
reason that the shortest period gave. Partition records one entry per
bucket that it declines and does not split further.

| pass | graph | reason | positions | models | collections |
|---|---|---|---:|---:|---:|
| reroll | log_prob | lanes read different non-constant slots | 39090 | 87 | 14 |
| partition | log_prob | lanes read different non-constant slots | 155 | 76 | 14 |
| reroll | write_array | lanes read different non-constant slots | 45682 | 46 | 13 |
| reroll | log_prob | a plain op reads different slots in the first lanes | 2534 | 38 | 13 |
| reroll | log_prob | a store does not march through one private vector | 1022 | 30 | 9 |
| reroll | write_array | a store does not march through one private vector | 196 | 21 | 7 |
| partition | log_prob | an op has no lane rule | 29 | 19 | 6 |
| partition | log_prob | the graph already used its split budget | 228 | 18 | 6 |
| partition | log_prob | a store does not march through one private vector | 42 | 18 | 9 |
| partition | bounded_log_prob | lanes read different non-constant slots | 37 | 14 | 3 |
| reroll | log_prob | the density has no fused form for these operands | 149 | 13 | 5 |
| partition | log_prob | the cost model prefers the scalar form | 19 | 13 | 5 |
| reroll | log_prob | an active scalar is read at two positions | 103 | 12 | 4 |
| reroll | write_array | a plain op reads different slots in the first lanes | 522 | 11 | 6 |
| reroll | write_array | the density has no fused form for these operands | 105 | 11 | 3 |
| partition | log_prob | lane widths do not agree | 14 | 11 | 4 |
| reroll | bounded_log_prob | lanes read different non-constant slots | 24 | 9 | 2 |
| reroll | log_prob | a row read or gather does not cover a packable range | 59 | 8 | 4 |
| reroll | log_prob | the cost model prefers the scalar form | 41 | 7 | 3 |
| partition | log_prob | an active scalar is read at two positions | 7 | 6 | 2 |
| reroll | bounded_log_prob | a plain op reads different slots in the first lanes | 30 | 5 | 1 |
| partition | log_prob | every lane computes the same term | 7 | 5 | 3 |
| reroll | log_prob | a lane value is read outside its lane | 83 | 4 | 3 |
| reroll | log_prob | an indexed read is out of range or has a varying base | 7 | 2 | 1 |
| reroll | write_array | a row read or gather does not cover a packable range | 3 | 2 | 2 |
| partition | bounded_log_prob | an op has no lane rule | 2 | 2 | 1 |

Models with no declined group: 221 of 401.
Models whose target lists one slot more than once: 15 (M0_model, Mt_model, aalto_poisson_hurdle, ch12_m12_3, ch12_m12_3_alt, ch12_m12_4, ch15_m15_8, ch15_m15_9, dogs_hierarchical, extra_hurdle_poisson, multi_occupancy, oncobayes2_blrm_exnex_sa, s2_hurdle_cumulative, s2_zi_beta, s2_zoi_beta).

Failed: bayesdfa_dfa_gp (dump_ops exit -6)

What the table says:

- "Lanes read different non-constant slots" is the most common reason in
  both passes. In partition it covers 76 models in 14 collections. Some of
  these are recurrences (each lane reads the previous lane's value), which
  no split can fuse; the rest are lanes that read one of a few slots, as
  the two branches of a data condition do. This is the case that splitting
  a bucket by slot addresses.
- The global cap of 32 partition splits is reached in 18 models
  (covid19imperial_v2 and v3 drop 100 later buckets each). This did not
  decide fusion: with the cap per bucket (later commit), or with no cap at
  all, no corpus program changes, because the buckets that reached it
  decline for other reasons once they get their splits.
- 15 models list one slot more than once in the target. The lane passes see
  those terms multiplied by their count (`scale_repeated_terms`), and no
  decline reason in the table is specific to them.

## After the lane changes of 2026-11-20

Splitting a bucket by slot changed three programs: `ch14_m14_7` (2,615 to
58 ops), `examplemodels_cjs_cov_raneff` (75,299 to 74,120) and
`hbayesdm_gng_m4` (80,277 to 80,146). Partition's "different non-constant
slots" row went from 76 to 71 models; most of what remains are recurrences,
where each lane reads the previous lane's value. The effect barrier, the
per-bucket split budget and the bound on reroll's lane count changed no
corpus program.
