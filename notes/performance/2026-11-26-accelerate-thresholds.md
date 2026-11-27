# 2026-11-26 · Thresholds for the Accelerate dense kernels

With `STANLI_ACCELERATE` (macOS), `accelerate_dense_ops` gives a matrix
product, matrix-vector product, Cholesky factor or `multi_normal_cholesky`
(data y and mu, active factor, propto) an accelerated opcode when the op's
static sizes reach a threshold (`runtime/src/dense_blas.cpp`). Below it the
op keeps the plain kernel and its results are bitwise those of a build
without the option.

## Rules

| Op | Accelerated when |
| --- | --- |
| `GEMM` (rows x inner x columns) | all three are at least 32 |
| `MATVEC` (data X, rows x columns) | rows >= 256 and columns >= 64, or rows >= 10000 and columns >= 10 |
| `CHOLESKY` | n >= 64 |
| `MULTI_NORMAL_CHOL_LPDF` (the propto, factor-only path) | n >= 64 |
| `MULTI_NORMAL_LPDF` (one vector, active covariance) | n >= 32 |
| `NORMAL_ID_GLM_LPDF` (data `y` and `X`, active `beta`) | rows >= 4096 and columns >= 16 |

Ops inside retained loops (`dyn_lengths`) are never accelerated.

## Sweep

`tools/bench_dense_blas` (target `bench_dense_blas`, excluded from the
default build) times the plain kernel and its accelerated form, forward plus
backward, on one thread, best of 7 rounds of at least
20 ms. Apple M3 Ultra, macOS 26.6.2, revision 8a11fd7d0 plus the sweep
shapes, 2026-11-26, load average 5 to 10 from other work on the 32-core
host. Four runs; the first two lack the shapes added in the last two.

Each threshold is the smallest size at which the accelerated kernel was
faster at every measured size and shape above it, moved one step up where
the ratio dipped near 1.0:

- **gemm:** slower at 8 to 12 cubed (0.77x to 0.87x) and for very thin
  shapes (1000x2x1, 4x4x100, 3x3x1000); 14 cubed is 1.03x, 16 cubed 2.2x
  to 2.9x. Every shape with all three sizes at least 24 is at least 2.9x
  faster.
- **matvec:** Accelerate's `dgemv` loses for short or thin matrices
  (1000x5 to 1000x24 0.82x to 1.09x, 100x24 0.72x, 5x10000 0.31x) and
  wins from 32 columns at 256 rows or more (2.3x), and from 10 columns at
  10000 rows (4.7x). 300x40 was only 1.04x, so the rule asks for 64
  columns (256x64: 2.9x).
- **cholesky:** at most 1.08x up to n = 24, then at least 1.15x from 32.
- **multi_normal_cholesky:** 1.15x at 24, at least 1.21x from 32.

## Inside a model

The single-op sweep overstates the gain for small factors. In `sw_gp`
(Cholesky of n = 40) with a threshold of 32, `STANLI_PROFILE=1` over 20000
gradients showed the accelerated Cholesky 1.1x faster than the plain one,
but `GP_COV`, `GEMM` and `SET_INDEX`, which did not change, ran about 25%
slower, and the gradient was 1.05x slower overall (`bench_grad --timed`, six
alternating runs each). Moving the Cholesky adjoint's work array out of the
arena did not change this. The cause was not established.

The census Gaussian process (`cholesky_decompose` plus `multi_normal_cholesky`
with data y), gradient time plain / accelerated with both thresholds at 32,
six alternating `bench_grad --timed` runs, two sweeps, load average 9 to 11:

| N | 24 (no op accelerated) | 32 | 40 | 48 | 64 | 80 | 96 | 128 | 160 | 256 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| speedup | 0.973x, 0.987x | 1.006x, 1.014x | 0.987x, 0.982x | 1.014x, 1.009x | 1.086x, 1.098x | 1.158x, 1.161x | 1.222x | 1.442x | 1.492x | 1.791x |

Both thresholds are therefore 64. The census product model (two GEMMs) at
N x K, one sweep: 16x16 (below the threshold) 0.988x, 24x24 1.30x, 32x32
1.84x, 48x48 2.21x, 64x64 2.65x, 128x32 2.41x, 512x128 4.03x, With the gemm threshold at 24, `kronecker_gp` (four 30 x 30 x 30 products
among eigendecompositions) was 1.012x slower, four runs each of
`harnesses/corpus_bench.py`, every candidate run slower. Its products ran
2.2x faster (5.9 to 2.7 ms over 300 gradients) while the backward passes of
`EIGENVECTORS_SYM`, `LKJ_CORR_CHOL_LPDF`, `MULT_LOWER_TRI_SELF_TRANSPOSE`
and `CONSTRAIN_CHOL_CORR` ran about 25% slower; their forward passes did
not. The gemm threshold is therefore 32. One untested explanation: results
that Accelerate's matrix unit writes go to L2 and skip L1, so the next op
to read them waits longer. A size threshold cannot rule out this
cost in a model where a small accelerated op sits among much larger plain
ones.

