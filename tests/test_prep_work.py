"""Preparation work must not grow with the wrong part of a model.

Each case prepares two sizes of one model shape and reads the lowering's work
counters (`stanli_prep ... stage=lower_work`, printed under
STANLI_PROFILE_PREP=1): the statements it lowered and the bytes of data it
copied. The counters do not depend on the clock, so the bounds are exact
consequences of the algorithm, not timer noise.

  test_prep_work.py BENCH_GRAD STANC STANLI_RUN
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

WORK = re.compile(r"stanli_prep graph=(\S+) stage=lower_work ns=\d+ "
                  r"statements=(\d+) copied_data_bytes=(\d+)")


def work(bench, stanc, directory, name, source, data):
    """{graph: (statements, copied bytes)} for the last row of each graph."""
    stan = directory / f"{name}.stan"
    stan.write_text(source)
    mir = subprocess.run([stanc, "--O1", "--debug-optimized-mir", str(stan)],
                         check=True, capture_output=True, text=True).stdout
    sexp = directory / f"{name}.sexp"
    sexp.write_text(mir)
    data_path = directory / f"{name}.json"
    data_path.write_text(json.dumps(data))
    env = dict(os.environ, STANLI_PROFILE_PREP="1")
    run = subprocess.run([bench, str(sexp), str(data_path), "--prep",
                          "--parallel-wa=off"],
                         check=True, capture_output=True, text=True, env=env)
    rows = {}
    for graph, statements, copied in WORK.findall(run.stdout + run.stderr):
        rows[graph] = (int(statements), int(copied))
    if "log_prob" not in rows:
        raise RuntimeError(f"{name}: no lower_work row in\n{run.stderr}")
    return rows


def run_work(stanli_run, stanc, directory, name, source, data, *options):
    """The same counters from stanli_run, which takes the threading options
    that bench_grad does not."""
    stan = directory / f"{name}.stan"
    stan.write_text(source)
    data_path = directory / f"{name}.json"
    data_path.write_text(json.dumps(data))
    env = dict(os.environ, STANLI_PROFILE_PREP="1")
    run = subprocess.run([stanli_run, str(stan), str(data_path), "--stanc",
                          stanc, "--warmup", "1", "--samples", "1", *options],
                         check=True, capture_output=True, text=True, env=env)
    return {graph: (int(statements), int(copied))
            for graph, statements, copied in WORK.findall(run.stderr)}


def ifs_in_retained_loop(k, n):
    """A loop that parameter control keeps, with k `if` statements in its
    body and two data vectors of n elements that the body reads."""
    ifs = "\n".join(
        f"      if (s > {i}) s = s - x[{i % n + 1}]; else s = s + y[{i % n + 1}];"
        for i in range(k))
    source = f"""data {{ int N; vector[N] x; vector[N] y; }}
