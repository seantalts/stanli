# Splitting the browser runtime: what was tried and why it is not in

Densities are 55% of the compressed browser download and most models use a
handful, so loading the uncommon ones on demand is an obvious idea. It was
built, it worked, and it was removed. This records what was measured, so
that a second attempt starts from the one thing that actually blocks it.

The code is in the history: the commit that added `stanli_load_pack`,
`stanli_register_kernel_c` and `tests/test_wasm_pack.cjs`, and the one
that removed them.

## What the payload is made of

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

`MAIN_MODULE=2` is the mode worth having: a model using only core
densities downloads 1.04 MB instead of 1.52 MB. It does not load a side
module. That mode exports only what `EXPORTED_FUNCTIONS` names, and a side
module also imports `__stack_pointer` and the `__cpp_exception` tag, which
are not functions and cannot be named there. The load fails with
`imported mutable global must be a WebAssembly.Global object`, and adding
`__stack_pointer` to `EXPORTED_FUNCTIONS` is rejected as an undefined
exported symbol.

`MAIN_MODULE=1` exports everything a side module might want, which is why
it works and why it adds 0.69 MB gzipped to the core. Core plus pack is
then 2.72 MB against 1.52 MB unsplit: worse for every model, whether or
not it needs the pack.

The whole question is therefore on the emscripten side. Everything else
worked.

## What did work, and is worth reusing

**Dynamic linking is free here.** `MAIN_MODULE=2` with `-fPIC`, measured
against the same tree: 1.05 MB gzipped either way, 449 against 451
ns/gradient. WebAssembly calls already go indirect through a function
table, so there is no PLT or GOT penalty of the kind native dynamic
linking pays. An earlier version of `docs/benchmarks.md` assumed this cost
would exceed the saving; that was wrong.

**A side module can call back into the core.** A spike loaded one with
`dlopen` and called a function in it that called a function in the main
module. A pack can use the core's `register_kernel` and libm rather than
carrying its own copies.

**The design fits the runtime as it stands.** Dispatch is already
`kernel(opcode).forward`, a function-pointer table, so late registration
only fills in slots. The lowering already fails with
`opcode not registered: OP_...`, so the trigger is self-correcting: try to
compile, fetch the pack on that error, retry once. No list of density
names has to be mirrored in JavaScript.

## Traps, if this is attempted again

- The pack must be compiled with the same exception ABI as the core
  (`-fwasm-exceptions`). Without it the load fails on `__stack_pointer`
  in a way that looks like a dynamic-linking bug and is not.
- `dlfcn.h` does not exist on mingw. Anything that includes it has to be
  guarded, or the Windows wheel stops building. That job runs after the
  merge rather than as a gate, so it fails on `main`.
- A define that selects which kernels register has to reach both the
  static library, where `densities.cpp` lives, and the executable, where
  the loader lives. Setting only the first makes the loader a silent
  no-op that returns success.
- The pack sources must leave the static library only when the split is
  on, not whenever the platform is emscripten, or an unsplit browser build
  fails to link.

## The alternative that needs no dynamic linking

Two prebuilt whole-module bundles, common densities and everything,
selected once the compiled model's op set is known. No PIC, no side
modules, and the worst case is one wasted fetch of the smaller file. It
cannot add to a running module, so a model needing one uncommon density
downloads the whole large bundle, but it has no emscripten blocker.