| Kernel | Shape | Plain ns (best) | Accelerated ns (best) | Speedup, lowest to highest run |
|---|---|---:|---:|---|
| gemm | 4x4x4 | 129 | 85 | 1.50x to 1.57x |
| gemm | 6x6x6 | 256 | 158 | 1.61x to 1.66x |
| gemm | 8x8x8 | 334 | 403 | 0.77x to 0.83x |
| gemm | 10x10x10 | 487 | 580 | 0.82x to 0.84x |
| gemm | 12x12x12 | 607 | 723 | 0.83x to 0.87x |
| gemm | 16x16x16 | 1054 | 371 | 2.22x to 2.90x |
| gemm | 24x24x24 | 2643 | 816 | 2.90x to 3.32x |
| gemm | 32x32x32 | 5415 | 961 | 4.57x to 5.64x |
| gemm | 48x48x48 | 16411 | 2874 | 5.66x to 6.02x |
| gemm | 64x64x64 | 38131 | 6024 | 6.23x to 6.51x |
| gemm | 96x96x96 | 114331 | 15012 | 7.47x to 7.86x |
| gemm | 128x128x128 | 266668 | 33233 | 8.02x to 8.11x |
| gemm | 192x192x192 | 846583 | 108992 | 7.67x to 7.99x |
| gemm | 256x256x256 | 1921755 | 250674 | 7.66x to 7.89x |
| gemm | 512x512x512 | 15220062 | 2109211 | 6.83x to 7.33x |
| gemm | 512x128x128 | 1018508 | 140766 | 7.24x to 7.44x |
| gemm | 512x128x512 | 3895036 | 510986 | 7.16x to 7.90x |
| gemm | 1000x10x10 | 28133 | 5653 | 4.91x to 5.09x |
| gemm | 1000x100x10 | 223961 | 37226 | 5.96x to 6.07x |
| gemm | 100x100x100 | 129550 | 25036 | 5.17x to 5.22x |
| gemm | 64x64x4 | 5125 | 1065 | 4.73x to 4.92x |
| gemm | 256x256x8 | 110287 | 18012 | 6.02x to 6.28x |
| gemm | 4096x64x64 | 2263924 | 345751 | 6.22x to 6.94x |
| gemm | 32x32x512 | 80380 | 10690 | 7.52x to 7.83x |
| gemm | 100x2x2 | 625 | 402 | 1.55x to 1.60x |
| gemm | 1000x2x1 | 2151 | 2582 | 0.82x to 0.84x |
| gemm | 2x100x2 | 636 | 440 | 1.45x to 1.47x |
| gemm | 4x4x100 | 866 | 1015 | 0.83x to 0.87x |
| gemm | 8x8x64 | 1321 | 704 | 1.86x to 1.92x |
| gemm | 64x8x8 | 1277 | 371 | 3.36x to 3.51x |
| gemm | 8x64x8 | 1315 | 468 | 2.79x to 2.96x |
| gemm | 100x3x3 | 1019 | 697 | 1.43x to 1.53x |
| gemm | 20x20x2 | 625 | 588 | 1.05x to 1.07x |
| gemm | 16x16x1 | 237 | 203 | 1.15x to 1.20x |
| gemm | 64x64x1 | 2220 | 925 | 2.38x to 2.45x |
| gemm | 3x3x1000 | 9502 | 14736 | 0.64x to 0.70x |
| gemm | 14x14x14 | 869 | 846 | 1.03x to 1.04x |
| gemm | 24x1000x24 | 99092 | 19060 | 5.13x to 5.20x |
| gemm | 1000x24x24 | 96538 | 17137 | 5.55x to 5.70x |
| gemm | 24x24x1000 | 93412 | 23087 | 4.00x to 4.12x |
| matvec | 32x32 | 166 | 271 | 0.53x to 0.63x |
| matvec | 48x48 | 338 | 439 | 0.61x to 0.78x |
| matvec | 64x64 | 581 | 591 | 0.98x to 1.00x |
| matvec | 96x96 | 1254 | 1242 | 0.99x to 1.01x |
| matvec | 128x128 | 2307 | 1172 | 1.71x to 2.04x |
| matvec | 256x256 | 12574 | 3777 | 3.33x to 3.41x |
| matvec | 512x512 | 48498 | 10989 | 4.41x to 4.76x |
| matvec | 1024x1024 | 207907 | 41093 | 4.88x to 5.74x |
| matvec | 2048x2048 | 1208268 | 605516 | 1.92x to 2.34x |
| matvec | 100x5 | 178 | 159 | 1.11x to 1.12x |
| matvec | 100x10 | 220 | 204 | 1.08x to 1.09x |
| matvec | 100x24 | 352 | 464 | 0.72x to 0.77x |
| matvec | 100x64 | 919 | 954 | 0.95x to 0.97x |
| matvec | 1000x5 | 1593 | 1927 | 0.82x to 0.86x |
| matvec | 1000x10 | 1857 | 2203 | 0.84x to 0.89x |
| matvec | 1000x16 | 2460 | 2472 | 0.99x to 1.04x |
| matvec | 1000x24 | 4203 | 4104 | 0.91x to 1.09x |
| matvec | 1000x32 | 5965 | 2591 | 2.30x to 2.37x |
| matvec | 1000x64 | 12386 | 5155 | 2.40x to 2.51x |
| matvec | 1000x100 | 20277 | 5719 | 3.46x to 3.55x |
| matvec | 10000x5 | 17468 | 18455 | 0.94x to 0.95x |
| matvec | 10000x10 | 26620 | 5674 | 4.65x to 4.73x |
| matvec | 10000x20 | 47882 | 10428 | 4.43x to 4.60x |
| matvec | 10000x100 | 247041 | 46852 | 5.14x to 5.39x |
| matvec | 100000x10 | 263581 | 46649 | 5.65x to 5.92x |
| matvec | 5000x24 | 28637 | 6022 | 4.76x to 4.99x |
| matvec | 20x1000 | 3565 | 4462 | 0.77x to 0.84x |
| matvec | 5x10000 | 17481 | 55704 | 0.30x to 0.31x |
| matvec | 256x32 | 1112 | 476 | 2.30x to 2.43x |
| matvec | 512x32 | 3049 | 962 | 3.17x to 3.18x |
| matvec | 2048x32 | 19484 | 3848 | 5.06x to 5.11x |
| matvec | 5000x10 | 13047 | 11493 | 1.13x to 1.14x |
| matvec | 256x64 | 2736 | 953 | 2.87x to 2.89x |
| matvec | 300x40 | 1638 | 1564 | 1.04x to 1.06x |
| matvec | 2000x16 | 5904 | 5640 | 0.93x to 1.08x |
| cholesky | 4 | 200 | 199 | 1.01x to 1.04x |
| multi_normal_cholesky | 4 | 240 | 262 | 0.91x to 0.92x |
| cholesky | 8 | 530 | 498 | 1.04x to 1.07x |
| multi_normal_cholesky | 8 | 441 | 469 | 0.94x to 0.97x |
| cholesky | 12 | 990 | 954 | 1.01x to 1.04x |
| multi_normal_cholesky | 12 | 740 | 724 | 1.00x to 1.05x |
| cholesky | 16 | 1665 | 1662 | 1.00x to 1.02x |
| multi_normal_cholesky | 16 | 1168 | 1086 | 1.07x to 1.11x |
| cholesky | 24 | 3662 | 3503 | 1.04x to 1.09x |
| multi_normal_cholesky | 24 | 2349 | 1979 | 1.15x to 1.20x |
| cholesky | 32 | 7774 | 6166 | 1.26x to 1.27x |
| multi_normal_cholesky | 32 | 4160 | 3189 | 1.27x to 1.30x |
| cholesky | 48 | 16623 | 13213 | 1.23x to 1.27x |
| multi_normal_cholesky | 48 | 9358 | 6812 | 1.37x to 1.41x |
| cholesky | 64 | 29524 | 21934 | 1.34x to 1.36x |
| multi_normal_cholesky | 64 | 18581 | 12944 | 1.44x to 1.46x |
| cholesky | 96 | 66361 | 52299 | 1.25x to 1.27x |
| multi_normal_cholesky | 96 | 46888 | 24911 | 1.85x to 1.91x |
| cholesky | 128 | 125657 | 79267 | 1.55x to 1.59x |
| multi_normal_cholesky | 128 | 95921 | 42177 | 2.20x to 2.27x |
| cholesky | 192 | 316101 | 179315 | 1.75x to 1.81x |
| multi_normal_cholesky | 192 | 255552 | 89465 | 2.79x to 2.92x |
| cholesky | 256 | 662767 | 263884 | 2.46x to 2.57x |
| multi_normal_cholesky | 256 | 579642 | 171128 | 3.21x to 3.45x |
| cholesky | 384 | 1840247 | 590572 | 3.08x to 3.13x |
| multi_normal_cholesky | 384 | 1598885 | 376681 | 4.24x to 4.30x |
| cholesky | 512 | 4169422 | 1267412 | 2.77x to 3.59x |
| multi_normal_cholesky | 512 | 3969536 | 827035 | 4.01x to 4.88x |
| cholesky | 1024 | 27921417 | 7640802 | 3.43x to 3.91x |
| multi_normal_cholesky | 1024 | 30282125 | 4017417 | 6.53x to 7.61x |

