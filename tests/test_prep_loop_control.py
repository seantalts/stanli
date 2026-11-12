"""Preparation time of an unrolled loop must not depend on how its body
spells a decision the data already makes.

Each pair is one loop over N observations written two ways that lower to the
same graph. A `break`, `continue` or `return` that unwinds lowering once per
iteration costs ten to twenty times the plain form, so the bound sits well
above timer noise on a busy CI runner and well below that.
"""
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

N = 8000
REPEATS = 5
MAX_RATIO = 4.0

DATA = "data { int N; vector[N] y; array[N] int k; }\n"
PARAMETERS = "parameters { real mu; }\n"

INLINE = DATA + PARAMETERS + """model {
  for (n in 1:N) {
    if (k[n] == 1) target += normal_lpdf(y[n] | mu, 1);
    else target += normal_lpdf(y[n] | mu, 2);
  }
}
"""

TWO_RETURNS = """functions {
  real f(real y, real mu, int k) {
    if (k == 1) {
      return normal_lpdf(y | mu, 1);
    }
    return normal_lpdf(y | mu, 2);
  }
}
""" + DATA + PARAMETERS + """model {
  for (n in 1:N) target += f(y[n], mu, k[n]);
}
"""

CONTINUE = DATA + PARAMETERS + """model {
  for (n in 1:N) {
    if (k[n] == 1) continue;
    target += normal_lpdf(y[n] | mu, 2);
  }
}
"""

PAIRS = {
    "two-return user function": (INLINE, TWO_RETURNS),
    "data-decided continue": (INLINE, CONTINUE),
}


def prep_seconds(bench, stanc, directory, source):
    stan = directory / "model.stan"
    stan.write_text(source)
    mir = subprocess.run([stanc, "--O1", "--debug-optimized-mir", str(stan)],
                         check=True, capture_output=True, text=True).stdout
    (directory / "model.sexp").write_text(mir)
    best = float("inf")
    for _ in range(REPEATS):
        out = subprocess.run([bench, str(directory / "model.sexp"),
                              str(directory / "data.json"), "--prep"],
                             check=True, capture_output=True, text=True).stdout
        best = min(best, float(out.split()[0]))
    return best


def main(bench, stanc):
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)
        (directory / "data.json").write_text(json.dumps({
            "N": N,
            "y": [math.sin(i) for i in range(N)],
            "k": [1 + i % 2 for i in range(N)],
        }))
        for name, (plain, variant) in PAIRS.items():
            base = prep_seconds(bench, stanc, directory, plain)
            other = prep_seconds(bench, stanc, directory, variant)
            ratio = other / base
            print(f"{name}: plain {base:.4f}s, variant {other:.4f}s, ratio {ratio:.1f}")
            if ratio > MAX_RATIO:
                failures.append(f"{name}: {ratio:.1f}x the plain loop")
    if failures:
        sys.exit("loop control slows preparation:\n  " + "\n  ".join(failures))
    print("PASS: loop control does not slow preparation")


if __name__ == "__main__":
    main(*sys.argv[1:])