parameters {{ real theta; }}
model {{
  real s = 0;
  for (g in 1:40) {{
    if (s < theta) {{
{ifs}
    }}
    s = s * 0.5 + theta;
  }}
  target += -square(s);
  theta ~ normal(0, 1);
}}
"""
    data = {"N": n, "x": [0.1 * i for i in range(n)],
            "y": [0.2 * i for i in range(n)]}
    return source, data


def check_ifs_in_retained_loop(measure):
    n = 8000
    data_bytes = 2 * n * 8
    small = measure("ifs4", *ifs_in_retained_loop(4, n))["log_prob"][1]
    large = measure("ifs16", *ifs_in_retained_loop(16, n))["log_prob"][1]
    print(f"ifs in a retained loop: 4 ifs copy {small} bytes, 16 copy {large}")
    if large - small >= data_bytes:
        return [f"12 more `if` statements copied {large - small} more bytes "
                f"of data ({data_bytes} bytes in all)"]
    return []


def calls_in_retained_loop(k, n):
    """The same loop reading k data values at positions that a user function
    computes. The overload keeps stanc from inlining the function."""
    reads = "\n".join(f"      s = s - x[half(N - {i % n})];" for i in range(k))
    source = f"""functions {{
  int half(int m) {{ return m %/% 2 + 1; }}
  real half(real m) {{ return m / 2; }}
}}
data {{ int N; vector[N] x; vector[N] y; }}
parameters {{ real theta; }}
model {{
  real s = 0;
  for (g in 1:40) {{
    if (s < theta) {{
{reads}
    }}
    s = s * 0.5 + theta;
  }}
  target += -square(s);
  theta ~ normal(0, 1);
}}
"""
    data = {"N": n, "x": [0.1 * i for i in range(n)],
            "y": [0.2 * i for i in range(n)]}
    return source, data


def check_calls_in_retained_loop(measure):
    n = 8000
    data_bytes = 2 * n * 8
    small = measure("calls4", *calls_in_retained_loop(4, n))["log_prob"][1]
    large = measure("calls16", *calls_in_retained_loop(16, n))["log_prob"][1]
    print(f"calls in a retained loop: 4 calls copy {small} bytes, 16 copy {large}")
    if large - small >= data_bytes:
        return [f"12 more indices that call a function copied "
                f"{large - small} more bytes of data ({data_bytes} bytes in all)"]
    return []


def reduce_sum_beside_data(n):
    """reduce_sum in four chunks, beside a data vector of n elements that
    the partial sum never sees."""
    m = 100000
    source = """functions {
  real part(array[] real ys, int start, int end, real mu) {
    return normal_lpdf(ys | mu, 1);
  }
}
data { int M; array[M] real y; int N; vector[N] z; }
parameters { real mu; }
model {
  target += reduce_sum_static(part, y, M %/% 4, mu);
  mu ~ normal(mean(z), 1);
}
"""
    data = {"M": m, "y": [0.01 * i for i in range(m)], "N": n,
            "z": [0.5] * n}
    return source, data


def check_reduce_sum_beside_data(run_measure):
    small_n, large_n = 1000, 100000
    small = run_measure("rs_small", *reduce_sum_beside_data(small_n))
    large = run_measure("rs_large", *reduce_sum_beside_data(large_n))
    if not small or not large:
        print("reduce_sum: not lowered in chunks in this build; skipped")
        return []
    small, large = small["log_prob"][1], large["log_prob"][1]
    grown = (large_n - small_n) * 8
    print(f"reduce_sum beside data: {small} bytes copied with {small_n} "
          f"unrelated values, {large} with {large_n}")
    if large - small >= grown:
        return [f"{grown} more bytes of data that reduce_sum never reads "
                f"made lowering copy {large - small} more bytes"]
    return []


def carried_loop_after_large_graph(first):
    """A loop of 1000 trips that carries m, beside 300,000 rows of
    independent terms that come before it or after it. After them, the
    graph has passed the size at which lowering asks whether to keep the
    loop. The slice that starts at t keeps the retained loop from holding
    it, so the answer is no."""
    t, n = 1000, 300000
    loop = """  {
    real m = 0;
    for (t in 1:T) {
      m = 0.5 * m + sin(theta * x[t]) + cos(theta * x[t]) + exp(-theta * x[t])
          + log1p(square(theta) * x[t]) + tanh(theta * x[t]) + sin(m * x[t])
          + cos(m * x[t]) + atan(m * x[t]) + sqrt(1 + square(m) * x[t])
          + sin(m * y[t]);
      m += 0.001 * sum(x[t:T]);
      target += normal_lpdf(y[t] | m, 1);
    }
  }
"""
    rows = "  for (n in 1:N) target += normal_lpdf(z[n] | theta * w[n] + n, 2);\n"
    head = """data { int T; int N; vector[T] x; vector[T] y; vector[N] z; vector[N] w; }
parameters { real theta; }
model {
  theta ~ normal(0, 1);
"""
    source = head + (loop + rows if first else rows + loop) + "}\n"
    data = {"T": t, "N": n, "x": [0.001 * i for i in range(t)],
            "y": [0.002 * i for i in range(t)],
            "z": [0.0001 * i for i in range(n)],
            "w": [0.0003 * i for i in range(n)]}
    return source, data


def check_refused_retention(measure):
    first = measure("loop_first", *carried_loop_after_large_graph(True))
    after = measure("loop_after", *carried_loop_after_large_graph(False))
    first, after = first["log_prob"][0], after["log_prob"][0]
    print(f"refused retention: {first} statements with the loop first, "
          f"{after} with it after the rows")
    if after - first >= 1000:
        return [f"a loop that lowering declined to keep was lowered again: "
                f"{after - first} more statements"]
    return []


CASES = [check_ifs_in_retained_loop, check_calls_in_retained_loop,
         check_refused_retention]
RUN_CASES = [check_reduce_sum_beside_data]


def main(bench, stanc, stanli_run):
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)

        def measure(name, source, data):
            return work(bench, stanc, directory, name, source, data)

        def run_measure(name, source, data):
            return run_work(stanli_run, stanc, directory, name, source, data,
                            "--threads-per-chain", "4")

        for case in CASES:
            failures += case(measure)
        for case in RUN_CASES:
            failures += case(run_measure)
    if failures:
        sys.exit("preparation work grows with the wrong size:\n  " +
                 "\n  ".join(failures))
    print("PASS")


if __name__ == "__main__":
    main(*sys.argv[1:])
