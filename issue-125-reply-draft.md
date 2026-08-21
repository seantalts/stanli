Thanks for the report and the clean reprex, that made this quick to track down.

Root cause: stanc3 keeps every overload of a user-defined function under the same name in its IR, and stanli looked functions up by name alone, so the last definition won. Your model ends up storing the vector overload of `cox_lccdf`; the body of `cox_lcdf` calls the scalar overload, and that call is what failed the type check. That is why declaring `cox_lcdf` was enough to trigger the error even though it is never invoked: the `target += cox_lccdf(Y | ...)` line was fine on its own.

Fixed in #126: calls now resolve to the overload their argument types select. Your reprex compiles and evaluates correctly, with the model block resolving to the vector overload as expected.

The fix will be in the next release. Once that is tagged, rerun `stanli_install(overwrite = TRUE)` to pick up the new runtime.
