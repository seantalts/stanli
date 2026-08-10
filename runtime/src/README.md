# runtime/src

The implementation: one `.cpp` per subsystem, compiled into the
library and included by nothing. Interfaces these files share with
each other or with the tools live in
[`runtime/include/stanli/`](../include/stanli/); op implementations
live in [`runtime/kernels/`](../kernels/).

The compiler is [`mir_reader.cpp`](mir_reader.cpp) (stanc3's
s-expressions in) and [`lower.cpp`](lower.cpp) (op graph out),
followed by the optimization passes in pipeline order:
[`inplace.cpp`](inplace.cpp), [`constfold.cpp`](constfold.cpp),
[`reroll.cpp`](reroll.cpp), and [`island.cpp`](island.cpp) with its
generated backward in [`adjoint.cpp`](adjoint.cpp). What each pass buys and how it was
measured is [`OPTIMIZATIONS.md`](OPTIMIZATIONS.md); the how and why
of each subsystem, with recipes for common changes, is
[`docs/hacking.md`](../../docs/hacking.md).

Everything else runs the result: [`executor.cpp`](executor.cpp) walks
the op list, [`nuts.cpp`](nuts.cpp) samples with Stan's own NUTS,
[`wa_interp.cpp`](wa_interp.cpp) interprets generated quantities the
graph cannot express, and [`capi.cpp`](capi.cpp) wraps the whole
thing in the C ABI the language wrappers call.
