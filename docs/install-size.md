# Install size

stanli ships as one self-contained shared library: 29.7 MB installed and
10.8 MB compressed in the wheel, measured on the manylinux_2_28_x86_64
artifact.

## What takes the space

Most of the space goes to the densities. A distribution is instantiated
once per activity mask (which arguments are autodiff), twice for propto,
and again for the elementwise form. That is `4 * 2^N` templates per
distribution, about 630 KB of object each. This is the cost of shipping
precompiled math. CmdStan instantiates only the combination that your
model uses, but it compiles each model separately.

`STANLI_SCALAR_DENSITY_LIST` in
[`optable.hpp`](../runtime/include/stanli/optable.hpp) sets how many of
these templates each density gets. The thirteen distributions that models
use most get all of them, less common ones get fewer, and each of the 72
distribution functions gets one instantiation.

The interpreter and NUTS together are about 410 KB. We measured two ways
to shrink the library further and did not adopt them. Dead-code stripping
does not work inside the OCaml object. Compiling stanc3 to bytecode saves
3.7 MB but makes model compilation about 8x slower.

## The browser build

The browser build is a different binary with no embedded stanc3, because
the compiler ships separately as JavaScript. `stanli-compiler.js` contains
stanc3 plus the shared stanli OCaml pipeline and emits portable MIR. It is
2,990,736 bytes raw and 425,026 bytes gzipped in the current measured
build. A page that ships precompiled MIR does not need to download it.

`stanli.wasm` is 5.79 MB raw and 1.52 MB gzipped. Densities make up 55%
of the compressed runtime. We measured this by replacing every density
kernel with a stub and relinking. We built on-demand loading for the
uncommon densities and then removed it. The measurements and the
emscripten limitation that blocks it are in
[density-pack.md](internal/archive/density-pack.md).

## Measuring it

`tools/build_wheel.sh` runs [`tools/binary_size.py`](../tools/binary_size.py)
on the library just before stripping it, and CI publishes the per-symbol
breakdown as the `binary-size` artifact of the manylinux x86_64 wheel
build. The size must be measured at that step. The shipped library
exports a few hundred C ABI names and has no static symbol table, so you
cannot work out per-symbol sizes from the wheel or the runtime tarball
afterwards. To measure locally, run `tools/binary_size.py` on an
unstripped build.
