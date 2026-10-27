"""Preparing a model must stay linear in the depth of an expression.

The model has a left-associated sum 20000 terms deep inside a branch that a
literal condition never takes. Preparation still reads, decodes and probes the
whole log_prob block, so a per-node walk of every subtree is quadratic in the
depth: seconds here, tens of milliseconds when linear. The child runs with a
raised stack limit because a chain this deep needs one to be read at all.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

DEPTH = 20000
BOUND_SECONDS = 1.5

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


def raise_stack():
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
    want = 64 * 1024 * 1024
    if hard != resource.RLIM_INFINITY:
        want = min(want, hard)
    resource.setrlimit(resource.RLIMIT_STACK, (want, hard))


@unittest.skipIf(sys.platform == "win32", "needs setrlimit for the stack")
class DeepExpressionPrep(unittest.TestCase):
    def test_prep_is_linear_in_depth(self):
        with tempfile.TemporaryDirectory() as directory:
            mir = Path(directory) / "deep.sexp"
            data = Path(directory) / "data.json"
            mir.write_text(model(DEPTH))
            data.write_text("{}")
            start = time.perf_counter()
            run = subprocess.run([BENCH_GRAD, str(mir), str(data), "--prep"],
                                 capture_output=True, text=True,
                                 preexec_fn=raise_stack, env=dict(os.environ, STANLI_PROFILE_PREP="0"))
            elapsed = time.perf_counter() - start
        self.assertEqual(run.returncode, 0, run.stderr[-400:])
        self.assertLess(elapsed, BOUND_SECONDS,
                        f"prep of a {DEPTH}-deep expression took {elapsed:.2f} s")


if __name__ == "__main__":
    BENCH_GRAD = sys.argv.pop(1)
    unittest.main()
