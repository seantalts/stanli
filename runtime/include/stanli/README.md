# runtime/include/stanli

Every interface that crosses a translation-unit boundary lives here:
what the `runtime/src/` TUs share with each other, with the kernels,
and with the tools and tests. CMake exposes this directory as the
library's public include path and installs it alongside the built
library, so this is also what a C++ embedder sees.

Three kinds of file, and the reason each is a header:

- **The stable surface.** [`capi.h`](capi.h) is the C ABI the Python,
  R, and wasm wrappers call. It is the only file here with a
  compatibility promise; everything else is internal and moves
  freely.
- **Shared types and entry points.** The IR
  ([`graph.hpp`](graph.hpp): `Slot` + `Op` over flat arenas), the MIR
  types ([`mir.hpp`](mir.hpp)), and one small header per pass or
  subsystem ([`island.hpp`](island.hpp), [`nuts.hpp`](nuts.hpp), ...)
  declaring what its `.cpp` in `runtime/src/` defines.
- **Templates that must instantiate in more than one TU.** The MIR
  interpreter ([`mir_interp.hpp`](mir_interp.hpp)) and the register
  machine ([`program.hpp`](program.hpp)) are templated on the scalar
  and run on both `double` and stan-math's `var`; the recording
  scalar ([`recorder.hpp`](recorder.hpp)) and the X-macro op tables
  ([`optable.hpp`](optable.hpp)) generate code into whichever TU
  includes them. Header-only is not a style choice for these; it is
  what lets one implementation serve every instantiation.

The per-file map with one line on each subsystem is in
[`docs/hacking.md`](../../../docs/hacking.md).
