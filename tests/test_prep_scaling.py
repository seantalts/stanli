"""Preparation time must grow linearly with the number of statements.

Each model repeats one statement shape N times in a section that is lowered
statement by statement. Quadrupling N should roughly quadruple preparation
time; a quadratic cost would multiply it by sixteen. The bound sits between
the two, so it tolerates timer noise but not a per-statement copy of the
lowering state.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

SMALL, LARGE = 300, 1200
REPEATS = 3
MAX_RATIO = 9.0

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
    "constrained parameters": constrained_parameters,
    "transformed parameter chain": transformed_parameter_chain,
}


def prep_seconds(bench, stanc, directory, source):
    stan = directory / "model.stan"
    data = directory / "data.json"
    stan.write_text(source)
    data.write_text("{}\n")
    mir = subprocess.run([stanc, "--O1", "--debug-optimized-mir", str(stan)],
                         check=True, capture_output=True, text=True).stdout
    (directory / "model.sexp").write_text(mir)
    best = float("inf")
    for _ in range(REPEATS):
        out = subprocess.run([bench, str(directory / "model.sexp"), str(data), "--prep"],
                             check=True, capture_output=True, text=True).stdout
        best = min(best, float(out.split()[0]))
    return best


def main(bench, stanc):
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        for name, build in MODELS.items():
            directory = Path(scratch)
            small = prep_seconds(bench, stanc, directory, build(SMALL))
            large = prep_seconds(bench, stanc, directory, build(LARGE))
            ratio = large / small
            print(f"{name}: {SMALL} -> {small:.4f}s, {LARGE} -> {large:.4f}s, ratio {ratio:.1f}")
            if ratio > MAX_RATIO:
                failures.append(f"{name}: {LARGE // SMALL}x more statements cost {ratio:.1f}x more")
    if failures:
        sys.exit("superlinear preparation:\n  " + "\n  ".join(failures))
    print("PASS: preparation time is linear in statement count")


if __name__ == "__main__":
    main(*sys.argv[1:])
