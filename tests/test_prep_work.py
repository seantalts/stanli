"""Preparation work must not grow with the wrong part of a model.

Each case prepares two sizes of one model shape and reads work counters that
STANLI_PROFILE_PREP=1 prints: how many statements lowering processed and how
many bytes of data it copied, how many adjoints it generated for
region-map bodies, and how many function bodies and statements the
write_array control scan entered (`stage=lower_work`), and how many lane steps reroll took
(`stage=reroll ... lane_steps`). The counters do not depend on
the clock, so the bounds are exact consequences of the algorithm, not timer
noise.

  test_prep_work.py BENCH_GRAD STANLI_CHECK STANLI_RUN
"""
import json
import math
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
BIND = re.compile(r"stanli_prep graph=(\S+) stage=bind_data ")
REROLL = re.compile(r"stanli_prep graph=(\S+) stage=reroll ns=\d+ .*?"
                    r"lane_steps=(\d+)")
ADJOINTS = re.compile(r"stanli_prep graph=(\S+) stage=lower_work ns=\d+ .*?"
                      r"region_map_adjoints=(\d+)")
CALL_SCANS = re.compile(r"stanli_prep graph=(\S+) stage=lower_work ns=\d+ .*?"
                        r"call_scans=(\d+)")
SCAN_STATEMENTS = re.compile(
    r"stanli_prep graph=(\S+) stage=lower_work ns=\d+ .*?"
    r"scan_statements=(\d+)")


def work(bench, check, directory, name, source, data, switches=None):
    """{graph: (statements, copied bytes)} for the last row of each graph,
    with `switches` set in the environment. "lowerings" counts the lowerings
    of each graph that finished and "starts" those that began. The model gets
    the MIR fixtures' O1 passes (tools/gen_fixtures.py)."""
    stan = directory / f"{name}.stan"
    stan.write_text(source)
    mir = subprocess.run(
        [check, "--emit-mir", str(stan), "--passes", O1_PASSES],
        check=True, capture_output=True, text=True).stdout
    mir_path = directory / f"{name}.mir"
    mir_path.write_text(mir)
    data_path = directory / f"{name}.json"
    data_path.write_text(json.dumps(data))
    env = dict(os.environ, STANLI_PROFILE_PREP="1", **(switches or {}))
    run = subprocess.run([bench, str(mir_path), str(data_path), "--prep",
                          "--parallel-wa=off"],
                         check=True, capture_output=True, text=True, env=env)
    rows, lowerings, lane_steps = {}, {}, {}
    for graph, statements, copied in WORK.findall(run.stdout + run.stderr):
        rows[graph] = (int(statements), int(copied))
        lowerings[graph] = lowerings.get(graph, 0) + 1
    for graph, steps in REROLL.findall(run.stdout + run.stderr):
        lane_steps[graph] = int(steps)
    adjoints = {graph: int(count)
                for graph, count in ADJOINTS.findall(run.stdout + run.stderr)}
    rows["lowerings"] = lowerings
    starts = {}
    for graph in BIND.findall(run.stdout + run.stderr):
        starts[graph] = starts.get(graph, 0) + 1
    rows["starts"] = starts
    rows["lane_steps"] = lane_steps
    rows["region_map_adjoints"] = adjoints
    rows["call_scans"] = {
        graph: int(count)
        for graph, count in CALL_SCANS.findall(run.stdout + run.stderr)}
    rows["scan_statements"] = {
        graph: int(count)
        for graph, count in SCAN_STATEMENTS.findall(run.stdout + run.stderr)}
    if not {"log_prob", "bounded_log_prob", "budget_log_prob"} & rows.keys():
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


def region_map_body(k):
    """A loop that a region map can take, with a branch on the parameter
    and k more statements that set the size of the map's body."""
    lines = "\n".join(
        f"    s = s + 0.001 * exp(theta * x[n] * {i % 97 + 1});"
        for i in range(k))
    source = f"""data {{ int N; vector[N] x; }}
parameters {{ real theta; }}
model {{
  theta ~ normal(0, 1);
  for (n in 1:N) {{
    real s = x[n];
    if (s < theta) s = s + 1;
{lines}
    target += -square(s);
  }}
}}
"""
    return source, {"N": 6, "x": [0.1 * i for i in range(6)]}


def check_oversized_region_map(measure):
    small = measure("rmap_small", *region_map_body(10))
    large = measure("rmap_large", *region_map_body(4000))
    # The bounded-unroll trial shares the trace, so the last row of either
    # log-density graph holds the total.
    small = max(small["region_map_adjoints"].values())
    large = max(large["region_map_adjoints"].values())
    print(f"region-map adjoints: {small} for 10 statements, {large} for 4000")
    if small == 0:
        return ["the small region-map body got no adjoint"]
    if large != 0:
        return [f"a region-map body already too long got {large} adjoints"]
    return []


