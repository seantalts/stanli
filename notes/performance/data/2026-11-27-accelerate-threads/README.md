# Accelerate's threads and the bits of each result

Raw output of `tools/stress_blas_threads` for the "Threads" section of
[the thresholds note](../../2026-11-26-accelerate-thresholds.md#threads).
Each line compares the threaded calls of one routine at one shape with the
single-threaded call on the same inputs. `cpu/wall` is the CPU time over the
wall time of the threaded calls; near 1 means that Accelerate used one
thread. The tool that wrote these files also printed a `one/threaded`
column, which compared one cold single-threaded call with warm threaded
calls; it is biased, and the tool no longer prints it.

| files | machine | command |
| --- | --- | --- |
| `m3-shapes-one-process.txt` | Apple M3 Ultra, 24 performance cores, macOS 26.6.2 | `--kernel-shapes --scale 20` |
| `m3-shapes-four-processes-p1.txt` to `-p4` | the same, four processes at once | the same |
| `m3-sweep.txt` | the same | `--sweep --scale 0.01` |
| `m3-shapes-veclib-threads-N.txt` | the same, `VECLIB_MAXIMUM_THREADS=N` | `--kernel-shapes --scale 2` |
| `m3-sweep-triangular-veclib-threads-N.txt` | the same, `VECLIB_MAXIMUM_THREADS=N` | `--sweep --scale 0.01 --only dtrsm_R_L_N,dtrsm_L_L_T,dtrmm_L_L_T,dtrtri_L` |
| `m3-tri-range-four-processes-p1.txt` to `-p4` | the same, four processes at once | `--tri-range 512 4096 --scale 0.01` |
| `m3-tri-range-veclib-threads-N.txt` | the same, `VECLIB_MAXIMUM_THREADS=N`, three processes at once | the same |
| `m3-tri-range-above-4096.txt` | the same | `--tri-range 4097 8192 --tri-step 61 --scale 0.01` |
| `m1-shapes-one-process.txt` | GitHub `macos-26` runner: Apple M1 (Virtual), 3 cores, macOS 26.7.2 | `--kernel-shapes --scale 20` |
| `m1-shapes-three-processes-p1.txt` to `-p3` | the same, three processes at once | the same |
| `m1-sweep.txt` | the same | `--sweep --scale 0.01` |
| `m1-tri-range-one-process.txt` | the same | `--tri-range 512 4096 --scale 0.01` (run 46050467649, cancelled after this step) |
| `m1-square.txt` | the same | no arguments: the square stress of the earlier runs |

The M3 runs were on 2026-11-27 at load average 3.5 to 15 from other work
(`m3-timeline.txt`). The M1 runs were workflow run 46038970861 on a scratch
branch.
