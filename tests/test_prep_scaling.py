"""Preparation time must grow linearly with the number of statements.

Each model repeats one statement shape N times in a section that is lowered
statement by statement. Growing N eightfold should grow preparation time
about eightfold; a quadratic cost would multiply it by sixty-four. The bound
sits between the two, so it tolerates timer noise on a busy CI runner but not
a per-statement copy of the lowering state.

Compiling the models with stanc takes most of the time, and only the
preparation timings need a quiet machine, so the script has two modes:

  test_prep_scaling.py compile STANC DIR   writes DIR/model<i>-<N>.sexp
  test_prep_scaling.py time BENCH_GRAD DIR times preparation of each one
"""
from concurrent.futures import ThreadPoolExecutor
import shutil
import subprocess
import sys
from pathlib import Path

SMALL, LARGE = 400, 3200
REPEATS = 5
MAX_RATIO = 20.0

PREAMBLE = "parameters { real mu; }\nmodel { mu ~ normal(0, 1); }\n"


def generated_quantities(statement):
    def build(n):
        body = "\n".join(statement(i) for i in range(n))
        return PREAMBLE + "generated quantities {\n" + body + "\n}\n"
    return build


def constrained_parameters(n):
    declarations = "\n".join(f"real<lower=0, upper=1> p{i};" for i in range(n))
    return "parameters {\n" + declarations + "\n}\nmodel { p0 ~ beta(2, 2); }\n"


def transformed_parameter_chain(n):
    body = "real t0 = mu;\n" + "\n".join(f"real t{i} = t{i - 1} + 1;" for i in range(1, n))
    return ("parameters { real mu; }\ntransformed parameters {\n" + body +
            "\n}\nmodel { mu ~ normal(0, 1); }\n")


MODELS = {
    "scalar generated quantities": generated_quantities(lambda i: f"real g{i} = mu + {i};"),
    "generated quantities for loops": generated_quantities(
        lambda i: f"array[4] real a{i};\nfor (k in 1:4) a{i}[k] = mu * k + {i};"),
    "generated quantities if/else": generated_quantities(
        lambda i: f"real h{i};\nif (mu > {i % 7}) h{i} = mu + {i}; else h{i} = mu - {i};"),
    "generated quantities while loops": generated_quantities(
        lambda i: f"real w{i} = mu;\n{{ int c{i} = 0; while (c{i} < 3) {{ w{i} += mu; c{i} += 1; }} }}"),
    "generated quantities nested control and rng": generated_quantities(
        lambda i: (f"array[3] real d{i};\n"
                   f"for (n in 1:3) {{ real step = 0; int k = 0; "
                   f"while (k < 2) {{ step += mu; k += 1; }} "
                   f"if (step > 0) d{i}[n] = normal_rng(mu, 1.0); "
                   f"else d{i}[n] = normal_rng(-mu, 1.0); }}")),
    "constrained parameters": constrained_parameters,
    "transformed parameter chain": transformed_parameter_chain,
}


def mir_path(directory, index, n):
    return directory / f"model{index}-{n}.sexp"


def compile_models(stanc, directory):
    directory = Path(directory)
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    (directory / "data.json").write_text("{}\n")

    def compile_one(job):
        index, build, n = job
        stan = directory / f"model{index}-{n}.stan"
        stan.write_text(build(n))
        mir = subprocess.run([stanc, "--O1", "--debug-optimized-mir", str(stan)],
                             check=True, capture_output=True, text=True).stdout
        mir_path(directory, index, n).write_text(mir)

    # stanc's optimizer is superlinear, so start the large models first.
    jobs = [(index, build, n) for n in (LARGE, SMALL)
            for index, build in enumerate(MODELS.values())]
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(compile_one, jobs))
    print(f"compiled {len(jobs)} models into {directory}")


def prep_seconds(bench, directory, index, n):
    best = float("inf")
    for _ in range(REPEATS):
        out = subprocess.run([bench, str(mir_path(directory, index, n)),
                              str(directory / "data.json"), "--prep"],
                             check=True, capture_output=True, text=True).stdout
        best = min(best, float(out.split()[0]))
    return best


def time_models(bench, directory):
    directory = Path(directory)
    failures = []
    for index, name in enumerate(MODELS):
        small = prep_seconds(bench, directory, index, SMALL)
        large = prep_seconds(bench, directory, index, LARGE)
        ratio = large / small
        print(f"{name}: {SMALL} -> {small:.4f}s, {LARGE} -> {large:.4f}s, ratio {ratio:.1f}")
        if ratio > MAX_RATIO:
            failures.append(f"{name}: {LARGE // SMALL}x more statements cost {ratio:.1f}x more")
    if failures:
        sys.exit("superlinear preparation:\n  " + "\n  ".join(failures))
    print("PASS: preparation time is linear in statement count")


if __name__ == "__main__":
    mode, tool, directory = sys.argv[1:]
    {"compile": compile_models, "time": time_models}[mode](tool, directory)
