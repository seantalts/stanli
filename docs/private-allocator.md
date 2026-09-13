# Private native allocation

Fresh Linux builds select `STANLI_SHARED_ALLOCATOR=AUTO`, enabling the
private allocator on supported configurations. Windows and macOS default
to SYSTEM. This Linux rollout is gated by the
[matched validation plan](linux-allocator-rollout.md); the PR remains a draft
until those results justify promotion. No Stan Math or Eigen source changes
are required, and no claim is made that every model is faster.

After the usual dependency/compiler setup:

```sh
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DSTANLI_SHARED_ALLOCATOR=MIMALLOC
cmake --build build-rel --parallel 4
ctest --test-dir build-rel --output-on-failure
```

`SYSTEM` leaves allocation unchanged. `MIMALLOC` requires a supported
configuration and fails at configuration time otherwise. `AUTO` selects
private mimalloc on supported configurations and SYSTEM elsewhere; AUTO is
an explicit opt-in outside Linux. Existing cached selections are respected,
including SYSTEM. The build ID of an enabled shared library ends in
`-mimalloc-3.5.1-private-c`.

## Ownership boundary

Only C allocation inside the native shared library is redirected. Python/R
host allocations and C++ `new/delete` retain their existing allocators.
Static libraries, CLI executables, browser/WASM and webR are unchanged.
Always release returned C API/BridgeStan buffers with their documented
matching library free function; they are not host-owned buffers.

- macOS uses private, export-hidden C allocation definitions. These also
  cover C archives linked into that DSO, but not external libc++. Foreign
  system pointers are freed/reallocated through their owning malloc zone.
- Linux and MinGW rewrite a **copy** of the target's input archive. Only its
  C allocation references change; original reusable objects, dependency
  archives and the statically linked C++ runtime retain system allocation.
  There is no final-link `--wrap`, process interposition or preload.
- mimalloc 3.5.1 is pinned in `deps/fetch.sh`, built privately without
  override/interpose/zone/redirect support or newer-CPU optimizations.
  TLS is LOCAL_DYNAMIC on Linux (late loading), PTHREADS on macOS and WIN32
  on Windows. No custom purge delay, heap reset or per-gradient collection
  is configured.

The supported native targets are macOS/Linux arm64 and x86_64, and Windows
x86_64 with MinGW/UCRT. CMake 3.21 or newer is required for the private
integration. Sanitizers, LTO/IPO, MSVC, Windows ARM and WASM retain SYSTEM
under AUTO; forcing MIMALLOC on an unsupported configuration is an error.
The execution matrix tests each Mac architecture separately, not universal
execution or the oldest supported macOS release. The runtime remains loaded
until process exit in host tests; arbitrary unloading of the embedded
compiler is not promised.

## Validation and remaining rollout gates

`tests/allocator` is a small standalone ownership gate requiring only the
fetched dependencies. It covers C/C++ ownership, unchanged reusable objects,
linked dependencies, Eigen, foreign realloc/free, allocation failure, threads,
cross-thread release, probe handle closure/reopening and export isolation.
The full runtime additionally checks late loading, C API/BridgeStan strings
and repeated gradients when the compiler is embedded.

```sh
cmake -S tests/allocator -B build-allocator -DCMAKE_BUILD_TYPE=Release
cmake --build build-allocator --parallel 4
ctest --test-dir build-allocator --output-on-failure
```

The reusable ownership workflow runs on five native platform/architecture
pairs plus Linux Clang. The existing required PR gate depends on it.
The wheels workflow also accepts a manual `shared_allocator=MIMALLOC`
validation input; normal PR, release and nightly builds use the platform
default, including Linux's supported private-allocator path.

Default promotion is a separate platform-by-platform decision. It needs
broader corrected warm-gradient comparisons, first-gradient/startup costs,
end-to-end sampling, memory behavior and the applicable native/binding CI.
Linux's tiny parallel losses were startup-sensitive in the original short
benchmark; sustained native warmup corrected that measurement boundary, not
shipping code. Apple ARM's large-vector loss remains unresolved. Neither a
Linux win nor ownership parity establishes an Apple default.

## Research checkpoint and split

The complete research is preserved at
[`4e2d1bb4`](https://github.com/seantalts/stanli/tree/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439),
also retained on `codex/allocator-research-archive`:

- [Investigation and negative results](https://github.com/seantalts/stanli/blob/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439/docs/allocator-regression-investigation.md)
- [Measurements, ranges and limitations](https://github.com/seantalts/stanli/blob/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439/docs/allocator-rollout.md)
- [Raw evidence, full snapshots and source identities](https://github.com/seantalts/stanli/blob/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439/tools/allocator/results/README.md)

PR #363 now consolidates the integration, corrected benchmark and Linux
default rollout at the user's request. It excludes loop-alignment controls
(a separate parked branch), raw historical research artifacts and unsuccessful
placement/TLS/large-allocation fallback prototypes. The remaining Apple
mechanism question is preserved, not solved by an allocator threshold or
default change.
