"""A data-only call in transformed parameters is evaluated once per graph.

Preparation evaluates data-only values at compile time. A call that only
reads data should cost about the same to prepare in transformed parameters
as in transformed data, apart from lowering it once for log_prob and once
for write_array. The model repeats an expensive call, so evaluating it a
second time in each graph shows up as twice the preparation time.

  test_prep_data_calls.py STANC BENCH_GRAD DIR
"""
import subprocess
import sys
from pathlib import Path

CALLS = 100
REPEATS = 5
# Two graphs that each evaluate every call once take about twice as long as
# transformed data; evaluating each call twice per graph takes four times.
MAX_RATIO = 3.0

PARAMETERS = "parameters { real mu; }\n"
MODEL = "model { mu ~ normal(0, 1); }\n"


def calls():
    return "\n".join(f"  real t{i} = beta_neg_binomial_cdf(6 | 6, {2.9 + i / 100}, 6);"
                     for i in range(CALLS))


MODELS = {
    "transformed data": "transformed data {\n" + calls() + "\n}\n" + PARAMETERS + MODEL,
    "transformed parameters": PARAMETERS + "transformed parameters {\n" + calls() + "\n}\n" + MODEL,
}


def prep_seconds(bench, mir, data):
    best = float("inf")
    for _ in range(REPEATS):
        out = subprocess.run([bench, str(mir), str(data), "--prep", "--parallel-wa=off"],
                             check=True, capture_output=True, text=True).stdout
        best = min(best, float(out.split()[0]))
    return best


def main(stanc, bench, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    data = directory / "data.json"
    data.write_text("{}\n")
    seconds = {}
    for index, (block, source) in enumerate(MODELS.items()):
        stan = directory / f"model{index}.stan"
        stan.write_text(source)
        mir = directory / f"model{index}.sexp"
        mir.write_text(subprocess.run([stanc, "--O1", "--debug-optimized-mir", str(stan)],
                                      check=True, capture_output=True, text=True).stdout)
        seconds[block] = prep_seconds(bench, mir, data)
        print(f"{block}: {seconds[block]:.4f}s")
    ratio = seconds["transformed parameters"] / seconds["transformed data"]
    print(f"ratio {ratio:.1f}")
    if ratio > MAX_RATIO:
        sys.exit(f"transformed parameters cost {ratio:.1f}x transformed data to prepare")
    print("PASS: each data-only call is evaluated once per graph")


if __name__ == "__main__":
    main(*sys.argv[1:])
