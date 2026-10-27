"""Preparing a model must stay linear in the depth of an expression, and a
very deep expression must prepare at the default 8 MB stack or fail cleanly.

The first model has a left-associated sum 20000 terms deep inside a branch that
a literal condition never takes. Preparation still reads, decodes and probes
the whole log_prob block, so a per-node walk of every subtree is quadratic in
the depth: seconds here, tens of milliseconds when linear.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

DEPTH = 20000
TOO_DEEP = 100000
BOUND_SECONDS = 3.0

REAL = "(meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))"
INT = "(meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))"
DATA_REAL = "(meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))"


def model(depth):
    theta = f"((pattern (Var theta)) {REAL})"
    open_plus = "((pattern (FunApp (StanLib Plus__ FnPlain AoS) ("
    close_plus = f" {theta}))) {REAL})"
    chain = open_plus * depth + theta + close_plus * depth
    zero = f"((pattern (Promotion ((pattern (Lit Int 0)) {INT}) UReal DataOnly)) {DATA_REAL})"
    one = f"((pattern (Promotion ((pattern (Lit Int 1)) {INT}) UReal DataOnly)) {DATA_REAL})"
    return f"""((functions_block ()) (input_vars ()) (prepare_data ())
 (log_prob
  (((pattern (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
      (initialize (Assign ((pattern (FunApp (CompilerInternal (FnReadParam (constrain Identity) (dims ()) (mem_pattern AoS))) ())) {REAL})))))
    (meta <opaque>))
   ((pattern (TargetPE ((pattern (FunApp (StanLib normal_lpdf (FnLpdf true) AoS) ({theta} {zero} {one}))) {REAL})))
    (meta <opaque>))
   ((pattern (IfElse ((pattern (Lit Int 0)) {INT})
      ((pattern (Block (((pattern (TargetPE {chain})) (meta <opaque>))))) (meta <opaque>))
      ()))
    (meta <opaque>))))
 (output_vars ((theta <opaque> ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters) (out_trans Identity)))))
 (prog_name deep_expression) (prog_path deep_expression.stan))
"""


def default_stack():
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
    want = 8 * 1024 * 1024
    if hard != resource.RLIM_INFINITY:
        want = min(want, hard)
    resource.setrlimit(resource.RLIMIT_STACK, (want, hard))


def live_model(kind, depth):
    """target += a depth-deep sum of sin(mu * k), or sin(sin(...(mu)))."""
    def fun(name, args):
        return f"((pattern (FunApp (StanLib {name} FnPlain AoS) ({args}))) {REAL})"
    mu = f"((pattern (Var mu)) {REAL})"
    if kind == "call":
        expr = "".join(f"((pattern (FunApp (StanLib sin FnPlain AoS) (" for _ in range(depth))
        expr += mu + f"))) {REAL})" * depth
    else:
        def term(k):
            return fun("sin", fun("Times__", f"{mu} ((pattern (Lit Real {k}.0)) {DATA_REAL})"))
        plus = "((pattern (FunApp (StanLib Plus__ FnPlain AoS) ("
        parts = [plus * (depth - 1), term(1)]
        parts += [f" {term(k)}))) {REAL})" for k in range(2, depth + 1)]
        expr = "".join(parts)
    return f"""((functions_block ()) (input_vars ()) (prepare_data ())
 (log_prob
  (((pattern (Decl (decl_adtype AutoDiffable) (decl_id mu) (decl_type (Sized SReal))
      (initialize (Assign ((pattern (FunApp (CompilerInternal (FnReadParam (constrain Identity) (dims ()) (mem_pattern AoS))) ())) {REAL})))))
    (meta <opaque>))
   ((pattern (TargetPE {expr})) (meta <opaque>))))
 (output_vars ((mu <opaque> ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters) (out_trans Identity)))))
 (prog_name deep) (prog_path deep.stan))
"""


def prepare(text):
    with tempfile.TemporaryDirectory() as directory:
        mir = Path(directory) / "deep.sexp"
        data = Path(directory) / "data.json"
        mir.write_text(text)
        data.write_text("{}")
        start = time.perf_counter()
        run = subprocess.run([BENCH_GRAD, str(mir), str(data), "--prep"],
                             capture_output=True, text=True,
                             preexec_fn=default_stack, env=dict(os.environ, STANLI_PROFILE_PREP="0"))
        return run, time.perf_counter() - start


@unittest.skipIf(sys.platform == "win32", "needs setrlimit for the stack")
class DeepExpressionPrep(unittest.TestCase):
    def test_prep_is_linear_in_depth(self):
        run, elapsed = prepare(model(DEPTH))
        self.assertEqual(run.returncode, 0, run.stderr[-400:])
        self.assertLess(elapsed, BOUND_SECONDS,
                        f"prep of a {DEPTH}-deep expression took {elapsed:.2f} s")

    def test_deep_sum_prepares_at_default_stack(self):
        run, _ = prepare(live_model("sum", DEPTH))
        self.assertEqual(run.returncode, 0, run.stderr[-400:])

    def test_deep_call_prepares_at_default_stack(self):
        run, _ = prepare(live_model("call", DEPTH))
        self.assertEqual(run.returncode, 0, run.stderr[-400:])

    def test_excessive_depth_is_a_clean_error(self):
        for kind in ("sum", "call"):
            with self.subTest(kind=kind):
                run, _ = prepare(live_model(kind, TOO_DEEP))
                self.assertEqual(run.returncode, 1, f"signal {-run.returncode}")
                self.assertIn("nested too deeply", run.stderr)


if __name__ == "__main__":
    BENCH_GRAD = sys.argv.pop(1)
    unittest.main()