def same_call_in_generated_loop(n):
    """generated quantities with a loop of n trips that calls one function
    in every trip, with arguments that the scan knows the same of. The
    overload keeps stanc from inlining the function."""
    source = """functions {
  real total(real h, vector v) {
    real s = 0;
    for (i in 1:num_elements(v)) s += -h * exp(v[i]);
    return s;
  }
  real total(real h, real v) { return -h * exp(v); }
}
data { int N; int K; vector[K] x; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  array[N] real y;
  {
    vector[K] eta = mu + x;
    for (n in 1:N) {
      real h = mu * n;
      y[n] = total(h, eta);
    }
  }
}
"""
    return source, {"N": n, "K": 4, "x": [0.1, 0.2, 0.3, 0.4]}


def check_call_scans(measure):
    small_n, large_n = 200, 2000
    small = measure("scan_small", *same_call_in_generated_loop(small_n))
    large = measure("scan_large", *same_call_in_generated_loop(large_n))
    small = small["call_scans"]["write_array"]
    large = large["call_scans"]["write_array"]
    print(f"write_array control scan: {small} function bodies for {small_n} "
          f"trips, {large} for {large_n}")
    if small == 0:
        return ["the write_array control scan entered no function body"]
    if large > 3 * small:
        return [f"{large_n // small_n} times the trips made the scan enter "
                f"{large} function bodies instead of {small}"]
    return []


def loop_without_trip_control(n):
    """generated quantities with a loop of n trips whose body decides
    nothing by the trip."""
    source = """data { int N; int K; vector[K] x; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  array[N] real y;
  {
    vector[K] eta = mu + x;
    for (n in 1:N) {
      real h = mu * n;
      real s = 0;
      for (k in 1:K) s += -h * exp(eta[k]);
      y[n] = s;
    }
  }
}
"""
    return source, {"N": n, "K": 4, "x": [0.1, 0.2, 0.3, 0.4]}


def check_scan_once_per_loop(measure):
    small_n, large_n = 200, 2000
    small = measure("trips_small", *loop_without_trip_control(small_n))
    large = measure("trips_large", *loop_without_trip_control(large_n))
    small = small["scan_statements"]["write_array"]
    large = large["scan_statements"]["write_array"]
    print(f"write_array control scan of a loop: {small} statements for "
          f"{small_n} trips, {large} for {large_n}")
    if large > 3 * small:
        return [f"{large_n // small_n} times the trips made the scan visit "
                f"{large} statements instead of {small}"]
    return []


def search_in_called_function(n):
    """generated quantities with a loop of n trips that calls a function
    with a search loop over 200 data rows, for a different target in each
    trip. The overload keeps stanc from inlining the function."""
    source = """functions {
  real find(array[] int ids, int wanted, vector v) {
    real out = 0;
    int done = 0;
    for (r in 1:size(ids)) {
      if (!done && ids[r] == wanted) {
        out = v[r];
        done = 1;
      }
    }
    return out;
  }
  real find(int ids, int wanted, real v) { return v; }
}
data { int N; int M; array[M] int ids; vector[M] x; }
parameters { real mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  array[N] real y;
  {
    vector[M] eta = mu + x;
    for (n in 1:N) {
      real h = mu * n;
      y[n] = h + find(ids, n, eta);
    }
  }
}
"""
    m = 200
    return source, {"N": n, "M": m, "ids": [i % 997 + 1 for i in range(m)],
                    "x": [0.001 * i for i in range(m)]}


def check_scan_of_calls_stops(measure):
    small_n, large_n = 400, 800
    small = measure("search_small", *search_in_called_function(small_n))
    large = measure("search_large", *search_in_called_function(large_n))
    small = small["scan_statements"]["write_array"]
    large = large["scan_statements"]["write_array"]
    print(f"write_array control scan of a search in a called function: "
          f"{small} statements for {small_n} trips, {large} for {large_n}")
    if large > 1.5 * small:
        return [f"twice the trips made the scan visit {large} statements "
                f"instead of {small}"]
    return []


def varied_rows(n):
    """n data values with no repeats, so that no two rows fold together."""
    return [0.9 * math.sin(7.0 * i + 1.0) + i / n for i in range(n)]


def mixture_loops(loops, n):
    """`loops` loops over n rows whose body reroll declines, so each one
    unrolls to about nine ops a row."""
    body = "\n".join(
        f"""  for (n in 1:N)
    target += {k + 1} * log_sum_exp(
        log(theta) + normal_lpdf(y[n] | mu[1], sigma),
        log1m(theta) + normal_lpdf(y[n] | mu[2], sigma));"""
        for k in range(loops))
    source = f"""data {{ int N; vector[N] y; }}
parameters {{
  real<lower=0, upper=1> theta;
  ordered[2] mu;
  real<lower=0> sigma;
}}
model {{
  mu ~ normal(0, 3);
  sigma ~ exponential(1);
{body}
}}
"""
    return source, {"N": n, "y": varied_rows(n)}


