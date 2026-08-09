# Splitting the browser runtime: what was tried and why it is not in

Densities are 55% of the compressed browser download and most models
use a handful, so loading the uncommon ones on demand is an obvious
idea. It was built, it worked, and it was removed. This records what
was measured, so a second attempt starts from the one thing that
actually blocks it. The code is in the history: the commit that added
`stanli_load_pack`, `stanli_register_kernel_c` and
`tests/test_wasm_pack.cjs`, and the one that removed them.

## The payload

Measured by stubbing every density, cdf and tail kernel and relinking
(macOS arm64, emsdk 6.0.6):

| | raw | gzip |
|---|---:|---:|
| core runtime, plus `multi_normal` and `lkj_corr_cholesky` | 2.26 MB | 0.69 MB |
| everything, as shipped | 5.79 MB | 1.52 MB |

The density surface is 3.53 MB raw, 0.83 MB gzipped. The core cannot be
split.

## Why it is not in

| | gzip |
|---|---:|
| one module, as shipped | 1.52 MB |
| split, `MAIN_MODULE=1`: core + pack | 1.73 + 0.99 MB |
| split, `MAIN_MODULE=2`: core + pack | 1.04 + 0.99 MB |

`MAIN_MODULE=2` is the mode worth having (a core-only model downloads
1.04 MB instead of 1.52 MB), and it cannot load a side module: it
exports only what `EXPORTED_FUNCTIONS` names, and a side module also
needs `__stack_pointer` and the `__cpp_exception` tag, which are not
functions and cannot be named there. The load fails with
`imported mutable global must be a WebAssembly.Global object`.
`MAIN_MODULE=1` exports everything, which is why it works and why it
adds 0.69 MB gzipped: core plus pack is then 2.72 MB against 1.52 MB
unsplit, worse for every model. The blocker is entirely on the
emscripten side; everything else worked.

## What did work, and is worth reusing

- **Dynamic linking is free here.** `MAIN_MODULE=2` with `-fPIC`
  measured 1.05 MB gzipped either way and 449 vs 451 ns/gradient: wasm
  calls already go indirect through a function table, so there is no
  PLT/GOT penalty.
- **A side module can call back into the core** (proven with a `dlopen`
  spike), so a pack can use the core's `register_kernel` and libm
  rather than carrying copies.
- **The trigger is self-correcting.** Dispatch is already a
  function-pointer table, and lowering already fails with
  `opcode not registered: OP_...`: try to compile, fetch the pack on
  that error, retry once. No density-name table mirrored in JavaScript.

## Traps, if this is attempted again

- The pack must use the same exception ABI as the core
  (`-fwasm-exceptions`); mismatched, the load fails on
  `__stack_pointer` in a way that looks like a dynamic-linking bug.
- `dlfcn.h` does not exist on mingw; guard any include of it or the
  Windows wheel stops building, after the merge, on `main`.
- A define selecting which kernels register must reach both the static
  library and the executable; setting only the first makes the loader a
  silent no-op.
- The pack sources must leave the static library only when the split is
  on, not whenever the platform is emscripten, or an unsplit browser
  build fails to link.

## The alternative that needs no dynamic linking

Two prebuilt whole-module bundles, common densities and everything,
selected once the compiled model's op set is known. No PIC, no side
modules, and the worst case is one wasted fetch of the smaller file. A
model needing one uncommon density downloads the whole large bundle,
but there is no emscripten blocker.
