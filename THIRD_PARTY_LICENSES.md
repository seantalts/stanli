# Third-party components in the stanli binary

The stanli shared library is self-contained: it links only the system C
and C++ runtimes, and everything else is compiled in. This file lists what
is compiled in and under what terms, because distributing the binary
distributes those components too.

| Component | Role in the binary | License |
| --- | --- | --- |
| [Stan math library](https://github.com/stan-dev/math) | every density, constraint transform, and reverse-mode derivative | BSD 3-Clause |
| [Stan](https://github.com/stan-dev/stan) | NUTS sampler and adaptation | BSD 3-Clause |
| [stanc3](https://github.com/stan-dev/stanc3) | the Stan compiler, compiled to a self-contained object and linked in | BSD 3-Clause |
| OCaml runtime | required by the compiled stanc3 object | LGPL 2.1 with the OCaml linking exception, which explicitly permits linking into a binary under other terms |
| [Eigen](https://eigen.tuxfamily.org) | dense linear algebra behind stan-math | MPL 2.0 |
| [Boost](https://www.boost.org) | math special functions and utilities used by stan-math | Boost Software License 1.0 |
| [SUNDIALS / CVODES](https://computing.llnl.gov/projects/sundials) | ODE integration for `integrate_ode_rk45` and `integrate_ode_bdf` | BSD 3-Clause |
| [nlohmann/json](https://github.com/nlohmann/json) | reading CmdStan-format JSON data | MIT |
| [walnutpie](https://github.com/flatironinstitute/walnuts) | WALNUTS sampler and its warmup adaptation | MIT |

Full license texts ship with the vendored sources fetched by
`deps/fetch.sh`; see `deps/math/LICENSE.md`, `deps/stan/LICENSE.md`, and
the license files under `deps/math/lib/`. walnutpie is vendored directly
in this repository, headers and license together, at
`runtime/third_party/walnutpie/`.

Notes on the two that carry conditions beyond attribution:

- **Eigen (MPL 2.0)** is a file-level copyleft: distributing the binary is
  fine, and modifications to Eigen's own files would have to be published.
  stanli does not modify Eigen.
- **OCaml runtime (LGPL 2.1)** ships with a linking exception written for
  exactly this case ("you may link this library into an executable and
  distribute that executable under terms of your choice"), so no relinking
  obligation attaches to the stanli binary.

Not in the binary, but in the repository: `tests/stanc3/` holds Stan
models copied from stanc3's test suite (BSD 3-Clause, the same license as
this repository), used as test inputs. Provenance and the few documented
runnable adaptations are in `tests/stanc3/README.md`.