def carried_loop(n):
    """A loop that carries a value from one row to the next, which the
    region map cannot take."""
    source = """data { int N; vector[N] y; }
parameters { real<lower=-1, upper=1> rho; real<lower=0> sigma; }
model {
  real m = 0;
  for (n in 1:N) {
    target += normal_lpdf(y[n] | rho * m, sigma);
    m = 0.5 * m + rho * y[n];
  }
}
"""
    return source, {"N": n, "y": varied_rows(n)}


# The program budget lowers log_prob a second time as budget_log_prob.
SMALL_BUDGET = {"STANLI_PROGRAM_BUDGET": "65536",
                "STANLI_LOOP_BUDGET": "1000000000"}


def check_budget_refused_loop_stops(measure):
    # With the lane form off, the loop that the budget chooses has no
    # compact form, so the second lowering stops at it.
    rows = measure("budget_refused", *mixture_loops(1, 256),
                   switches=dict(SMALL_BUDGET, STANLI_REGION_MAP_LANES="0"))
    started = rows["starts"].get("budget_log_prob", 0)
    finished = rows["lowerings"].get("budget_log_prob", 0)
    print(f"budget, no compact form: second lowering started {started} "
          f"times, finished {finished}")
    if started != 1 or finished != 0:
        return [f"a chosen loop with no compact form: the second lowering "
                f"started {started} times and finished {finished}"]
    # The first graph holds little data, so it is kept, not lowered again.
    first = rows["lowerings"].get("log_prob", 0)
    print(f"budget, no compact form: log_prob lowered {first} times")
    if first != 1:
        return [f"a first graph that holds little data was lowered {first} "
                f"times"]
    # Past STANLI_FREE_FIRST_BYTES the first graph is freed and lowered a
    # third time, and the result is the same model.
    freed = measure("budget_refused_freed", *mixture_loops(1, 256),
                    switches=dict(SMALL_BUDGET, STANLI_REGION_MAP_LANES="0",
                                  STANLI_FREE_FIRST_BYTES="0"))
    again = freed["lowerings"].get("log_prob", 0)
    print(f"budget, no compact form, first graph freed: log_prob lowered "
          f"{again} times")
    if again != 2:
        return [f"a freed first graph was lowered {again} times, not 2"]
    return []


def check_budget_skips_loops_without_lanes(measure):
    # Under the OP_LOOP budget, a loop that the region map cannot take is
    # not lowered again.
    rows = measure("budget_carried", *carried_loop(2000),
                   switches=SMALL_BUDGET)
    started = rows["starts"].get("budget_log_prob", 0)
    print(f"budget, carried loop: second lowering started {started} times")
    if started != 0:
        return [f"a loop without the lane form started {started} second "
                f"lowerings under the OP_LOOP budget"]
    return []


CEILING_NO_COMPACT = {"STANLI_PROGRAM_BUDGET": "0",
                      "STANLI_UNROLL_CEILING": "100",
                      "STANLI_REGION_MAP": "0",
                      "STANLI_STRUCTURED_LOOPS": "0"}


def check_unroll_ceiling_one_retry(measure):
    # One loop past the unroll ceiling with no compact form: the retry
    # unrolls it without a ceiling, so it is the only retry.
    rows = measure("ceiling_loop", *mixture_loops(1, 200),
                   switches=CEILING_NO_COMPACT)
    started = rows["starts"].get("budget_log_prob", 0)
    print(f"one loop past the unroll ceiling: {started} retries")
    if started != 1:
        return [f"one loop past the unroll ceiling took {started} retries, "
                f"not 1"]
    return []


def check_unroll_ceiling_retries_bounded(measure):
    # Ten loops past the unroll ceiling with no compact form: each retry
    # finds one more, and the eighth retry has no ceiling.
    rows = measure("ceiling_loops", *mixture_loops(10, 200),
                   switches=CEILING_NO_COMPACT)
    started = rows["starts"].get("budget_log_prob", 0)
    print(f"ten loops past the unroll ceiling: {started} retries")
    if started != 8:
        return [f"ten loops past the unroll ceiling took {started} retries, "
                f"not 8"]
    return []


CASES = [check_ifs_in_retained_loop, check_calls_in_retained_loop,
         check_refused_retention, check_write_array_lowered_once,
         check_reroll_rng_draws, check_oversized_region_map,
         check_call_scans, check_scan_once_per_loop, check_scan_of_calls_stops,
         check_budget_refused_loop_stops,
         check_budget_skips_loops_without_lanes,
         check_unroll_ceiling_one_retry,
         check_unroll_ceiling_retries_bounded]
RUN_CASES = [check_reduce_sum_beside_data]


def main(bench, check, stanli_run):
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)

        def measure(name, source, data, switches=None):
            return work(bench, check, directory, name, source, data,
                        switches)

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