## multi_normal and normal_id_glm (2026-11-26, later)

`bench_dense_blas`, two runs, load average 5.8 to 10.7 (the plain and the
accelerated kernel of each size run back to back):

- **multi_normal** (data y, active mu and covariance): 0.81x to 0.99x at
  n = 4 to 12, 1.17x at 16, 1.35x to 1.47x at 24 to 32, 1.30x to 1.65x at
  36 to 64, 2.27x to 2.79x at 96 to 128, 6.3x to 9.0x from 256. The rule is
  n >= 32. Only `ch14_m14_9`, `_10` and `_11` (n = 151) reach it; inside
  those models, six interleaved `bench_grad --timed` pairs each at load
  average 5.0, the gradient is 2.13x, 2.30x and 1.46x faster.
- **normal_id_glm** (data y and X, active alpha, beta and sigma), load
  average 3.6 to 3.9: slower at 919 x 2 (0.68x), 1000 x 10 (0.83x) and
  1000 x 24 (0.82x to 0.87x); 0.99x to 1.03x at 236 x 4, 2000 x 24 and
  10000 x 5; 1.15x at 5000 x 10; 1.31x to 1.39x at 4096 x 16; 2.6x to 3.6x
  at 4096 x 24 and above; 2.1x at 10000 x 10. The rule is rows >= 4096 and
  columns >= 16. In the corpus only `diamonds` (5000 x 24) reaches it, and
  its gradient is 2.53x faster in-model (six pairs, load average 5.0 to 5.2).

