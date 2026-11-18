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
| `empty_return_shape` | the shape of a user's model from PR #568, rewritten | a user function returning a tuple whose array of matrices is empty for one call and not for the other, read in transformed parameters and the model; a function returning an empty array of matrices in the model and generated quantities. `--O1` declares each inlined return temporary as `array[0] matrix[0, 0]`, and the empty destination takes the shape of the value assigned to it |
| `reject_udf_data_size` | the shape of OncoBayes2's `max_int`, rewritten | a data size, `array[M] vector[max_int(Nc)]`, computed by a user function that calls `reject()` |
| `tuple_array_params` | written for this directory | arrays of tuples as parameters: a two-dimensional array of a bounded real and a simplex, an array of tuples holding an array of tuples with an offset/multiplier real, a simplex and a Cholesky correlation factor, and a tuple of a bounded vector and an array; a transformed-parameter array of tuples built from them and a generated-quantities array of tuples of draws |
| `tuple_coef_cont` | the shape of a user's model from issue #567, rewritten | a user function returning a five-tuple of vector, matrices and arrays of matrices (one sized `P_r[1] ? R : 1`) into a model-block local, projected into the target; tuple data, an array of tuples in data and generated quantities, and a tuple parameter |
| `tuple_decompositions` | written for this directory | `eigendecompose_sym`, `qr_thin` (unpacked into two matrices) and `qr`, which return tuples, in transformed data, the model and generated quantities |
| `tuple_decompositions_more` | written for this directory | `svd` of a square and a non-square matrix in transformed data, the model and generated quantities; `csr_extract` of data matrices in transformed data and generated quantities; `eigendecompose_sym` and `qr_thin` inside a `while` loop whose condition reads a parameter; `qr` of a parameter in generated quantities |
| `tuple_effect_once` | written for this directory | a user function taking a tuple, called on an overloaded function that returns a tuple, prints and draws, so it is not inlined; the inlined body reads both slots of the argument, and the call must still run once |
| `tuple_loops` | written for this directory | arrays of tuples read and written element by element in loops, as data, a model-block local and generated quantities with RNG draws; a projection of a tuple of two draws; tuple parameters with a bound and a simplex; a bounded tuple transformed parameter from a user function that returns from a branch on a parameter and from inside a loop; an int promoted to real in a tuple |
| `tuple_reduce_sum` | written for this directory | `reduce_sum` and `reduce_sum_static` whose shared arguments are a tuple and an array of tuples from data, and a partial sum calling an overloaded user function that returns a tuple; stanc3 refuses an array of tuples as the sliced argument |

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
