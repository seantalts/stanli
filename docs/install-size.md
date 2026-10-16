# Install size

stanli ships as one self-contained shared library: 29.7 MB installed and
10.8 MB compressed in the wheel, measured on the manylinux_2_28_x86_64
artifact.

## What takes the space

The densities dominate. A distribution is instantiated once per activity
mask (which arguments are autodiff), twice for propto, and again for the
elementwise form. That is `4 * 2^N` templates per distribution, about
630 KB of object each. This is the standing cost of shipping precompiled
math. CmdStan instantiates only the combination your model uses and pays
for it with a per-model compile.

Each density chooses how much of that ladder to instantiate
(`STANLI_SCALAR_DENSITY_LIST` in
[`optable.hpp`](../runtime/include/stanli/optable.hpp)). The thirteen
distributions models lean on take all of it, the long tail takes less,
and the 72 distribution functions take one instantiation each.

The interpreter and NUTS together are about 410 KB. Shrinking further was
measured and declined. Dead-code stripping cannot reach inside the OCaml
object, and compiling stanc3 to bytecode saves 3.7 MB at the cost of
about 8x slower model compilation.

## The lite build

`-DSTANLI_LITE_LP=ON` drops the propto family. The library is 48% smaller
and every gradient stays bitwise, but `lp__` differs from CmdStan's by a
per-model constant. It is off in every shipped build, browser included,
so any run can be compared against CmdStan directly. `stanli_exact_lp()`
reports which build is loaded. See [lp-constant.md](lp-constant.md).

## The browser build

The browser build is a different binary with no embedded stanc3, because
the compiler ships separately as JavaScript. `stanli-compiler.js` contains
stanc3 plus the shared stanli OCaml pipeline and emits portable MIR. It is
2,990,736 bytes raw and 425,026 bytes gzipped in the current measured
build. For one rollback cycle the package also carries stock
`stancjs.bc.js` (2,971,677 bytes raw, 418,847 bytes gzipped), which emits
O1 legacy MIR and loads only if the portable compiler is unavailable. A
page that ships precompiled MIR fetches neither compiler.

`stanli.wasm` is 5.79 MB raw and 1.52 MB gzipped. Densities are 55% of the
compressed runtime payload, measured by stubbing every density kernel and
relinking. Loading the uncommon densities on demand was built and
removed. The measurements and the emscripten limitation that blocks it
are in [density-pack.md](internal/archive/density-pack.md).

## Measuring it

`tools/build_wheel.sh` runs [`tools/binary_size.py`](../tools/binary_size.py)
on the library just before stripping it, and CI publishes the per-symbol
breakdown as the `binary-size` artifact of the manylinux x86_64 wheel
build. It has to be measured there. The shipped library exports a few
hundred C ABI names and carries no static symbol table, so the
attribution cannot be reconstructed from the wheel or the runtime tarball
afterwards. To measure locally, run `tools/binary_size.py` on an
unstripped build.
