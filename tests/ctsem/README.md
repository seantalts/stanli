# Model from ctsem

[ctsem](https://github.com/cdriveraus/ctsem) fits continuous-time
state-space models. All of its Stan fits go through one generic template,
`inst/stan/ctsm.stan`, which computes a Kalman filter over every data row
inside one loop. It is the model behind the loop-lowering work in
[issue #248](https://github.com/seantalts/stanli/issues/248).

[`ctsem_ctsm.stan`](ctsem_ctsm.stan) is that file, checked in unmodified:

- revision `cdriveraus/ctsem@a0f1e69b7282c1dcaed820919ae0d8b280d076c4`
- SHA-256 `b148f5b3f129f981e7a3bfbd966e825c28fbc7314d14e2896d18c7c6bc1b01d2`
- URL `https://raw.githubusercontent.com/cdriveraus/ctsem/a0f1e69b7282c1dcaed820919ae0d8b280d076c4/inst/stan/ctsm.stan`
- license GPL-3, text in [`licenses/GPL-3`](licenses/GPL-3) and
  [`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md)

[`ctsem_ctsm.json`](ctsem_ctsm.json) is synthetic data, 40 subjects with 8
time points each (320 rows). [`tools/gen_ctsem_data.R`](../../tools/gen_ctsem_data.R)
builds it with the ctsem package installed from the revision above: the
two-process example from ctsem's `?ctGenerate` page, seed 20260903, then
the standata list ctsem builds for `ctFit(..., fit=FALSE, optimize=FALSE,
priors=TRUE)`, written out in the data-block order. Rerunning

    Rscript tools/gen_ctsem_data.R tests/ctsem/ctsem_ctsm.json --subjects 40

reproduces the file byte for byte. 40 subjects keeps the subject count above
the 32-iteration threshold of the mapped-region lowering while keeping
reference evaluations cheap.

[`ctsem_ctsm_400.json`](ctsem_ctsm_400.json) is the same generator at 400
subjects (3200 rows, 217 KiB), and
[`ctsem_ctsm_400.stan`](ctsem_ctsm_400.stan) is a byte-identical copy of
`ctsem_ctsm.stan`, because the corpus inventory pairs each model file with one
data file. Rerunning

    Rscript tools/gen_ctsem_data.R tests/ctsem/ctsem_ctsm_400.json --subjects 400

reproduces it. The size is chosen so that lowering the Kalman loop as
straight-line code is obvious. Unrolled, the 40 subject entry lowers to 26,941
graph ops and the 400 subject entry to 265,621, with preparation about three
times longer at 400 subjects. As one retained loop both lower to 402 ops, and
the 400 subject entry still needs only a second or two to prepare. A
CmdStan gradient at 3200 rows takes tens of milliseconds, so recording and
replaying it stays cheap.

[`tests/test_ctsem_engine.py`](../test_ctsem_engine.py) pins the engine for both
entries. The numerical replay cannot tell a retained loop from the same
computation unrolled, because the answers agree and only preparation time and
memory grow with the data. The test compiles each entry and checks that the
final `log_prob` graph holds exactly one `LOOP` and at most 2000 ops.

The models go through the same oracle as the rest of the corpus: CmdStan's
recorded log density and full gradient in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py), held to the 1e-9
scaled-error gate. The references were recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan
`a6806ef8` and Math `5252d51d` on Darwin arm64, and each entry carries that
as its own provenance block. No Linux x86_64 recording exists.
