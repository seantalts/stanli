# Embedding the model in the data argument

Status: implemented. Companion to
[2026-08-10-bridgestan-facade-design.md](2026-08-10-bridgestan-facade-design.md),
which built the lib-pair transport this spec adds an alternative to.

## Problem

A BridgeStan client identifies a model by the shared library it loads.
The facade's answer is the lib pair: a full copy of the runtime
(~29 MB), renamed per model, with a sidecar manifest holding the MIR.
`bs_model_construct` finds the sidecar by asking `dladdr` where its own
code was loaded from.

The copy is the cost, and it is not just disk. It exists because dlopen
identifies a library by inode, so the runtime must be a distinct file
per model or every model resolves to the first one loaded. The `dladdr`
trick also has no Windows equivalent in the facade today, so
`bs_model_construct` refuses there outright.

Both problems come from routing model identity through the filesystem.
The construct call already has a channel that arrives at exactly the
right moment and is opaque to every client: the `data` argument.

## Design

The manifest may ride inside the data JSON, under one reserved key:

```json
{
  "__stanli": {"build_id": "...", "mir": "...", "name": "eight_schools"},
  "J": 8,
  "y": [28, 8, -3, 7, -1, 1, 18, 12]
}
```

`bs_model_construct` checks the resolved data text (literal or `.json`
file, same rule as before) for the key. If present, the sub-object is
validated by `bs_read_manifest`, the same function that reads sidecars,
so there is ONE manifest format with two transports. The key is then
stripped and the remaining object is the model's data. If absent,
construction falls back to the sidecar lookup unchanged.

The key can never collide with a data variable: Stan identifiers begin
with a letter.

Malformed is loud, not a fallback. A payload that mentions `__stanli`
but fails to parse, or carries a manifest with the wrong `build_id`,
is an error naming the embedded manifest. Falling back to the sidecar
would mask the typo behind a confusing "no sidecar found" message.

On the Python side, `stanli.bridgestan_model(...)` is the sugar the
facade design doc sketched: compile the model, splice the manifest into
the data, and return a `bridgestan.StanModel` bound to the runtime
library itself. No pair is written. `bridgestan` is imported lazily and
stays out of stanli's dependencies.

## What it provides

- **Zero copies.** Every model shares the one runtime library already
  inside the installed wheel. The per-model cost drops from ~29 MB to
  the size of the MIR text (tens of KB), and it lives in memory, not
  the pair cache.
- **No dlopen aliasing.** Model identity moves from "which file did
  dlopen open" to construct time, so the same library serving many
  models is not merely safe but the point.
- **`bs_model_construct` on Windows.** The embedded path needs no
  `dladdr`, so the one platform refusal in the facade disappears for
  clients that use it.
- **In-process testability.** The sidecar path only works under a real
  dlopen (a linked test resolves the anchor to the test binary), which
  is why the pair test lives in Python. The embedded path is plain
  argument passing and tests in the C++ suite.

## What it requires

- **A cooperating caller.** Someone must splice the manifest into the
  data: `bridgestan_model` does it for Python, and other languages need
  the same few lines. Clients that take only a library path (walnutpie's
  `stan_cli`, the R and Julia packages driving a prebuilt `.so`) cannot
  use it, which is why the pair transport remains.
- **Inlined data.** Path-form data has to be read and re-serialized to
  splice the key in, so the caller-side helper eats the file. The C
  ABI itself still accepts a `.json` path whose contents embed the key.
- **The same build discipline as the sidecar.** `build_id` must match
  the runtime, for the same reason sidecars check it: the MIR dialect
  and the lowering that reads it move together.

## Pros and cons against the pair

Pro: no 29 MB copy, no cache directory to manage, no inode trick,
works on Windows, testable in-process. The manifest travels with the
construct call, so there is no staleness window between writing a pair
and loading it.

Con: it is invisible to path-only clients, needs a per-language shim,
and puts a multi-hundred-KB string through an argument every client
logs or stores at will (a pair keeps the MIR in one file on disk). The
data argument also stops being "just the data": tooling that round-trips
it must preserve a key it does not understand.

The two transports compose rather than compete: embedded first when the
key is present, sidecar otherwise, one manifest format underneath.
