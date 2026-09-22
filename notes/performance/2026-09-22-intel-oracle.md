# Intel macOS oracle for kronecker_gp

Release validation at `f1face395` failed point zero against the Darwin arm64
recording: scaled gradient error 0.0434879 exceeded the existing 0.0328 gate.
An [independent Intel recording](https://github.com/seantalts/stanli/actions/runs/35775679693)
used the same pinned CmdStan 2.40, Stan, Math and stanc3 sources, AppleClang
17.0.0, and the existing reference driver with `-O1 -ffp-contract=off`.

At the offending component, Intel CmdStan and Stanli both return
`-25.710840086478377`; the arm64 oracle returns `-24.592729467555493`.
This failure therefore measures cross-platform oracle drift. The remaining
same-platform maximum scaled differences at the three points are 0.001771,
0.000399 and 0.000418, in the model's already documented unstable bandwidth
gradient. These remain within the original gate.

`docs/corpus-refs-darwin-x86_64.json.gz` retains all three independent CmdStan
answers, input/toolchain hashes and provenance. Selection happens by platform
and compiler before evaluation. The original recording, point statuses,
recorded deviations and tolerance formula remain unchanged. The supplement
preserves the original log-density/gradient coverage; it does not add an
eigenvector-output gate. Those outputs have different bases in a nearly
degenerate eigenspace (maximum component differences 0.805, 0.795 and 0.750)
and were already absent from the original gate. Raw outputs from both engines,
including those eigenvectors, remain in the workflow artifact.

The diagnostic ran at `42535829`, which differs from the failed main runtime
only in release metadata and the diagnostic files. The subsequent identity
slice fix is independently checked for exact gradient preservation. The
ordinary Intel release replay remains required.
