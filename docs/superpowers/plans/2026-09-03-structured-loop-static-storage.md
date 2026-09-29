# Structured loops: one executor, static storage classes

Date: 2026-09-03. Branch `feat/loop-jit-prep`. Base: main after #312.

## Why

The retained-loop executor that landed in #306/#309/#311 stores a fresh
version of every kernel output, including whole-container copies for
`x[i] = v` and every data-only intermediate. Four runtime "plans" and a
four-band double-encoded handle scheme were then added to reclaim the memory
that model wastes. Measured on main (2026-09-03):

- `x[i] = f(x[i-1])` over a `vector[N]`: history is O(N^2). N=10k gradient is
  326x slower than unrolling and takes 1.6 GB; radon_county selects a 2.5 GB
  rectangle per Executor. With dynamic history the reverse pass is still
  O(N) per scalar update, so N=100k takes 5.7 s per gradient.
- Fixed dispatch: ~35 ns per kernel per gradient against ~15 ns flat. 38% of
  the time is rebuilding `KernelCtx` per call; the flat executor prebinds it.
- Every plan is a runtime decision. A stencil JIT of the body needs each
  node's storage decided at prepare time.

This change deletes the rectangular executor, the dynamic executor's plans
and handle bands, the target-fragment machinery, and the while/for capacity
proofs, and replaces them with one executor over static per-node storage
classes. The compiler side (`lower_structured_loop.inc`) keeps its body
lowering; only capacity requirements and the target export change.

## Payload

```cpp
struct StructuredLoop {
  struct Node {
    enum Kind { Sequence, KernelCall, Alias, If, For, While, Break, Continue,
                Target } kind;
    // KernelCall only, decided by prepare():
    enum Storage { Retained, Transient, InPlace } storage = Retained;
    bool active = false;        // static: this call's backward may run
    int invariant_loop = -1;    // dense loop index whose entry invalidates
                                // a cached result; -1 recomputes every visit
    uint32_t site = ~0u;        // dense KernelCall index
    int64_t workspace = -1;     // Transient: offset into the state workspace
    int64_t kernel_scratch = 0;
    int loop_index = -1;        // For/While: dense loop index
    std::vector<Node> children;
    int op = -1, dst = -1, src = -1;
    int condition = -1, iterator = -1, lower = -1, upper = -1;
    void (*forward)(KernelCtx&) = nullptr;
    void (*backward)(KernelCtx&) = nullptr;
  };
  struct Import { int slot = -1; int input = -1; int64_t offset = 0;
                  bool active = false; };
  Graph body;
  std::vector<std::pair<int, std::vector<double>>> fills;
  std::vector<Import> imports;
  std::vector<int> outputs;
  bool has_target = false;      // one scalar output after `outputs`
  Node root;
  int64_t initial_size = 0;     // sum of body slot lengths
  int64_t workspace_size = 0;   // sum of Transient out/out2/scratch lengths
  size_t node_count = 0, site_count = 0, loop_count = 0;
  void prepare();
};
```

Removed: `capacity`, `frame_size`, `target_capacity`, `compact_update_cell`,
`record_site`, `dynamic_history`, `target_fragment`, every `*_offset`,
`primal_size`, `scratch_size`, `record_node_count`, `compact_update_sites`,
`TargetReduction`, `OP_TARGET_REDUCE`, `prepare(int64_t max_bytes)`.

`Import.active` comes from the parent `Val::autodiff` of the imported value
(runtime dimension slots are inactive).

## prepare()

Validation as today (slot ranges, arity, kernel registered, no stateful
kernel, no in-place opcodes, no OP_ISLAND/OP_LOOP, Alias lengths equal,
Break/Continue inside a loop, control slots scalar). Then, in order:

1. Number sites (KernelCall preorder) and loops (For/While preorder). Assign
   `body.slots[i].offset` cumulatively; `initial_size` is the total.
2. Fuse in-place updates. In every Sequence, a `KernelCall` whose op is
   `OP_SET_INDEX_DYNAMIC` with `out = o`, `in[0] = B`, immediately followed by
   `Alias{dst = B, src = o}`, where `o` has no other reader anywhere in the
   tree (no other KernelCall input, no other Alias src, not a Target src, not
   in `outputs`), becomes `storage = InPlace` and the Alias is erased from the
   Sequence. For invariance and activity below, an InPlace node writes `B`.
3. Static activity. `active[slot]` starts true for active imports, false
   otherwise. Iterate to a fixed point over the whole tree: for a KernelCall
   with `backward != nullptr`, `active[out] |= any(active[in[k]])`, same for
   `out2`; for InPlace, `active[B] |= active[rhs]`; for Alias,
   `active[dst] |= active[src]`. A KernelCall is `active` iff it has a
   backward and any input is active (InPlace: base or rhs active).
4. Written sets. For each loop node L, `written(L)` = out/out2 of KernelCalls
   under L, dst of Aliases under L, B of InPlace nodes under L, iterators of
   For nodes under L (including L itself).
5. Invariance. For a KernelCall K (not InPlace, not `is_effectful_op`), walk
   its enclosing loops from outermost to innermost; `invariant_loop` is the
   outermost loop L such that no input slot of K is in `written(L)`.
   Otherwise -1. Nodes outside any loop stay -1.
6. Transient. A KernelCall K with `!active` is Transient iff for its `out`
   (and `out2`): no Alias has it as src, no Target has it as src, it is not
   in `outputs`, no InPlace node has it as base, and every KernelCall reading
   it is `!active`. Control reads (If/While condition, For bounds) are
   allowed. Transient nodes get `workspace` offsets covering out, out2 and
   kernel scratch. Everything else is Retained.

Out slots are unique per KernelCall (lowering emits a fresh slot per
`emit_value`); prepare asserts this.

## Executor state (one per bound Executor, `KernelState`)

```cpp
struct Version { double* value; int64_t adjoint; };
// adjoint >= 0: offset into `adjoints`; -1: inactive;
// <= -2: import ordinal -(adjoint + 2), adjoint lives in outer in_adj.
struct Record {
  uint32_t site; enum { Kernel, InPlace, Copy } kind;
  int64_t handles;                 // Kernel: offset into `handles`, n_in entries
  int64_t out, out2;               // Kernel: output versions (out2 = -1 if none)
  double* scratch;                 // Kernel
  int64_t base, rhs, undo, count;  // InPlace: versions and undo span
  int64_t from, to;                // Copy
};
struct LoopState : KernelState {
  BlockArena arena;                 // reached values and kernel scratch
  std::vector<double> workspace;    // Transient cells, sized once
  std::vector<Version> versions;
  std::vector<int64_t> bindings;    // slot -> version
  std::vector<int64_t> handles;     // saved input versions of Kernel records
  std::vector<double> undo;         // InPlace: count pairs (position, old)
  std::vector<Record> records;
  std::vector<int64_t> target_refs;
  std::vector<int32_t> owner;       // version -> slot that may mutate it, or -1
  std::vector<int64_t> node_generation;   // site -> loop generation when cached
  std::vector<int64_t> node_version, node_version2;  // cached outputs
  std::vector<int64_t> loop_generation;   // loop -> entries so far
  std::vector<KernelCtx> ctx;       // site -> prebound template
  std::vector<const Node*> sites;
  std::vector<double> adjoints; int64_t adjoint_size = 0;
  bool reverse_ready = false;
};
```

