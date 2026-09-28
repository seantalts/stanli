# Compile value-only runtime callback arguments

Base52ee7217; synchronized upstream base5b913866768f351f86eed4d5a7246414530f3b2d.
Native Release, AppleClang21, Darwin arm64. Instruction-generation/JIT research
remains tabled. This patch extends existing graph/register/callback engines.

## Change and proof

A data-only real can vary with the draw in generated quantities. The old
packer demanded such values as preparation constants, so the entire output
block fell back to MirInterp. In write_array only, real callback actuals now
use the existing runtime theta buffer; integer arguments and solver controls
retain their static requirements. Runtime availability and scalar AD activity
are separate: every write_array solver uses double inputs and zero activity
bits, retaining the ODE/DAE/adjoint mask-present flags. Legacy algebra's Program
mask is made consistent too; legacy ODE/algebra runtime x_r coverage is not
claimed by this extension.

Explicitly inactive ODE, DAE, algebra and quadrature calls now reserve no
derivative scratch. Their forwards do not write it and their backwards do not
read it or change adjoints. Active variants retain their existing layout.
Adjoint ODE already needed no such scratch. This prevents an unused
output-by-parameter Jacobian from growing with newly runtime-bound real arrays.
The RhsArg layout and public ABI are unchanged by this patch.

The kernel test executes all five families with null scratch and sentinel
adjoints, finite/Inf/NaN seeds, both ODE input encodings, and million-element
shape-only scratch queries. It explicitly checks that missing activity markers
retain legacy active allocation. The previous all-double ODE test expected a
materialized zero Jacobian; it now expects no storage, while all mixed-activity
zero-column and exact pullback checks remain intact.

## Validation

Clean runtime-contract rebuild. All 292 native tests pass, including the final
RNG callback fixture and the reviewed scratch/encoding checks. All329 recorded
CmdStan models pass at three points, with1,020,194 values and all124 platform ULP
gates. Worst corpus error remains9.38e-13 scaled /7040 ULP; this is not a global
ten-ULP claim. Installed Python and R tests pass after the reviewed source fix.

New independently recorded CmdStan2.40.0 references cover data-qualified scalar,
vector, empty-vector and matrix runtime arguments, sign-dependent callback
branches, an ODE/legacy-algebra function used in both model and GQ, RNG-valued
actuals plus the next draw, and deliberately retained integer/control/dynamic
local fallbacks. The ordinary GQ model selects seven compiled double solver
sites. A refused callback remains local to its compiled output graph. A
separate dynamic-local output fixture preserves whole-block fallback coverage.

Across10 models, six calls each on one instance with changing signs and a
revisited point, old and new public C API density/gradient/output bits agree
exactly:60 cases /552 values. The stochastic case uses an explicit seed/chain
and verifies subsequent RNG state. Fable reviewed the plan and inactive-scratch
implementation. Its legacy-algebra mask and test-encoding findings are fixed;
review texts and dispositions are retained beside this note.

## Measurements

Six alternating fresh-process pairs per case. Phase measurements use the public
C API,200ms warmup and250ms windows; source compilation is measured separately
from preparation of existing MIR. Short inference includes100 warmup,100 draws
and all output rows. The isolated GQ model has a cheap Gaussian target; the
combined context model also has solvers in its density and is not an ordinary
posterior workload. These are bounded coverage/performance fixtures.

| Median metric | Runtime-values GQ before→after | Combined contexts before→after |
| --- | ---: | ---: |
| Prepare MIR |420.7→421.8 us|649.8→456.1 us|
| First gradient |6.0→12.8 us|182.3→305.9 us|
| First output row |350.4→105.5 us|293.6→40.9 us|
| Warm output row |268.7→22.2 us|259.4→18.5 us|
| Inference plus rows |17.191→1.902 ms|10.183→10.253 s|
| Whole-process peak RSS |28.885→29.368 MB|102.924→100.123 MB|

Warm rows improve12.1x and14.0x. The GQ-focused complete inference example
improves9.0x; the density-dominated combined fixture does not improve overall.
First-gradient increases are explicit costs. Less preparation-time interpreted
probing can shift cold initialization into first use, but that attribution has
not been separately proven. Prepared graphs also retain code/specs; the GQ-only
process peak rises0.483MB. No claim of universally lower memory or startup cost
is made.

Direct native gradient medians(MAD), avoiding Python overhead, are37.93(0.58)→
38.47(1.26)ns for the cheap target,162.49(4.36)→158.17(3.86)us for combined
contexts, and1.865(0.064)→1.867(0.077)us for the ordinary branch-ODE canary.
Gradient values are bitwise identical. These measurements do not establish a
model-gradient speedup from a write_array optimization.

The canary's initial short inference+outputs increases3.164→3.387ms(+7.1%).
The predeclared eight A/A and eight A/B warmed-pair confirmation retains an
A/B inference ratio of1.039(MAD0.035), versus A/A0.963(MAD0.044); gradient ratios
are1.028(MAD0.022) versus0.940(MAD0.049). Draws and output hashes agree across
all32 processes. The distributions overlap substantially, so this follow-up
is inconclusive; it does not prove absence of a small slowdown. The original
+7.1% single-inference signal is retained as a limitation. The direct native
canary gradient remains effectively flat. No further timing retries were used.
The library's uncompressed size is unchanged(39,647,328 bytes); gzip decreases
104 bytes. No dependency is added. Numerical oracles, raw phase/native samples,
manifest hashes, canary controls and size evidence are retained in the adjacent
`2026-09-28-callback-runtime-values-performance.json`.