## multi_normal with the newer dpotri (2026-11-27)

The multi_normal kernel now calls `dpotri$NEWLAPACK` from Accelerate's newer
LAPACK interface. On one thread that routine is 1.05x to 1.1x slower than
the legacy `dpotri_` at n = 32 to 64, and faster from n = 128 (1.3x at 128,
1.8x at 512). `bench_dense_blas`, Release build, three runs, load average
3.5 to 5.6, plain / accelerated for the whole kernel, forward plus backward:

| n | 16 | 24 | 32 | 36 | 40 | 48 | 56 | 64 | 80 | 96 | 128 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| lowest | 0.88x | 1.07x | 1.18x | 1.09x | 1.18x | 1.30x | 1.37x | 1.36x | 1.52x | 2.01x | 2.76x |
| highest | 0.90x | 1.09x | 1.22x | 1.23x | 1.18x | 1.32x | 1.50x | 1.49x | 1.66x | 2.04x | 2.81x |

The accelerated kernel is still faster at every size from 24, so the rule
stays n >= 32. In the corpus only `ch14_m14_9`, `_10` and `_11` (n = 151)
reach it, so no model moves across it.


<a id="threads"></a>

## Threads and the bits of each result (2026-11-27)

`tools/stress_blas_threads` makes the BLAS and LAPACK calls of every
accelerated kernel with Accelerate's threads allowed and compares each
result bit for bit with the single-threaded call on the same inputs. The
calls have the kernels' transposes, alpha, beta and leading dimensions.
`--kernel-shapes` covers square and rectangular shapes from the thresholds
to n = 4096, the shapes of the corpus models that reach the thresholds
(`nn_rbm1bJ100`'s 60000 x 785 x 100 product, the Cholesky factor at n = 101,
`multi_normal` at n = 151 and `normal_id_glm` at 5000 x 24), and the blocked
Cholesky adjoint, replayed call for call on views into an n x n matrix.
`--sweep` makes the same calls at every square size from the thresholds to
1200, every 16 to 2048 and every 64 to 4096, and at a grid of 2,071
product shapes and 54 matrix-vector shapes. On the M3 Ultra we also ran four
processes at once and `VECLIB_MAXIMUM_THREADS` from 2 to 16, which changes
how many threads Accelerate uses. On the M1 of the CI Mac we ran three
processes at once. The raw output is in
[`data/2026-11-27-accelerate-threads`](data/2026-11-27-accelerate-threads/README.md).