`BlockArena` is today's `DynamicArena` minus location handles: geometric
blocks, bump allocation, `clear()` keeps the first block.

`ctx[site]` is filled once at state construction with `n_in`, `variant`,
`idata`, `n_idata`, `udata`, `in[k].len`, `out.len`, `out2.len`, and
`in_adj[k].len`. Forward and backward only store pointers into it.

## Forward

1. Release the previous tape: clear arena, versions, handles, undo, records,
   target_refs, owner; `adjoint_size = 0`; `reverse_ready = false`.
2. Allocate the initial block (`initial_size`), copy fills and imports into it
   (imports are snapshotted as today), and create one inactive version per
   slot pointing into it. Import versions carry the import marker when the
   outer `in_adj` is non-null. `bindings[slot]` = that version.
3. Create the Transient versions (pointing into `workspace`) and store their
   indices in `node_version[site]`; reset `node_generation` to -1 and
   `loop_generation` to 0.
4. Walk the tree:
   - Sequence: children in order, propagate Break/Continue.
   - If: `value(condition) != 0 ? child 0 : child 1`.
   - For: validate integer bounds as today; `++loop_generation[loop]`; per
     iteration allocate one arena double for the iterator, create an inactive
     version, bind it, run the body; Break exits, Continue proceeds.
   - While: `++loop_generation[loop]`; loop { children[0]; if
     `value(condition) == 0` break; children[1] (Break exits) }. No capacity.
   - Break/Continue: return the flow.
   - Target: `target_refs.push_back(bindings[src])`.
   - Alias: `bindings[dst] = bindings[src]; owner[bindings[src]] = -1`.
   - KernelCall, `invariant_loop >= 0` and `node_generation[site] ==
     loop_generation[invariant_loop]`: rebind `out`/`out2` to the cached
     versions and return.
   - KernelCall Transient: bind ctx input pointers from `bindings`, out/out2/
     scratch into the workspace, call forward, bind outputs to the node's
     fixed versions.
   - KernelCall Retained: append `n_in` handles; allocate `out.len + out2.len
     + kernel_scratch` from the arena; bind pointers; call forward; create
     output versions (adjoint: if `active`, reserve `len` from `adjoint_size`,
     else -1); if `active`, push a Kernel record.
   - KernelCall InPlace: `V = bindings[B]`. If `owner[V] != B`: allocate a
     copy V' of V (same length) with `owner[V'] = B`, adjoint reserved if V is
     active or rhs is active, push a Copy record {V, V'}, rebind B to V'. Then
     if the version is inactive and rhs is active, reserve an adjoint for it
     (promotion). Compute the selected positions from the selector inputs
     with the existing `IndexRuntime` (factor the position walk out of
     `set_index_forward`), append `(position, old value)` pairs to `undo` in
     write order, write the rhs elements, push an InPlace record. `bindings[B]`
     is unchanged.
   - After any KernelCall that has `invariant_loop >= 0`, record
     `node_generation[site] = loop_generation[invariant_loop]` and the output
     versions in `node_version`/`node_version2`.
5. Copy `bindings[out]` values for `outputs` into `ctx.out`. If `has_target`,
   reduce the target leaf values with the six-way grouping used today
   (`dynamic_loop_forward`'s non-fragment branch) into the last output.
6. `reverse_ready = true`.

A kernel that throws propagates unchanged; the next forward releases the
partial tape. Reentrancy is not supported (nested OP_LOOP is refused at
prepare).

## Backward

1. Require `reverse_ready`, clear it. `adjoints.assign(adjoint_size, 0)`.
2. Seed: for each output slot, `adj(bindings[slot])[i] += out_adj_vec[pos+i]`
   when the version has an adjoint; for the target, every leaf in
   `target_refs` gets `+= out_adj_vec[last]`.
3. For records in reverse:
   - Kernel: bind ctx from the saved handles (value pointers, adjoint
     pointers or null), out/out2 values and adjoints (`out_adj` scalar when
     `out.len == 1`, `out2_adj`), scratch; call backward.
   - InPlace: for k from count-1 down to 0: `if (adj_base) { if (adj_rhs)
     adj_rhs[k] += adj_base[pos_k]; adj_base[pos_k] = 0; }
     base.value[pos_k] = old_k`.
   - Copy: `if (adj_from && adj_to) adj_from[i] += adj_to[i]`.
4. Release the tape.

`adj(version)`: `adjoint >= 0` → `adjoints.data() + adjoint`; `-1` → null;
import → `outer.in_adj[imports[k].input].data + imports[k].offset` (null when
the outer adjoint is null).

## Compiler changes (`lower_structured_loop.inc`, `lower.cpp`)

- `While` no longer requires `region_while_capacity`. Keep the counter range
  inference it performs when the proof succeeds; when it fails, lower the
  loop anyway with no range for the counter. Delete
  `positive_when_guard_reached` (the `x / ceil(x / scale)` idiom). If
  `real_ranges` / `region_real_range` / `region_enumerated_data_range` /
  `region_real_definitions` have no remaining consumer after that, delete
  them too.
- `For` inside a region no longer fails when `region_range` of a bound is
  unknown; the zero-trip and one-trip special cases apply only when both
  ranges are known. `loop.capacity` is gone.
- `try_lower_region`: `Import.active` from the parent `Val::autodiff`; the
  region output is `outputs` then one target scalar; the target term is an
  `OP_INDEX` of that scalar pushed to `target_terms`. Delete
  `target_fragments`, `reduce_target_sources`, `structured_history_used`,
  `STANLI_STRUCTURED_HISTORY_BYTES`, `region_physical_memory_bytes`, the
  `dynamic_history` selection, and the `scratch_bytes` diagnostic field
  (print `nodes=... kernels=...` only).
- `lower.cpp::run`: drop the fragmented-target branch.
- `optable.hpp`: remove `OP_TARGET_REDUCE`.

The selector (`region_auto_profitable`, `region_cost`, `StructuredMode`) is
unchanged in this step.

## Tests

`tests/test_structured_loop.cpp` replaces `test_structured_loop_production.cpp`.
Port every test that asserts semantics: values and gradients against the
unrolled path or a hand computation, refusal and rollback, selection under
auto/prefer/force, direct-index descriptors, zero/one/many trips, nested
for/while/if, break/continue, alias and copy-on-write behaviour, failure
then retry, two executors sharing a graph, effects. Drop tests that assert
plan internals, environment ablation switches, memory-profile output,
`scratch_bytes`, handle bands, or rectangular sizing. Add programmatic-plan
tests for: Transient classification (a compare feeding an If is Transient;
a compare feeding an Alias is not), invariant reuse for an active kernel
(its backward runs once and the gradient matches recomputation), InPlace on
an import base (copy then mutate; parent value untouched), InPlace with an
inactive base and active rhs (promotion), duplicate positions in one update
(last write wins, earlier rhs adjoints are zero), and a container read by an
active kernel before an in-place update (LIFO undo restores the value seen
by that kernel's backward).

Retained-vs-unrolled comparisons of `lp` and gradients use a relative
tolerance of 1e-12: the region sums its target leaves with its own six-way
tree, so bitwise parity with the unrolled reduction is not expected.

## Addendum: memoizing data-only subtrees across evaluations

Measured after the rewrite on ctsem N=33: 11.1M kernel calls per gradient, of
which 10.8M are inactive and 9.7M Transient. The active work is 210K records
and 127K in-place updates. Almost all of the inactive work is data-only
bookkeeping (`for (ri in 1:size(ms))` scans with a `while (whenyes == 0 ...)`
inside, row-boundary searches) whose values and control are identical in every
evaluation because they depend on data only. The unrolled path folds all of
it at compile time; the retained loop re-executes it per gradient, and the
one version per loop iteration and per persistent inactive value is where the
432 MB (vs 86 MB before the rewrite) went.

Fix: memoize whole data-only subtrees by visit ordinal, persisting across
evaluations in the executor state.

### Static analysis (prepare)

- `Import.data_only` comes from the parent `Val::si.param_free`.
- `param_dep[slot]` fixed point over the tree, independent of `active`:
  imports start at `!data_only`; every KernelCall (backward or not) makes
  `out`/`out2` param_dep if any input is; Alias propagates src to dst; InPlace
  makes the base param_dep if the rhs or any selector is. Control dependence:
  a write (kernel out/out2, alias dst, in-place base, for iterator) executed
  under any enclosing If/While whose condition slot is param_dep, or For whose
  bound slots are param_dep, makes the written slot param_dep.
- A node is `memo` when it is a maximal such node: it is a For, While, If, or
  a Sequence created by grouping a maximal run of consecutive KernelCall/Alias
  children of one Sequence, and inside it every KernelCall has all inputs
  data-only and is not `is_effectful_op`, there is no InPlace, no Target, no
  Break/Continue that would leave the node (a Break/Continue whose target loop
  is inside the node is fine), every If/While condition and For bound is
  data-only, and every enclosing If/While/For up to the root is data-only
  controlled. Do not mark a child of a memo node.
- `memo_outs`: the slots written inside the node (kernel out/out2, alias dst,
  for iterator) that are read anywhere outside it (kernel input, alias src,
  control, target, output). `memo_index` numbers memo nodes densely;
  `memo_count` goes on the payload.

Group runs before classification so the tree seen by the executor has the
grouping Sequences. A run of length one is still grouped (a lone data-only
kernel whose result feeds a parameter-dependent branch is worth skipping).

### Executor

`LoopState` gains `std::vector<std::vector<double>> memo_tape` (one per memo
node, entries of `memo_outs` total length, persistent across evaluations),
`std::vector<int64_t> memo_ordinal` (reset per forward), and `bool
memo_ready` (false until one forward completes; never cleared afterwards, a
failed later forward does not invalidate data-only values). `release()` does
not touch the tapes.

Forward, on reaching a memo node m: `k = memo_ordinal[m]++`.
- If `memo_ready`: require `k < entries(m)` else throw
  `logic_error("structured memo trace mismatch")`; for each live-out slot
  `bindings[slot] = make_version(tape[m].data() + k * total + offset, -1)`;
  return Normal without visiting children.
- Else: execute the node normally; afterwards append the live-out values
  (`value(slot)`, `len` each) to `tape[m]`. Pointers into `tape[m]` are only
  handed out once `memo_ready`, so growth during the first forward is safe.

Set `memo_ready = true` at the end of a successful forward. Memoized nodes
create no records, so backward is unchanged.

### Expected effect

ctsem N=33: the `while (whenyes ...)` loop (4.8M calls) and the per-row
condition cones (5 kernels x 700K visits) become one ordinal load and one
version per visit. Kernel calls should drop from 11.1M to well under 2M and
RSS back below the pre-rewrite 86 MB.

## Addendum 2: replaying data-only control decisions

After subtree memoization ctsem N=33 still performs 8M memo restores per
gradient (66 MB of tape). They come from loops whose body is a data-only guard
followed by an active arm, `for (ri in 1:size(ms)) if (m == ms[ri,7] && ...)
{ ... }`: the guard cone is memoized, but its result is a live-out because the
`If` reads it, so every visit restores a value and creates or moves a version.
The unrolled path keeps only the iterations whose guard is true.

The decision itself is data-only, so record it instead of the value, and
record which iterations of a data-only loop do anything at all.

### Static

In `group()`, when a node is reached with `ok == true` (every enclosing
If/While/For is data-only controlled) and is not memoizable as a whole:

- An `If` with `!controlled(n)` gets `trace = true`.
- A `For` with `!controlled(n)` whose body contains no parameter-dependent
  control (every If/While/For under it is `!controlled`) gets `trace = true`.
  The restriction keeps the set of effective iterations independent of the
  parameters.
- A `While` with `!controlled(n)` whose condition child `children[0]` is
  memoizable, and whose slots written there have no consumer outside it other
  than the loop's own condition read, gets `trace = true`; replay skips
  `children[0]` entirely.

Reads of `n.condition` (or the For bounds) by a traced node are not outside
reads in `number()`, so a guard or bound cone whose only consumer is a traced
node has no live-outs. `group()` merges any run of consecutive memoizable
children, not just KernelCall/Alias, into one memo node: ctsem's `&&` guards
lower to chains of data-only `If`s that would otherwise each restore a
live-out for the next. `trace_count` on the payload counts traced nodes.

### Executor

`LoopState` holds one sequential control trace, `std::vector<int64_t> trace`
with a read position reset every forward, filled while `!memo_ready` and read
afterwards. Replay order equals recording order because every traced decision
sits under data-only control. A mismatch throws
`logic_error("structured control trace mismatch")`.

An effects counter `effects` is incremented by everything observable: a kernel
call that actually runs (Transient, Retained or InPlace), an Alias, a Target,
Break, Continue, and a memo node with live-outs whether recording or
restoring. Work inside a memo node without live-outs is not observable: its
run during recording leaves the counter unchanged, and replay skips it.

Recording:

- Traced `If`: append the arm taken.
- Traced `While`: append a placeholder at entry, run as usual counting body
  executions, patch the count in at exit.
- Traced `For`: append a placeholder; for every iteration remember the trace
  length and the effects counter, append the iterator value, run the body,
  and if the effects counter did not move truncate the trace back to the
  remembered length (dropping the iteration's own inner decisions), else count
  it effective. Patch the effective count in at exit. Break is an effect, so
  the breaking iteration is kept.

Replay:

- Traced `If`: read the arm and run that child without reading the condition.
- Traced `While`: read the count `c` and run `children[1]` `c` times without
  running `children[0]`; Break inside the body still exits.
- Traced `For`: read the effective count, then for each read the iterator
  value, bind the iterator (cell or fresh version as today) and run the body;
  the bounds are not read.
- Memo node with no live-outs: return `Normal` immediately without touching
  its ordinal.

### Measured effect (ctsem N=33)

Before: `memo_restores=8051046 memo_tape=8198097`, 88 ms per gradient, 298 MB.
After: `memo_restores=95841 memo_tape=176387 traces=495 trace=648813
visits=1988500`, 27 ms per gradient, 188 MB. Parity with main is bitwise at
two points and across six replayed points in one process.

## Addendum: selector and the Prefer flag

### What the measurements say

- Retaining a loop whose body carries control the unrolled path cannot fold is
  a large win at every size: a `while` inside a `for` is 1.3x faster retained
  at N=100 and 57x at N=10k; a parameter-dependent `if` inside a data loop is
  3.5x faster at N=100 and 22x at N=1000. The legacy path emits one island
  per iteration and the islands' reverse pass is quadratic.
- Retaining a loop the unrolled path can vectorize (`target += normal_lpdf(y[i]
  | ...)`, `y_hat[i] = a[county[i]]`) loses 15-60x even after the rewrite,
  because reroll turns it into one vector kernel.
- Retaining a plain recurrence with no control loses 1.4-1.8x on dispatch.
- Under `STANLI_STRUCTURED_LOOPS=1`, `structured_enabled()` also changes nine
  non-loop lowering decisions in the parent; that path crashes accel_gp
  (`runtime-control region: function dot_self`) because `needs_runtime_value`
  treats `rows(lscale)` as runtime when static evaluation would have folded
  it.

### Selector

`region_auto_profitable` becomes:

- the statement is a `For` at outer depth 1 with exact bounds and at least 32
  trips, or a `While` at outer depth 1;
- `region_runtime_control(s)`: the body, transitively through UDF bodies
  (bounded depth, visited set), contains a `While`, an `IfElse` whose
  condition is not `data_only`, or a `TernaryIf`/`EAnd`/`EOr` expression whose
  condition is not `data_only`.

The `region_cost >= 1e6` gate and `region_contains_while` are deleted. After
the trial lowers, the candidate is kept only if its tree still contains an
`If` or `While` node (specialization may have folded the hazard away).

`region_unroll_profitable`, Prefer and Force keep their meaning for testing.
`structured_target_sites` and `region_target_count` are dead and deleted.

### Parent lowering

The nine `structured_enabled()` sites in `lower.cpp` outside a region become
mode independent: the runtime-value path is taken only when
`needs_runtime_value(e)` and `try_eval_pure(e)` fails (or, for the `For`
bounds, `eval_int` would fail). The island `param_free` refinement at the
`OP_ISLAND` emission site is removed; islands keep the legacy dependence.
`structured_enabled()` then reads `region_current != nullptr` and the
`Prefer`/`Force` clauses go away.

Gate for the change: the 130-model corpus census under `STANLI_STRUCTURED_LOOPS=0`
and unset must produce byte-identical graph dumps for every model that
compiles today, and accel_gp must compile under `=1`.

## Addendum: straight-line segments run on the register machine

Profile of ctsem N=400 after the previous steps (337 ms per gradient): 42% of
samples are the tree walk itself (`Execution::run`/`forward`), 17% the dynamic
index kernels' per-call validation, under 10% Eigen. The body executes 23M
tree nodes per gradient for 3.9M tape records. A scalar recurrence retained
under `=1` pays ~25 ns per kernel round trip against the flat executor's 13.

The repository already has the fix for scalar-heavy straight-line code: the
register `Program` (program.hpp) with its generated reverse pass
(adjoint.hpp), which the island carver builds from graph ops. A run of
kernel calls in a body becomes one program: scalar ops become register
instructions dispatched by a switch, matrix ops stay `CALL`s, aliases become
register renames, and the whole run costs one arena frame and one tape
record instead of one record, one version and up to six handles per kernel.
This program stream is also the input a stencil JIT would compile.

### Compiler side (`island.cpp` / `island.hpp`)

```cpp
struct SegmentItem { int op = -1; int alias_dst = -1, alias_src = -1; };
struct SegmentBinding { int slot = -1; int reg = 0; int len = 0; };
struct Segment {
  IslandProg program;            // forward code, calls, pool, adj, n_regs
  std::vector<SegmentBinding> ins, outs;
};
bool compile_segment(const Graph& g, const std::vector<SegmentItem>& items,
                     const std::unordered_map<int, const std::vector<double>*>& constants,
                     const std::vector<int>& live_outs,
                     const std::vector<char>& slot_active, Segment* out);
