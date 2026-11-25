"""Preparing a model must stay linear in the depth of an expression, and a
very deep expression must prepare at the default 8 MB stack or fail cleanly.

The first model has a left-associated sum 20000 terms deep inside a branch that
a literal condition never takes. Preparation still reads, decodes and probes
the whole log_prob block, so a per-node walk of every subtree is quadratic in
the depth: seconds here, tens of milliseconds when linear.
"""
import base64
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest

DEPTH = 20000
TOO_DEEP = 100000
BOUND_SECONDS = 3.0

# The programs are written straight to portable MIR (the STANLI5 wire format
# that tests/portable_mir_writer.hpp also writes), because stanc3 takes
# seconds to compile an expression 100000 levels deep. Each function returns
# the bytes of one node; an expression is its fields, then its arguments,
# then its type: depth, leaf, data-only, promoted, and an empty raw string.
REAL_LEAF, VAR, LIT_REAL, FUN_APP = 2, 0, 2, 4
DECL, TARGET, BLOCK, IF_ELSE, NR_FUN_APP = 0, 2, 3, 6, 8
STAN_LIB, INTERNAL = 0, 1


def u8(value):
    return struct.pack("<B", value)


def u32(value):
    return struct.pack("<I", value)


def string(text):
    data = text.encode()
    return u32(len(data)) + data


def items(nodes):
    return u32(len(nodes)) + b"".join(nodes)


def real_type(data_only):
    return u8(0) + u8(REAL_LEAF) + u8(data_only) + u8(0) + string("")


def var(name, data_only=False):
    return u8(VAR) + string(name) + u8(0) + real_type(data_only)


def lit(value):
    return u8(LIT_REAL) + struct.pack("<d", value) + real_type(True)


def call_head(name, n_args, lib=STAN_LIB, suffix=0, propto=False):
    return (u8(FUN_APP) + u8(lib) + string(name) + u8(suffix) + u8(propto) +
            u32(n_args))


def call(name, args, data_only=False, **kwargs):
    return call_head(name, len(args), **kwargs) + b"".join(args) + real_type(
        data_only)


def read_param():
    """real NAME = FnReadParam(), an unconstrained real parameter."""
    return call("FnReadParam", [], lib=INTERNAL)


def declare(name, init, data_only=False, read=False):
    """A real declaration with an initial value; `read` marks a parameter."""
    return (u8(DECL) + string(name) + u8(0) + string("SReal") + items([]) +
            string("") + string("") + u8(data_only) + u8(1) + init +
            (u8(1) + u8(0) + items([]) + string("") if read else u8(0)) +
            items([]) + string(""))


def target(value):
    return u8(TARGET) + value + string("")


def block(statements):
    return u8(BLOCK) + items(statements) + string("")


def write(name):
    """FnWriteParam(NAME), one write_array column."""
    return (u8(NR_FUN_APP) + u8(INTERNAL) + string("FnWriteParam") + u8(0) +
            items([]) + u8(2) + u8(0) + var(name, True) + string(""))


def program(log_prob, outputs, generated=(), first_generated=0):
    """The STANLI5 document of a program with no data and no functions.
    `outputs` are (name, block) pairs, 0 for a parameter and 2 for a
    generated quantity."""
    payload = (u32(0) + items([]) + items(log_prob) + items(list(generated)) +
               u32(first_generated) + u32(first_generated) + items([]) +
               items([string(name) + u8(kind) + string("SReal") + items([]) +
                      string("") + string("") + items([])
                      for name, kind in outputs]) +
               items([]) + items([]))
    return "STANLI5:" + base64.b64encode(payload).decode()


def model(depth):
    """theta ~ normal(0, 1), and inside if (0) a target increment of a
    left-associated sum of depth + 1 thetas."""
    theta = var("theta")
    chain = (call_head("Plus__", 2) * depth + theta +
             (theta + real_type(False)) * depth)
    density = call("normal_lpdf", [theta, lit(0.0), lit(1.0)], suffix=2,
                   propto=True)
    # The int literal 0: tag 1, its value, then an int type (leaf 1).
    condition = (u8(1) + struct.pack("<i", 0) + u8(0) + u8(1) + u8(1) +
                 u8(0) + string(""))
    branch = (u8(IF_ELSE) + condition + items([block([target(chain)])]) +
              string(""))
    return program([declare("theta", read_param(), read=True),
                    target(density), branch], [("theta", 0)])


