"""Preparation work must not grow with the wrong part of a model.

Each case prepares two sizes of one model shape and reads work counters that
STANLI_PROFILE_PREP=1 prints: how many statements lowering processed and how
many bytes of data it copied (`stage=lower_work`), and how many lane steps
reroll took (`stage=reroll ... lane_steps`). The counters do not depend on
the clock, so the bounds are exact consequences of the algorithm, not timer
noise.

  test_prep_work.py BENCH_GRAD STANLI_CHECK STANLI_RUN
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from gen_fixtures import O1_PASSES  # noqa: E402

WORK = re.compile(r"stanli_prep graph=(\S+) stage=lower_work ns=\d+ "
                  r"statements=(\d+) copied_data_bytes=(\d+)")
REROLL = re.compile(r"stanli_prep graph=(\S+) stage=reroll ns=\d+ .*?"
                    r"lane_steps=(\d+)")


def work(bench, check, directory, name, source, data):
    """{graph: (statements, copied bytes)} for the last row of each graph.
    The model gets the MIR fixtures' O1 passes (tools/gen_fixtures.py)."""
    stan = directory / f"{name}.stan"
    stan.write_text(source)
    mir = subprocess.run(
        [check, "--emit-mir", str(stan), "--passes", O1_PASSES],
        check=True, capture_output=True, text=True).stdout
    mir_path = directory / f"{name}.mir"
    mir_path.write_text(mir)
    data_path = directory / f"{name}.json"
    data_path.write_text(json.dumps(data))
    env = dict(os.environ, STANLI_PROFILE_PREP="1")
    run = subprocess.run([bench, str(mir_path), str(data_path), "--prep",
                          "--parallel-wa=off"],
                         check=True, capture_output=True, text=True, env=env)
    rows, lowerings, lane_steps = {}, {}, {}
    for graph, statements, copied in WORK.findall(run.stdout + run.stderr):
        rows[graph] = (int(statements), int(copied))
        lowerings[graph] = lowerings.get(graph, 0) + 1
    for graph, steps in REROLL.findall(run.stdout + run.stderr):
        lane_steps[graph] = int(steps)
    rows["lowerings"] = lowerings
    rows["lane_steps"] = lane_steps
    if "log_prob" not in rows:
        raise RuntimeError(f"{name}: no lower_work row in\n{run.stderr}")
    return rows


def run_work(stanli_run, directory, name, source, data, *options):
    """The same counters from stanli_run, which takes the threading options
    that bench_grad does not. It compiles with the shipping passes."""
    stan = directory / f"{name}.stan"
    stan.write_text(source)
    data_path = directory / f"{name}.json"
    data_path.write_text(json.dumps(data))
    env = dict(os.environ, STANLI_PROFILE_PREP="1")
    run = subprocess.run([stanli_run, str(stan), str(data_path),
                          "--warmup", "1", "--samples", "1", *options],
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


def count_sizes_later_declaration():
    """generated quantities with a carried loop that counts into k after
    70,000 draws, and a declaration that k sizes after it. A retained loop
    gives k only at run time, too late for the declaration."""
    source = """data { int N; int T; }
parameters { real theta; }
model { theta ~ normal(0, 1); }
generated quantities {
  vector[N] yrep;
  real s;
  for (n in 1:N) yrep[n] = normal_rng(theta, 1);
  {
    int k = 0;
    real acc = 0;
    for (t in 1:T) {
      acc = 0.5 * acc + theta + t;
      if (t % 3 == 0) k += 1;
    }
    vector[k] v = rep_vector(1, k);
    s = sum(v) + acc;
  }
}
"""
    return source, {"N": 70000, "T": 50}


def check_write_array_lowered_once(measure):
    rows = measure("wa_sizes", *count_sizes_later_declaration())
    times = rows["lowerings"].get("write_array", 0)
    print(f"write_array with a size read from a loop: lowered {times} times")
    if times != 1:
        return [f"write_array was lowered {times} times"]
    return []


def rng_draws(n):
    """generated quantities that store one scalar draw per element of a
    vector of n elements."""
    source = """data { int N; }
parameters { real theta; }
model { theta ~ normal(0, 1); }
generated quantities {
  vector[N] yrep;
  for (n in 1:N) yrep[n] = normal_rng(theta, 1);
}
"""
    return source, {"N": n}


def check_reroll_rng_draws(measure):
    small_n, large_n = 2000, 4000
    small = measure("rng_small", *rng_draws(small_n))["lane_steps"]
    large = measure("rng_large", *rng_draws(large_n))["lane_steps"]
    small, large = small["write_array"], large["write_array"]
    print(f"reroll over scalar draws: {small} lane steps for {small_n} "
          f"draws, {large} for {large_n}")
    if large > 3 * small:
        return [f"twice the draws made reroll take {large / small:.1f} times "
                f"the lane steps"]
    return []


CASES = [check_ifs_in_retained_loop, check_calls_in_retained_loop,
         check_refused_retention, check_write_array_lowered_once,
         check_reroll_rng_draws]
RUN_CASES = [check_reduce_sum_beside_data]


def main(bench, check, stanli_run):
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)

        def measure(name, source, data):
            return work(bench, check, directory, name, source, data)

        def run_measure(name, source, data):
            return run_work(stanli_run, directory, name, source, data,
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