```

Reuses the carver's `Compiler` over the items in order: an op item goes
through `Compiler::compile` (with no last-use aliasing, so `base_dead_here`
is false), an alias item sets `reg_of[dst] = read_reg(src)`. Unseen slots
read are live-ins (`cc.live_in_slots`, any number; the six-input limit does
not apply because the loop executor seeds registers itself); slots in
`constants` are absorbed. `live_outs` are the slots the caller says are read
after the run; their registers, flattened, become `program.out_regs`. Then
`compact_island_gated(program, false)`, `gen_adjoint`, and `native_adj` is
required. Refuse (return false) when any op is outside the vocabulary
(`in_vocab`, which also excludes propto densities and effect kernels), when
the adjoint is refused, or when a live-out's adjoint cells
`adj.adj_reg[reg .. reg+len)` are not contiguous (a version needs one
adjoint range). `ins[k].active` comes from `slot_active`.

### Payload and prepare

`StructuredLoop` gains `std::vector<Segment> segments`; `Node` gains
`kind = Segment` and `int segment = -1`. As the last step of `classify()`,
after memo grouping and transient classification, every non-memo Sequence
is scanned for maximal runs of consecutive children that are either an
Alias or a KernelCall with `storage != InPlace`, not
`(invariant_loop >= 0 && active)`, and not `is_effectful_op`. A run with at
least two kernel ops is offered to `compile_segment` with live-outs = slots
written in the run (kernel out/out2, alias dst) that are read anywhere
outside it (kernel input, alias src, control, target, output; the SlotUses
counts minus the run's own reads). On success the run is replaced by one
Segment node (`active` = any live-in active, given a `site`). On failure the
run stays as it is. Nodes inside memo subtrees are left alone.

### Executor

- Forward: `frame = arena.allocate(n_regs)`; for each `in`: copy `len`
  values from `value(slot)` into `frame + reg`, and if the segment is
  active push the live-in version handle; `run_program(program, frame,
  eval_state)`; if active reserve `program.adj.n_regs` adjoint cells at
  `base`; for each `out`: `bindings[slot] = make_version(frame + reg, active
  ? base + adj_reg[reg] : -1)`; push `Record{Segment, site, handles,
  frame_version, base}` where `frame_version` is an inactive version created
  for `frame`. `++effects`.
- Backward: `run_adjoint(program, program.adj, frame, adjoints + base)` (the
  file was zeroed by `assign` and already holds the live-out seeds through
  the versions' offsets); then for each active live-in with a non-null
  `adj(handle)`: `dst[i] += file[adj_reg[reg + i]]`.

### Tests

Programmatic plans in tests/test_structured_loop.cpp: a three-kernel scalar
recurrence body becomes one Segment node (assert `segments.size() == 1`,
tree has one Segment child) with values and gradients equal to the
unsegmented plan at two parameter values; a run mixing a matrix kernel (CALL)
and scalar ops; an alias inside the run whose cell is a live-out; a run
whose live-out is read only in the next iteration (loop-carried through the
cell); a run containing an in-place update splits into two segments around
it; a run with a lone kernel op stays a KernelCall; an effect kernel stays a
KernelCall; a refused adjoint (a kernel with no backward feeding an active
consumer, or an op outside the vocabulary) leaves the run as KernelCalls.
The fixture-based retained-vs-unrolled comparisons keep passing at 1e-12.

### Expected effect

m1 (scalar recurrence, N=10k, `=1`): from 836 µs toward the unrolled 404 µs
or below, since the three kernels become three register instructions. ctsem
N=400: fewer tree visits and records; the CALL-heavy Kalman step gains less.

## Addendum: the JIT, staged

Date: 2026-09-03. Status: proposal, not started. Depends on the segment
addendum (bodies as `Program` streams) being in.

### What is left to gain, and where

Measured after segments on the scalar recurrence (m1, N=10k, 462 µs per
gradient against 399 µs unrolled): libm tanh/cosh 26%, Program interpreter
forward plus generated adjoint 29%, tape bookkeeping (tree walk, frame
allocation, versions, records, the reverse record loop) 41%.

After segments, a retained iteration costs: the tree walk over the body's
few remaining nodes, one arena frame per segment, live-in copies, one record
per segment, `run_program` (switch dispatch, ~3 ns per scalar instruction,
a kernel call per CALL) and `run_adjoint` (same shape backwards). For a
scalar recurrence the switch and the per-iteration bookkeeping are the same
order of magnitude; for a matrix body the kernels dominate and nothing below
changes them. A JIT that only replaced `run_program` would therefore buy a
fraction of the remaining time. The design below compiles the loop, not the
instruction.

### Stage 1: one frame per iteration

A For or While whose body, after segment formation, contains only Segment
nodes, Alias nodes, traced Ifs and memo nodes (no nested loop, no InPlace)
gets a fixed frame layout: the segments' register files at fixed offsets,
one control word per traced If, the iterator, and the live-in handles the
active segments save. The loop allocates one arena block per entry sized
`trips * stride` for a For (a While grows in blocks) and the backward walks
frames arithmetically instead of through records: one record per loop
entry. The segments' `run_program` calls then read and write inside one
contiguous frame with static offsets, which is what a stencil needs.

This is portable and needs no new machinery; it removes the per-segment
records and most version traffic for the common body.

### Stage 2: copy-and-patch stencils for Program streams

The `Program` opcode set is closed (about 35 codes plus CALL) and the
generated adjoint is a second stream over the same codes. Each opcode gets a
stencil: a C function over `(double* frame)` whose register offsets,
constants and call targets are holes, ending in a tail call to the next
stencil. Compiling a segment concatenates the stencils for its forward
stream, patches the holes with the segment's register offsets, and does the
same for its adjoint stream. Kernel CALLs and libm calls stay calls; scalar
arithmetic, moves, comparisons and densities' partial rules become straight
machine code. A frame-relative addressing mode means one segment compiles
once and runs at any frame address.

What this needs:

- Stencil sources: one `stencils.c` compiled per target with a fixed calling
  convention and `musttail` continuations. Generated at build time only by
  developers: a `tools/gen_stencils.py` runs clang and `llvm-objdump` and
  writes `runtime/jit/stencils_<arch>.inc` (bytes plus relocation lists).
  The tables are checked in, the way fixture MIR is, and a CI job with LLVM
  regenerates and diffs them. Ordinary builds, wheels and R packages need
  no LLVM.
- A patcher per architecture: x86-64 (abs64, rel32) and arm64 (BRANCH26,
  ADRP/ADD page pairs, MOVZ/MOVK immediates), roughly 150 lines each.
- Executable memory: `mmap(MAP_JIT)` plus `pthread_jit_write_protect_np`
  and `sys_icache_invalidate` on macOS, `mprotect` on Linux, `VirtualAlloc`
  on Windows; about 80 lines.
- Fallback: `STANLI_JIT=0`, any unsupported opcode, any unsupported
  platform (wasm) runs the interpreter over the same Program. The JIT is
  never required for correctness, and every JIT'd segment is verified
  against the interpreter bitwise in the test suite, opcode by opcode.

Expected: scalar-dominated bodies 3-5x over the interpreter; CALL-dominated
bodies unchanged. Cost: about 1,500 lines plus generated tables and a
dev-only LLVM dependency.

### Decision points for the owner

1. Is a checked-in generated table with a dev-only clang/llvm-objdump
   requirement acceptable, or must the build stay pure CMake+C++?
2. Which platforms first: arm64 macOS (development machine), then x86-64
   Linux (CI), Windows later, wasm never.
3. Whether Stage 1 alone (portable, no machine code) is where to stop if the
   measured gap after segments is mostly bookkeeping rather than dispatch.

## Addendum: per-iteration frames for flat loops (JIT stage 1)

After segments, the scalar recurrence spends 41% of a gradient on tape
bookkeeping: a frame allocation, live-in copies, versions, a record per
segment per iteration, and the reverse record loop. All of it is dynamic
storage for something that is static: in a loop whose body has no nested
loop and no parameter-dependent control, every value has a fixed place in
an iteration's frame, and every active input of every node comes from a
statically known place: the same frame, the previous iteration's frame, or
a value that entered the loop.

### Flat loops

A For or While node L is flat when `escape[L]` is false and its body,
walking Sequences but treating memo nodes as opaque, contains only:

- Segment nodes;
- KernelCall nodes with storage Retained or Transient, not
  `invariant_loop >= 0 && active`, not `is_effectful_op`;
- Alias nodes;
- Target nodes;
- memo nodes (any content; they run or replay as today);
- traced If nodes whose arms, recursively, contain only Alias nodes of
  slots with `param_dep == false`, memo nodes, and such Ifs.

No nested non-memo For/While, no InPlace, no Break/Continue, no untraced If.
A loop that is not flat keeps today's path. `STANLI_NO_STRUCTURED_FRAMES=1`
disables frame planning for A/B tests.

### Frame plan (prepare)

Walk the flat body in order with a definition table `def[slot]` for the
slots written by Segment and KernelCall nodes and Aliases of them:

- Segment out `(slot, reg)`: `def[slot] = {Same, seg_base + reg, seg_adj_base
  + adj_reg[reg] if active else -1}`.
- KernelCall out/out2: the node gets a frame region `[out | out2 | scratch]`
  at `kernel_base`, and adjoint cells for out/out2 when active;
  `def[out] = {Same, kernel_base, kernel_adj_base}`.
- Alias `dst = src` outside any If: `def[dst] = def[src]` (a rename; nothing
  runs at execution time for it), unless `src` has no entry in `def`, in
  which case `def[dst]` is cleared (dst now names an outside value).
- The For iterator: one frame cell, `def[iterator] = {Iterator, iter_off}`.

The frame `stride` is the sum of the regions plus the iterator and one
control word per traced If in the body; `adj_stride` is the sum of the
adjoint regions.

Each node input (Segment live-in, KernelCall input) resolves to a
`Provenance {kind, value_off, adj_off, slot}` at plan time using the table
as it stands at that node: `Same` if `def[slot]` was written earlier in this
iteration; `Previous` if the slot is written somewhere in the body but not
yet at this point (its value is the previous iteration's final definition:
resolve against the table as it stands at the END of the body; for the first
executed iteration the entry binding is used); `Outside` otherwise (the
binding as it stands when the node runs; the value pointer is saved in the
iteration's pointer area and the adjoint, if the entry binding is active,
goes to the version captured at loop entry). Data-only inputs never need an
adjoint; `Iterator` reads the frame cell.

Slots written in the body and read by anything that reads bindings at run
time (Alias inside an If, memo nodes, If conditions, Target, and the code
after the loop) get a `moving` version: one Version per such slot created at
loop entry whose `value` pointer is updated after its writer runs each
iteration (adjoint -1 while moving). After the loop, every written slot is
rebound to a fresh version at its final definition in the last executed
frame with the matching adjoint cell, so outside consumers and the reverse
seeding work exactly as before. A zero-trip loop leaves bindings unchanged.

### Executor

- Loop entry: capture entry handles (`bindings[slot]` for every slot some
  input resolves to `Outside` or `Previous`), push them to `handles`; record
  the first frame index in `frames` (a `std::vector<double*>` in LoopState)
  and the adjoint base; create the moving versions.
- Each executed iteration k: `frame = arena.allocate(stride)`,
  `frames.push_back(frame)`, `reserve_adjoint(adj_stride)`, write the
  iterator cell; then the body in order: Segment copies its live-ins from
  their provenance and runs `run_program` in place; KernelCall binds its
  prebound ctx pointers (inputs by provenance, Outside pointers saved into
  the iteration's slot of `outside_ptrs`, outputs/scratch in the frame) and
  calls forward; Alias does nothing; Target pushes a version for the leaf
  at its frame cell; memo/If nodes run as today with control words written
  to the frame; after every writer, update its moving versions. Bump
  `effects` per Segment/KernelCall so traced-For iteration skipping keeps
  working (skipped iterations get no frame).
- Loop exit: push one `Record{Loop, site, frames_first, count, adj_base,
  handles_first}` (Record stays 32 bytes: `handles` = entry handles offset,
  `out` = first frame index, `other` = adjoint base; the count is
  `frames.size()` at exit minus the first index, stored by the next record's
  first index or a parallel `loop_counts` vector) and rebind written slots.
- Backward for a Loop record: for k = count-1 down to 0, frame = frames[k],
  adjoint file = adjoints + adj_base + k * adj_stride; visit body nodes in
  reverse; Segment: `run_adjoint(program, adj, frame + seg_base, file +
  seg_adj_base)` then harvest each active live-in into its provenance's
  adjoint (Same: this file; Previous: file of k-1, or the entry version when
  k == 0; Outside: the entry version); KernelCall: bind values and adjoints
  the same way and call backward; Target and Alias: nothing; traced If: read
  the control word and descend the taken arm (only memo/Alias inside, so
  nothing to do beyond descending).

### Tests

Programmatic plans: the three-kernel scalar recurrence as a flat For (assert
one Loop record per gradient via the tape line, values and gradients equal
to the same plan under `STANLI_NO_STRUCTURED_FRAMES=1` at two parameter
values, bitwise); a KernelCall-resident node with a matrix input wider than
16 (m4's shape); a loop-carried cell written by a Segment and read by the
next iteration's KernelCall; a data-only traced If with an Alias in each
arm feeding a Segment live-in; a Target inside the body; a flat While; a
flat traced For with skipped iterations; a zero-trip flat loop; a loop with
a parameter-dependent If falling back; two executors on one graph.
Fixture-based retained-vs-unrolled comparisons stay at 1e-12.

### Expected effect

m1 (N=10k, `=1`): 462 µs toward ~250 µs; m4 gains from the KernelCall-
resident path; ctsem unchanged (its loops nest and are not flat).

### Outcome (2026-09-03): implemented, measured, not kept

The frame path was built as specified (about 900 lines with tests, bitwise
A/B against the record path on every case). It did not move the numbers:
m1 462 -> 456 us, m4 1220 -> 1140, arK 87 -> 80, the rest unchanged. The
profile with frames on shows the step interpreter (`iterate`, `run_steps`,
`reverse_steps`) at 56% of the gradient where the record path had shown
records and versions at 41%: interpreting a static step list per iteration
costs what the records cost. The per-iteration floor is about 12 ns each
way in both designs and only compiled code removes it. The implementation
was dropped from the branch; this section stays as the frame layout a
loop-compiling JIT would consume.

What a JIT of the whole iteration could reach, from the same profile: m1
spends roughly 10 ns per iteration in libm, 13 ns in the Program
interpreter and 20 ns in bookkeeping; compiled, the last two shrink to a
few ns, so about 15 ns per iteration against the flat executor's 40. That
gain applies to loops that are flat, which today excludes ctsem (nested
loops), the corpus recurrences with in-place element updates (hmm, garch,
arma) and any body with an active loop-invariant kernel.

## Addendum: the recording evaluation gives silent subtrees their storage back

Date: 2026-09-03. Branch `feat/loop-next`.

Measured on ctsem N=400 before the change: the recording evaluation allocates
21.8M arena doubles and 15.8M versions against the replayed evaluation's 10.2M
and 5.3M. The whole difference is inside memo nodes, and 10.2M of it is one
iterator cell plus one version per iteration of a data-only `for` whose
iterator some kernel reads by name. Peak RSS is 1.24 GB against a 0.53 GB
steady-state tape.

A memo node with no live-outs is skipped entirely on replay, so what it
allocates while recording has no reader once it exits. Such a node
(`Node::memo_silent`) takes an arena mark and a version count at entry and
rewinds to them at exit. `BlockArena` keeps a cursor and the blocks past it,
so a rewind never frees a block the next visit would have to buy back.

Two things make the release safe:

- A traced `If` or `For` reads its condition or bounds while recording even
  though replay takes the decision from the trace, and `number()` discounts
  exactly those reads when it decides live-outs. Those slots
  (`Node::memo_keep`) move to fixed cells in the state with one version each
  made per evaluation, the way a Transient result does: the reader takes the
  value before the node runs again. Copying them into fresh arena versions
  instead was measured and costs more than the release recovers -- 11.0M kept
  versions against 10.2M freed.
- A node whose subtree contains an active call puts records, handles and
  adjoints on the tape that point into the region; those nodes are not
  released. Cached results of loop-invariant calls inside a released node have
  their generation invalidated.

Result on ctsem: recording arena 21.8M -> 11.5M doubles, versions 15.8M ->
5.6M. Peak RSS at N=400 1.24 GB -> 0.95 GB, at N=33 168 MB -> 142 MB.
Gradient time 307.4 -> 306.0 ms at N=400 and 26.34 -> 26.13 ms at N=33; lp and
gradients are bitwise identical at two points and the 130-model census is
unchanged. The tape diagnostic gained `record_arena` and `record_versions`,
the recording evaluation's peaks.

Not done: releasing memo nodes that do have live-outs. Their live-outs are
already copied to the memo tape, so a release would have to re-materialize
them; measured, that is 1.18M versions freed against 1.97M tape doubles
re-materialized per evaluation, which is a loss.

`BlockArena::grow` is `noinline`. Inlined into `allocate` it cost m1 14% and
ctsem 1%, by displacing the register program interpreter in the same
translation unit.

## Addendum: the dynamic index kernels, measured

Date: 2026-09-03. Branch `feat/loop-next`.

Sampled profile of a ctsem gradient at 400 rows, after the release above
(9,059 samples over 12 s of steady-state evaluation):

| | share |
| --- | --- |
| `validate_index` | 22.1% |
| `Execution::run` | 22.7% |
| `selected_position` | 8.8% |
| `Execution::forward` | 8.5% |
| `run_retained` | 5.0% |
| `index_input` | 4.7% |
| `run_in_place` | 4.4% |
| `structured_loop_backward` | 3.2% |
| `make_version` | 3.1% |
| Eigen | 2.5% |

The index machinery is 41%, not the 17% the earlier profile suggested, and
the tree walk 31%. Two things were wrong with the index path, both fixed:

- `selected_position` called `index_input` once per axis per selected element
  to bounds-check a descriptor index `validate_index` had already checked.
  The selector pointer now rides in `IndexRuntime`, which became one struct
  per axis (extent, count, stride, selector) instead of three parallel runs
  in one block.
- Every check built its message inline. `index_input` was too large to inline
  because of the `std::string` concatenation on its throw path, and
  `validate_index` carried string construction through its whole body. The
  throws moved to `[[noreturn]] noinline` helpers with the same messages.

Together: ctsem 26.4 -> 22.6 ms per gradient at 33 rows and 307 -> 265 ms at
400, radon_county retained 1.02 -> 0.82 ms, m2 1.08 -> 0.95 ms, arK 85 -> 76
us, everything else neutral, all results bitwise identical.

### Flattening nested sequences: measured, not kept

A pass in `classify` that splices a sequence's sequence children into it and
collapses lone-child sequences dropped ctsem's node visits per gradient from
22.38M to 21.77M (2.7%) and moved the gradient not at all (265.6 vs 264.8 ms,
inside the noise). ctsem's tree is already nearly flat after specialization;
the visits are kernel calls and traced control, not sequence nodes. The pass
was dropped. It would have to run before `form_segments` to be worth
anything, and that changes which runs become register programs, which changes
the last bit of a reduction.

### What is left in validate_index

Stubbing out every check in `validate_index` that depends only on the
descriptor and the input lengths -- the input layout, the selector-input
range checks, the shape check, the stride checks, the capacity comparison --
leaves ctsem at 21.4 ms at 33 rows and 251 ms at 400: 5.3% of a gradient.
Those checks are static per site, because the loop executor binds one
`KernelCtx` per site at state construction and the lengths come from the
plan's slots.

Collecting it needs a split of `validate_index` into a shape check over the
lengths and a value check, the first run once at `prepare` against a
length-only context, plus lean kernel entry points chosen per node so the
flat executor keeps checking. The risk is not the plumbing but the partition:
a check on the wrong side either runs never or runs always, and the wrong
side of "never" is a descriptor bug that reaches a kernel. It also moves
descriptor errors from the first forward to `prepare`. Not done here.

## Addendum: candidate coverage, diagnosed

Date: 2026-09-03.

### The corpus refusals are mode-1 only

Under `STANLI_STRUCTURED_LOOPS=1` the 130-model corpus produces 226 candidate
refusals over 20 models, concentrated in seven: dogs and its three variants
(31 each), covid19imperial v2 and v3 (30 each), multi_occupancy (29). Under
the shipping selector (auto) the same corpus produces **zero** refusals: every
loop the selector picks lowers. The refusals are an A/B-mode coverage gap,
not a shipped one.

They are three messages and one cause:

- `prepare_data: unknown variable j (type UInt)` (162): a data expression
  inside the region names an enclosing loop's iterator. The lowering folds
  data expressions with the `prepare_data` MIR interpreter, whose unknown
  variables resolve through `int_env`, the unrolled-loop counter environment.
  A retained loop's iterator is a runtime slot and is not there.
- `size expression needs unknown int i` (58, lower.cpp:1108): a declaration
  inside the region is sized by an expression that needs a loop iterator.
- `runtime-sized structured declaration ... needs a finite capacity` and
  `structured slice needs a fixed lower bound and finite capacity` (1 each).

All three are one gap: **a region cell's capacity has to be a compile-time
constant**, so a local whose size varies per iteration cannot live in one.
Closing it means capacities that are the maximum over the iteration space
(which needs a bound the compiler does not have today) or slots with a
runtime length, and a runtime-int path for data expressions that name an
iterator. That is a large change with no effect on the default selector.

### ctsem's O1 MIR

`ctsem.stanli.mir` (stanc3 `--O1`) refuses the candidate with `structured
assignment changes logical shape: inline_whichequals_inline_vecequals_return_
sym23___sym1104__`: an inlined `which_equal` returns a runtime-sized integer
array, and `region_assign`'s `same_view` requires the cell and the value to
have the same logical shape and slot length. Same gap as above.

The second failure is independent of retained loops. With
`STANLI_STRUCTURED_LOOPS=0` the same MIR fails identically with
`runtime-control region: integer inline_sdcovsqrt2cov_inline_constraincorsqrt1
_d_sym28___sym1405__ is not known at compile time`, from the island region
compiler in lower.cpp:3510. Fixing the candidate would not make ctsem's O1
MIR compile; the O1-budget fallback to O0 MIR is what makes the model work
and it stays necessary until that one is fixed too.

## Addendum: write_array and forward-only regions

Date: 2026-09-03. Status: assessed, not implemented.

ctsem's `write_array` falls back to the MIR interpreter per draw with
`runtime-control region: right-hand side needs too many registers` -- the
island region compiler, not the loop. `try_lower_region` never runs there:
`in_write_array` returns false at its first line, and four more guards in the
region helpers (`lower_structured_loop.inc:1054, 1057, 1080, 1092`) turn off
the runtime-value path, the runtime scalar form, the runtime-value rescue in
call lowering, and `param_free` inside write_array.

A forward-only region is the same tree with no tape: every `Import.active` is
false, `has_target` is false, no adjoint is ever reserved and
`structured_loop_backward` is never called, so the executor needs nothing new.
The memo and trace machinery is a good fit for write_array, which runs the
same data-only work once per draw.

What is not free:

- The four helper guards exist because write_array treats every value as
  parameter-dependent (`si.param_free = false` at 1092) and refuses the
  runtime-value path. Each has to be re-derived for a region rather than
  removed.
- ctsem's transformed parameters are assigned inside the Kalman loop, and
  some of them are the runtime-sized locals the addendum above describes. The
  region would refuse them for the same reason the log_prob candidate refuses
  ctsem's O1 MIR.

So the write_array path depends on the runtime-sized-cell work, and is worth
starting only after it. Left for that change.

## Addendum: region cell capacities revisited from brms

Date: 2026-09-05.

The open item above ("a region cell's capacity has to be a compile-time
constant") was reached again from the other side: two brms 2.23.0 models fail
to compile because a runtime-control region wants a local whose extent is not
a compile-time integer. `normal_time_hom_flex_lpdf`, emitted for `unstr()`
autocorrelation, declares `array[nobs[i]] int iobs` and `array[I-i+1] int
lp_terms` inside a `while (i <= I)`. `log_Z_com_poisson` writes
`log_Z_terms[k]` and reduces `log_Z_terms[1:k]` with `k` advanced by a
parameter-sensitive `while`.

They look like one problem and are two.

The `unstr()` loop is entirely data-determined. `I` is `size(nobs)`, the
skip-ahead is driven by `has_lp`, which is filled from `Jtime` comparisons,
and nothing in the control flow reads a parameter. The region exists only
because `while` had no compile-time form: `for` unrolls against `int_env` and
`while` did not, so the loop became an island and its counter became a
register, and from there every extent that mentions the counter is unknown.
Giving `while` the same compile-time unroll `for` already has removes the
region and with it the whole question of the capacity.

That unroll is gated rather than unconditional. A `while` keeps its loop form
unless its body declares a local whose extent the body itself computes, which
is the only shape with no loop form at all; a data-controlled `while` over a
million trips still lowers as one loop, which is what `whileloop.stan`
asserts. A `while` nested inside one being unrolled unrolls with it, or the
outer counter returns to a register and takes the unroll with it. The unroll
stops after 65536 trips, so a decidable guard that never becomes false fails
with the extent error it would have failed with before rather than running
the compiler forever.

Capacity bounds for the register program were not attempted: the
program addresses registers statically, so `log_Z_terms[k] = ...` has no form
there at any capacity, and a masked reduction over the bound would cost the
bound per evaluation.

Completing the structured-loop route for the `unstr()` loop was measured and
rejected. Closing the two range-proof gaps it needs (an `int_env`-sourced
cell carries a real range but no integer range, and the counter proof refuses
a counter written by a nested `while`) leaves `matrix[nobs[i], nobs[i]] L_i`,
which is a two-dimensional runtime-sized declaration, then `Cov[iobs, iobs]`,
`cholesky_decompose` of a runtime-shaped matrix and a
`multi_normal_cholesky_lpdf` over runtime-shaped stacked vectors. That is a
much larger change than the unroll, for a model the unroll already lowers as
ordinary graph ops.

`log_Z_com_poisson` is the real instance of the open item and stays refused.
Its `while` cannot be unrolled: the condition compares `log_Z_terms[k]` to
`log_Z_terms[k-1]`, both parameter-dependent. Under
`STANLI_STRUCTURED_LOOPS=1` the model does compile, so the executor's runtime
indexing covers the shape, but its answer is wrong, which is why the selector
was not extended to reach it. A reduction over a runtime-length slice ignores
the length and reads the whole capacity:

```stan
functions {
  real series(real theta) {
    int M = 8;
    int k = 2;
    vector[M] t;
    if (theta == 1) return -1;
    t[1] = -theta;
    t[2] = -2 * theta;
    while (t[k] >= t[k - 1] - 1 && k < M) { k += 1; t[k] = -k * theta; }
    return log_sum_exp(t[1:k]);
  }
}
```

With `theta = exp(0.1)` the loop exits at `k = 2` and CmdStan returns
`log_sum_exp([-1.105, -2.210]) = -0.819`. Under `STANLI_STRUCTURED_LOOPS=1`
the same call returns `1.863`, which is `log_sum_exp` over the eight-element
capacity with the unwritten tail read as zero. `num_elements(t[1:k])` returns
8 rather than 2 and `max(t[1:k])` returns 0; `sum` and `min` happen to
survive because zero is their identity here. The slice publishes its live
length in `runtime_dims` (`lower_structured_loop.inc:787`) and the reductions
do not consult it. Fixing that is the prerequisite for offering the selector
to this shape.

## Addendum: what is in the tail of a runtime-length value

A runtime-length value keeps its declared capacity as storage, so every one of
them has a tail past the live extent, and the four writers of that storage do
not agree on what is in it:

- An elementwise kernel with a dynamic-length output writes only the live
  prefix and leaves the tail holding whatever the slot held before.
- `OP_INDEX_DYNAMIC` zero-fills the part of its output the selection does not
  reach, so a gather's tail is zeros.
- A runtime-sized declaration fills its capacity with NaN (an integer array
  with `INT_MIN`), so an unwritten element is loud rather than plausible.
- Nothing rewrites a tail when the extent shrinks between iterations.

So the tail is not a defined value, and no consumer may read it. What keeps
consumers off it is entirely a compile-time property: a kernel that spans a
runtime-length operand must take the extent as an operand of its own, and
`emit_value` refuses to lower any other shape over one. That is why the
refusals matter as much as the forms that work. `append_row` and `head` have
no extent operand and decline; `num_elements` of a rank-two runtime view
declines because a single extent cannot express the leaf width; a full-extent
operand beside a runtime-length one declines because narrowing it to the live
extent would answer a question Stan itself rejects. Adding a form here means
adding the extent operand with it, never widening what may read the tail.
