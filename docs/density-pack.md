# Splitting the browser runtime: core plus an on-demand density pack

Design notes for loading uncommon densities lazily in the browser. Nothing
here is implemented yet. The measurements and the feasibility spike are
done, and this records them so the work does not start from guesses.

## The problem

`stanli.wasm` ships every density, and a model uses a handful. The browser
pays for all of them before it can do anything.

## What the payload is made of

Measured by stubbing every density, cdf and tail kernel and relinking
(macOS arm64, emsdk 6.0.6):

| | raw | gzip |
|---|---:|---:|
| core runtime, plus `multi_normal` and `lkj_corr_cholesky` | 2.26 MB | 0.69 MB |
| everything, as shipped | 5.79 MB | 1.52 MB |

The density surface is 3.53 MB raw, 0.83 MB gzipped: 55% of the compressed
download. The core cannot be split.

The core figure is from a build with every density body stubbed, so
`STANLI_LITE_LP` had nothing to act on and the number holds for either
setting. The total is the shipped build, which is exact-lp since that flag
went off by default.

## Why dynamic linking is affordable

Emscripten's `MAIN_MODULE=2` with `-fPIC`, measured against the same tree:

| | static | MAIN_MODULE=2 + fPIC |
|---|---:|---:|
| `stanli.wasm` gzipped | 1.05 MB | 1.05 MB |
| fixtures, geomean | 449 ns | 451 ns |

No measurable cost. WebAssembly calls already go indirect through a
function table, so there is no PLT or GOT penalty of the kind native
dynamic linking pays. An earlier version of `docs/benchmarks.md` assumed
this cost would exceed the saving and concluded the split was not worth
doing. That assumption was wrong.

## The mechanism works

A spike built a `SIDE_MODULE` against these headers, loaded it at runtime
with `dlopen`, and called a function in it that called back into the main
module. The call returned the expected value, which means a pack can use
`register_kernel`, `active_sink()` and libm from the core instead of
carrying its own copies.

Three properties of the existing design make this fit:

1. Dispatch is already `kernel(opcode).forward`, a function-pointer table.
   Late registration only has to fill in slots.
2. The lowering already fails with `unsupported function <name>`. The
   trigger is therefore self-correcting: try to lower, fetch the pack on
   that error, retry. No table of density names has to be duplicated in
   JavaScript.
3. `dlopen` and `dlsym` stay inside C. JavaScript only has to put the pack
   where the loader can read it, then call one exported entry point.

**Requirement found the hard way:** the pack must be compiled with the same
exception ABI as the core (`-fwasm-exceptions`). Without it the load fails
with `imported mutable global must be a WebAssembly.Global object` on
`__stack_pointer`, which reads like a dynamic-linking bug and is not.

## Sketch

Core exports one entry:

```c
/* Load a density pack and let it register its kernels. Returns 0 on
   success. */
int stanli_load_pack(const char* path);
```

which does `dlopen(path, RTLD_NOW | RTLD_GLOBAL)`, `dlsym` for
`stanli_pack_register`, and calls it. The pack's `stanli_pack_register`
calls `register_kernel` once per opcode it provides.

On the JavaScript side, `stanli_model_new` fails with the unsupported
function name; the worker fetches `stanli-pack.wasm`, writes it to the
emscripten filesystem, calls `stanli_load_pack`, and retries the compile
once. A second failure is a real error.

## What has to be decided

**Where the line goes.** Which densities are core and which are packed.
The density list only just settled at 71 of 72, which is why this work was
deferred: re-splitting later means re-verifying both artifacts. A starting
point is the tier field in `STANLI_SCALAR_DENSITY_LIST` (tier 3 is the
thirteen distributions models lean on) plus the discrete lpmfs, with the
cdfs and the multivariate tail in the pack.

**How many packs.** One is simplest. Splitting cdfs from the multivariate
tail would save a truncation-only model from downloading wisharts, at the
cost of a second artifact to build, verify and cache.

**Whether the wheel does this too.** Probably not. The wheel is one file
on disk and its size is not on anyone's critical path; the browser's is.

## Alternative, if dynamic linking turns out to be a problem

Two prebuilt whole-module bundles, common densities and everything,
selected once the compiled model's op set is known. No PIC, no dynamic
linking, and the worst case is one wasted fetch of the smaller file. This
was the fallback before PIC was measured. It is strictly worse when
dynamic linking works, because it cannot add to a running module.

## Verification this will need

The corpus replay already runs through the browser build
(`tools/wasm_check.sh` with `tools/verify_refs.py`). A split runtime has to
pass it with the pack loaded, and separately has to fail cleanly with the
pack absent: a model needing a packed density must report the unsupported
function, not compute a wrong answer. Both belong in CI.
