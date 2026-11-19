#!/usr/bin/env python3
"""A recursive user function must compile on a 1 MB stack.

Lowering inlines a user function call by lowering its body in place, so
each level of recursion holds a few tens of kilobytes of stack until the
recursion ends or reaches its depth limit. A Windows thread has 1 MB by
default, where Linux and macOS give the main thread 8 MB. Each model here
runs once on the default stack and once on a 1 MB stack (set with
setrlimit, or the Windows default), and both runs must print the same
result.

  test_udf_recursion_stack.py STANLI_CHECK
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent

# A recursion whose depth depends on a parameter-free argument that lowering
# only knows at run time inside the loop.
WHILE_COUNTER = ROOT / "compile_time_values" / "while_recursive_udf.stan"

# A recursion 40 calls deep that lowering inlines all the way down, because
# its argument is a parameter.
PARAMETER = """
functions {
  real rec(int n, real x) {
    if (n <= 0) {
      return x;
    }
    return tanh(rec(n - 1, x));
  }
}
parameters {
  real mu;
}
model {
  target += rec(40, mu);
  mu ~ normal(0, 1);
}
"""

# Mutual recursion, so the check cannot be only for a function that calls
# itself.
MUTUAL = """
functions {
  real odd(int n, real x);
  real even(int n, real x) {
    if (n <= 0) {
      return x;
    }
    return sin(odd(n - 1, x));
  }
  real odd(int n, real x) {
    return cos(even(n - 1, x));
  }
}
parameters {
  real mu;
}
model {
  target += even(40, mu);
  mu ~ normal(0, 1);
}
"""

STACK_BYTES = 1 << 20


def small_stack():
    import resource
    _, hard = resource.getrlimit(resource.RLIMIT_STACK)
    want = STACK_BYTES
    if hard != resource.RLIM_INFINITY:
        want = min(want, hard)
    resource.setrlimit(resource.RLIMIT_STACK, (want, hard))


def run(check, stan, data, limit):
    preexec = small_stack if limit and sys.platform != "win32" else None
    return subprocess.run([check, str(stan), str(data), "--point", "1"],
                          capture_output=True, text=True, preexec_fn=preexec)


def result_line(stdout):
    return next((l for l in stdout.splitlines() if l.startswith("OK ")), None)


def main(check):
    failures = 0
    with tempfile.TemporaryDirectory() as work:
        data = Path(work) / "data.json"
        data.write_text("{}\n")
        models = {"while_recursive_udf": WHILE_COUNTER}
        for name, text in (("parameter", PARAMETER), ("mutual", MUTUAL)):
            models[name] = Path(work) / f"{name}.stan"
            models[name].write_text(text)
        for name, stan in models.items():
            default = run(check, stan, data, limit=False)
            small = run(check, stan, data, limit=True)
            want = result_line(default.stdout)
            got = result_line(small.stdout)
            if want is None or got != want or small.returncode != 0:
                failures += 1
                print(f"FAIL {name}: default stack gave {want!r} "
                      f"(exit {default.returncode}), 1 MB stack gave {got!r} "
                      f"(exit {small.returncode})")
                for label, result in (("default", default), ("1 MB", small)):
                    if result.returncode != 0:
                        print(f"  {label} stderr:\n{result.stderr[-2000:]}")
            else:
                print(f"ok {name}: {got}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