Threaded calls, and how many of them gave bits other than one thread:

| Routine | M3 Ultra, macOS 26.6.2 | M1 (CI, 3 cores), macOS 26.7.2 |
| --- | --- | --- |
| `dgemm` | 3,365 of 5,406,332 differ, up to 5.6e-15 | none of 3,825,171, but it never used threads |
| `dsymm` | 426,200 of 1,980,407, up to 1.8e-15 | 270,971 of 1,399,052, up to 1.2e-15 |
| `dpotrf` | 2,512 of 288,470, up to 7.8e-16 | none of 204,648, but it never used threads |
| `dpotri` (newer interface) | 233 of 129,008, up to 2.9e-17 | 6,556 of 91,650, up to 3.9e-17 |
| `dgemv` | none of 750,216; threads only for X' y at 4096 x 16 | none of 527,734; threads for some tall X' y |
| `dtrmv` | none of 461,250, never used threads | none of 325,868, never used threads |
| `dtrsm` | none of 8,085,161 | none of 5,490,780 |
| `dtrmm` | none of 2,928,152 | none of 1,991,598 |
| `dtrtri` | none of 124,369 | none of 80,650 |

A difference is the largest change of any entry over the largest entry of
the result. None is above rounding level, and every threaded call of a shape
gave the same bits as the other threaded calls of that shape. The
differences start at these sizes in the sweep:

- `dgemm`: 323 of the 2,071 product shapes differ in at least one of the
  three calls of the product kernel. Examples are the two backward calls of
  a 1000 x 100 by 100 x 1000 product and the forward call of a 100 x 1500
  by 1500 x 1000 product. We found no simple rule that separates them, and
  `nn_rbm1bJ100`'s shape gave the same bits. One block call of the Cholesky
  adjoint differs at n = 1015.
- `dsymm` in the Cholesky adjoint: from n = 74 on both Macs.
- `dpotrf`: from n = 1025 on the M3 Ultra.
- `dpotri`: from n = 1090 on the M3 Ultra and from n = 132 on the M1.

`dtrsm` and `dtrmm` used threads only a little: CPU time over wall time was
at most 1.6 for one shape on the M3 Ultra and 1.3 on the M1. `dgemv` used them
only for the transposed product of a tall matrix with at most 24 columns (up
to 1.24 on the M3 Ultra and 1.9 on the M1), and `dtrmv` never did. `dtrtri`
used them on both Macs: up to 20 on the M3 Ultra at n = 4096 and up to 2.8
on the M1. `--tri-range 512 4096` then made `dtrtri` at every n in that
range: on the M3 Ultra in four processes at once and with
`VECLIB_MAXIMUM_THREADS` at 2, 4 and 8 (75,285 threaded calls), and on the M1
in one process (10,755 calls). Every call gave the single-threaded bits.
Apple does not document any of this, so a later macOS can change it.

Before this run, a stress of square matrices only had found differences
only in `dsymm` from n = 512 and `dpotrf` at n = 2048, and none in `dgemm`.
The rectangular shapes and the sweep found the others. On `main`, which lets
any accelerated call use threads while the chains leave cores free, the
results of `dgemm`, `dsymm` and `dpotrf` can therefore change with the
number of chains.

### Threads by default for the routines that keep their bits (not taken)

We tried letting the routines that kept their bits use Accelerate's threads
by default, while the chains leave cores free (branch
`spike/dtrtri-default-threads`, not pushed). `dgemm` is ruled out by the
table above. `dtrmv` never used threads and `dgemv` rarely did. `dtrsm` and
`dtrmm` hardly did, and asking for threads in the Cholesky adjoint made
`examplemodels_gp_fit_multi_output` (n = 101) 1.04x slower (eight
interleaved `bench_grad --timed` pairs). That left `dtrtri`. A Gaussian
process gradient (`cholesky_decompose` and `multi_normal_cholesky`), six
interleaved pairs per size, with `dtrtri`'s threads against one thread:

| n | 64 | 100 | 128 | 256 | 384 | 512 | 768 | 1024 | 2048 |
|---|---|---|---|---|---|---|---|---|---|
| with threads | 1.00x | 1.08x slower | 1.07x slower | 1.03x slower | 1.01x slower | 1.015x faster | 1.02x faster | 1.05x faster | 1.10x faster |

With threads only from n = 512 to 4096, no corpus model changes, because
none has a `multi_normal_cholesky` that large, and the rule would add an
option kind and two thresholds. So every accelerated call stays on one
thread unless the model sets `parallel_blas`.
