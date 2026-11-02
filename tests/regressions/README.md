# Lowering regressions

Programs, or reductions of programs, that once failed to lower. They go
through the same CmdStan oracle as posteriordb: references in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
`tools/verify_refs.py`.

| model | origin | what it reaches |
| --- | --- | --- |
| `while_counter_size` | written for this directory | a `while` loop counting up to a size, then `vector[k] z = mu[1:k]` in the model and transformed parameters blocks |
| `while_udf_slices` | `integer_division` and `to_vector_of_arrays` from rbmi (Apache-2.0, see `licenses/rbmi-Apache-2.0`), reshaped into a small model | a user function slicing a vector by a `while` counter, and a data size computed by a `while` loop in a user function |
| `rbmi_mmrm` | rbmi's MMRM model (Apache-2.0, see `licenses/rbmi-Apache-2.0`), unchanged, with small data written for this directory | the counter and slicing loops in one model, plus `-1/(n_visit - 1)` as a parameter bound, an integer division |
| `reject_udf_data_size` | the shape of OncoBayes2's `max_int`, rewritten | a data size, `array[M] vector[max_int(Nc)]`, computed by a user function that calls `reject()` |

`reject_udf_data_size` is valid at the committed data. With an empty `Nc` the
function rejects, and both CmdStan and stanli stop at data load with the
function's message.

To add one, put `foo.stan` and `foo.json` here and record its reference:

```
python3 tools/verify_sample.py deps/cmdstan deps/posteriordb foo
```

Commit `corpus-refs.json.gz` and `verification.json`, then run
`tools/gen_docs.py`. Models from stanc3's own test suite belong in
[`tests/stanc3_good/`](../stanc3_good/README.md) instead.