def default_stack():
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
    want = 8 * 1024 * 1024
    if hard != resource.RLIM_INFINITY:
        want = min(want, hard)
    resource.setrlimit(resource.RLIMIT_STACK, (want, hard))


def deep_expr(kind, depth, data_only):
    """A depth-deep sum of sin(mu * k), or sin(sin(...(mu)))."""
    mu = var("mu", data_only)
    if kind == "call":
        return (call_head("sin", 1) * depth + mu +
                real_type(data_only) * depth)

    def term(k):
        return call("sin", [call("Times__", [mu, lit(float(k))], data_only)],
                    data_only)
    parts = [call_head("Plus__", 2) * (depth - 1), term(1)]
    parts += [term(k) + real_type(data_only) for k in range(2, depth + 1)]
    return b"".join(parts)


def live_model(kind, depth, gq_depth=0):
    """target += a depth-deep expression of mu; with gq_depth, generated
    quantities also computes a gq_depth-deep expression of mu."""
    log_prob = [declare("mu", read_param(), read=True),
                target(deep_expr(kind, depth, False))]
    if not gq_depth:
        return program(log_prob, [("mu", 0)])
    generated = [declare("mu", read_param(), data_only=True, read=True),
                 write("mu"),
                 declare("g", deep_expr(kind, gq_depth, True), data_only=True),
                 write("g")]
    return program(log_prob, [("mu", 0), ("g", 2)], generated, 2)


def prepare(text, parallel_wa="auto"):
    with tempfile.TemporaryDirectory() as directory:
        mir = Path(directory) / "deep.mir"
        data = Path(directory) / "data.json"
        mir.write_text(text)
        data.write_text("{}")
        start = time.perf_counter()
        run = subprocess.run([BENCH_GRAD, str(mir), str(data), "--prep",
                              f"--parallel-wa={parallel_wa}"],
                             capture_output=True, text=True,
                             preexec_fn=default_stack)
        return run, time.perf_counter() - start


# CTest runs this class alone, since it checks a time bound.
@unittest.skipIf(sys.platform == "win32", "needs setrlimit for the stack")
class DeepExpressionPrepTime(unittest.TestCase):
    def test_prep_is_linear_in_depth(self):
        run, elapsed = prepare(model(DEPTH))
        self.assertEqual(run.returncode, 0, run.stderr[-400:])
        self.assertLess(elapsed, BOUND_SECONDS,
                        f"prep of a {DEPTH}-deep expression took {elapsed:.2f} s")


@unittest.skipIf(sys.platform == "win32", "needs setrlimit for the stack")
class DeepExpressionPrep(unittest.TestCase):
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

    def test_deep_generated_quantities_prepare_beside_log_prob(self):
        for kind in ("sum", "call"):
            cases = [(2, "force")] + [(DEPTH, mode) for mode in ("auto", "off")]
            for depth, mode in cases:
                with self.subTest(kind=kind, depth=depth, mode=mode):
                    run, _ = prepare(live_model(kind, depth, gq_depth=DEPTH),
                                     mode)
                    self.assertEqual(run.returncode, 0, run.stderr[-400:])

    def test_excessive_generated_quantities_depth_is_the_same_error(self):
        for kind in ("sum", "call"):
            with self.subTest(kind=kind):
                text = live_model(kind, 2, gq_depth=TOO_DEEP)
                beside, _ = prepare(text, "force")
                alone, _ = prepare(text, "off")
                self.assertEqual(beside.returncode, 1, f"signal {-beside.returncode}")
                self.assertIn("nested too deeply", beside.stderr)
                self.assertEqual(beside.stderr, alone.stderr)


if __name__ == "__main__":
    BENCH_GRAD = sys.argv.pop(1)
    unittest.main()
