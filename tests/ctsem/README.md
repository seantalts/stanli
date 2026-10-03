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

The model goes through the same oracle as the rest of the corpus: CmdStan's
recorded log density and full gradient in
`docs/internal/artifacts/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py), held to the 1e-9
scaled-error gate. The references were recorded with CmdStan 2.40.0 (`d3d5df6a`), Stan
`a6806ef8` and Math `5252d51d` on Darwin arm64, and the entry carries that
as its own provenance block. No Linux x86_64 recording exists.
