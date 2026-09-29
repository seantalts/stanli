# Tape islands: compile irreducible scalar residue to one op

**Goal:** the scalar regions no pass can vectorize — cross-lane
recurrences (HMM forward algorithms, GARCH/ARMA state updates) — run
today as thousands of dispatched scalar ops at 0.2-0.8x of CmdStan.
Compile each surviving region into one register-machine program executed
by a single op: forward on doubles, backward by replaying the program
under stan-math nested autodiff. CmdStan parity by construction for code
we cannot vectorize, plus a double-speed forward CmdStan does not have.

**USER DECISION (2026-08-06):** islands are carved AFTER every other
pass has run — in-place, store-to-load forwarding, constant folding,
re-roll — so the vectorizing passes always get first crack and islands
take only the residue they provably cannot help. The carver is a new
final pass in `lower.cpp`'s pipeline, between `reroll` and
`reduce_terms`.

Affected corpus models: `iohmm_reg` 0.22x (sampling timeout),
`hmm_gaussian` 0.67x (sampling timeout), `hmm_drive_0/1` 0.75-0.80x,
`hmm_example` 0.78x, `garch11`, `arma11`, plus every model with a
parameter recurrence.

## Design

### The op

`OP_ISLAND`: `udata` carries an `IslandProg` (same idea as the ODE
`RhsProgram`); `in[0..n)` are the live-in SLOTS (params and intermediates
the region reads, each len 1 or a small vector — at most 6, else refuse);
`out` is one packed vector, one element per live-out scalar. After the
island the carver emits one `OP_INDEX` (scalar) or `OP_SLICE` (vector)
per live-out, writing the ORIGINAL live-out slot ids — downstream
readers, roots, and target terms are untouched, no renaming, and
adjoints flow through the existing INDEX/SLICE backwards.

Per-lane data constants (len-1 fill slots, the dedup'd const pool)
become immediates in the program, exactly as re-roll's `const_val` does —
without this every observation would be a live-in.

### The program

`IslandProg`: flat instruction list over a register file, templated
runner `run_island<T>` (double for forward, var for backward), same
pattern as `run_rhs`. Vocabulary = the graph opcodes that survive in
residue:

- arithmetic/unaries (ADD, SUB, MUL, DIV, NEG, EXP, LOG, SQRT, SQUARE,
  INV_LOGIT, LOG1M, TANH, ...) — direct instruction forms
- `OP_ADD_N` — a chain of ADDs
- `OP_INDEX` / `OP_SET_INDEX(_INPLACE)` with constant immediates — MOVs
  into register ranges (a len-k slot is k consecutive registers)
- `OP_LOG_SUM_EXP` over a range; `OP_LSE2`, `OP_LOG_MIX` — scalar calls
- scalar densities — a DENSITY instruction carrying the density opcode,
  the variant byte, and an optional integer outcome. The runtime
  reproduces the lane's exact instantiation: propto bit selects the
  <true>/<false> template, activity mask binds each argument as T or as
  `value_of(T)` via the same mask-dispatch recursion densities.cpp uses.
  This is what keeps the island's lp value identical to the scalar ops
  it replaces (propto constant-dropping included).

Anything else ends the run. No dynamic indexing in v1 (graph immediates
are constants).

### The kernel

- forward: snapshot live-in values into scratch (see below), run
  `run_island<double>` seeding registers from `ctx.in`, write the packed
  live-outs to `ctx.out`.
- backward: `nested_rev_autodiff`; bind live-ins as var FROM THE
  SNAPSHOT; run `run_island<var>`; seed with the dot trick
  (`j = sum_m out_var[m] * out_adj_vec[m]`), `grad(j)`, copy each
  live-in's adjoint into `in_adj[k]` where non-null.
- scratch = sum of live-in lens. The snapshot is a correctness
  requirement, not an optimization: the in-place pass ran EARLIER and
  may have allowed a destructive overwrite of a live-in buffer after the
  region, having proven the original ops' backwards were scratch-only.
  The island's backward replays values, so it must read the values as
  they were at forward time.

Cost per gradient: double forward (cheap) + var forward + var backward.
CmdStan pays var forward + var backward for the same statements — parity
by construction, plus our double forward is nearly free next to the ~17ns
per-op dispatch × region size it replaces.

### The carver (`island.cpp`, disable: `STANLI_NO_ISLAND=1`)

Scan the post-reroll op list for maximal runs of consecutive
compilable ops, then island a run when:

- run length ≥ `kMinIslandOps` (start at 32; calibrate on bench_opcost —
  below some size the scratch-partials machinery is cheaper than a var
  replay)
- every slot touched is len ≤ `kMaxIslandVec` (64) — registers, not
  arenas
- distinct non-const live-in slots ≤ 6
- no op in the run writes a root, and no target term is produced inside
  the run (terms stay graph-visible; the trailing INDEX extractions keep
  live-out slot ids stable so the term list never changes)

Refusals bail per-run, never per-model, like every other pass.

### v2, separate slice: necessity islands (parameter conditionals)

`IfElse`/ternary/`while` on parameters are hard compile errors today
(`lower.cpp:2205/1031`). With the island runtime in place, lowering can
compile such regions straight from MIR (the RhsProgram compiler already
handles runtime branches as JZ/JMP with both arms writing the same
registers) into an island op that flows through the passes opaquely.
Branch-on-parameter gradients differentiate the taken branch — identical
to CmdStan, because it IS stan-math AD executing the same scalar code.
Not in this slice: v1 proves the runtime; v2 adds the MIR-side compiler
entry point and unlocks coverage.

## Order of work

1. `IslandProg` + `run_island<T>` + the kernel, with a hand-built
   program unit test (values + gradients vs an equivalent op graph).
2. The carver + `OP_ISLAND` emission + trailing extractions; unit tests
   on an HMM-shaped unrolled graph (mini forward algorithm): parity
   against the un-islanded graph at 1e-12 rel, refusal tests (short
   runs, >6 live-ins, oversized vectors, unsupported opcode splits the
   run, terms/roots inside the run).
3. Wire into `lower.cpp` after reroll; spot A/B (`--filter hmm`,
   `garch`, `arma`) then bench the HMM family head-to-head.
4. Full corpus A/B runs after #20 lands (user decision: batch the gate,
   bisect on divergence — commits stay small and single-purpose).

## Expected impact

| model | now | expected |
| --- | ---: | ---: |
| `iohmm_reg` | 0.22x + sampling timeout | ~1x, timeout cleared |
| `hmm_gaussian` | 0.67x + sampling timeout | ~1x, timeout cleared |
| `hmm_drive_0/1`, `hmm_example` | 0.75-0.80x | ~1x + free double fwd |
| `garch11`, `arma11` | 0.43-0.81x | ~1x |

## Risks

- The var replay allocates on stan-math's nested arena each gradient;
  for very large regions this is CmdStan's own cost profile (that is the
  point), but the nested stack must be sized once and reused —
  measure allocation churn on iohmm_reg before calling it done.
- Densities inside islands must reproduce propto/activity instantiation
  exactly or the A/B flags value deviations; the DENSITY instruction
  carries the lane's variant byte verbatim and the unit test compares
  against the graph ops bit-for-bit on the double path.
- An island absorbing an op whose slot the in-place pass aliased
  elsewhere: the snapshot rule plus "no roots written inside" covers the
  known cases; the pass-safety fuzz test gets an island-enabled run.
