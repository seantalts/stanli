#!/usr/bin/env python3
"""Generate the propto fixtures: densities under ~ with data arguments.

Under ~, Stan Math drops the terms of a density whose arguments are all
data. tests/fixtures/propto_data_arguments.stan and propto_multi_gp.stan
call each density below once for every split of its arguments into data
and parameters (only the all-data call for the densities in ALL_DATA_ONLY).
Their references are CmdStan's, recorded with
tools/verify_sample.py deps/cmdstan --corpus fixtures --model NAME.

  python3 tools/gen_propto_fixtures.py          # write
  python3 tools/gen_propto_fixtures.py --check  # freshness gate
"""
import argparse
import itertools
import json
import pathlib
import sys

FIXTURES = pathlib.Path(__file__).resolve().parent.parent / "tests" / "fixtures"

SPD = [[2.0, 0.3, 0.1], [0.3, 1.5, 0.2], [0.1, 0.2, 1.2]]
SPD2 = [[1.3, -0.2, 0.0], [-0.2, 0.9, 0.1], [0.0, 0.1, 2.1]]
CHOL = [[1.4, 0, 0], [0.2, 1.2, 0], [0.1, -0.3, 0.9]]
CHOL2 = [[1.1, 0, 0], [-0.1, 0.8, 0], [0.3, 0.2, 1.5]]
CORR = [[1.0, 0.3, 0.1], [0.3, 1.0, 0.2], [0.1, 0.2, 1.0]]
LCORR = [[1.0, 0, 0], [0.3, 0.9539392014169457, 0],
         [0.1, 0.2, 0.9746794344808963]]
V3 = [0.3, -0.5, 1.1]
V3B = [0.1, 0.2, -0.4]
POS3 = [0.7, 1.3, 2.2]
SIMPLEX = [0.2, 0.3, 0.5]
COUNTS = [2, 0, 5]
# y is 3 x 4 so multi_gp's Sigma (4 x 4) differs in size from w (3).
YKN = [[0.1 * (i - j) + 0.05 * i * j for j in range(4)] for i in range(3)]
SPDN = [[2.0 if i == j else 0.3 / (1 + abs(i - j)) for j in range(4)]
        for i in range(4)]
CHOLN = [[1.2 if i == j else (0.1 * (i - j) if i > j else 0.0)
          for j in range(4)] for i in range(4)]


def wishart(chol):
    m = "cholesky_factor_cov[3]" if chol else "cov_matrix[3]"
    return [("W", m, m, CHOL if chol else SPD),
            ("nu", "real<lower=3>", "real", 5.0),
            ("S", m, m, CHOL2 if chol else SPD2)]


def multi_gp(chol):
    m = "cholesky_factor_cov[4]" if chol else "cov_matrix[4]"
    return [("y", "matrix[3,4]", "matrix[3,4]", YKN),
            ("S", m, m, CHOLN if chol else SPDN),
            ("w", "vector<lower=0>[3]", "vector[3]", POS3)]


# name -> [(argument, parameter type, data type, data value)]. A parameter
# type of None marks an argument that is always data.
DENSITIES = {
    "wishart": wishart(False),
    "inv_wishart": wishart(False),
    "wishart_cholesky": wishart(True),
    "inv_wishart_cholesky": wishart(True),
    "multi_gp": multi_gp(False),
    "multi_gp_cholesky": multi_gp(True),
    "lkj_cov": [("W", "cov_matrix[3]", "cov_matrix[3]", SPD),
                ("mu", "vector[3]", "vector[3]", V3B),
                ("sigma", "vector<lower=0>[3]", "vector[3]", POS3),
                ("eta", "real<lower=0>", "real", 2.5)],
    "lkj_corr": [("O", "corr_matrix[3]", "corr_matrix[3]", CORR),
                 ("eta", "real<lower=0>", "real", 2.5)],
    "lkj_corr_cholesky": [("L", "cholesky_factor_corr[3]",
                           "cholesky_factor_corr[3]", LCORR),
                          ("eta", "real<lower=0>", "real", 2.5)],
    "multinomial": [("n", None, "array[3] int", COUNTS),
                    ("theta", "simplex[3]", "simplex[3]", SIMPLEX)],
    "multinomial_logit": [("n", None, "array[3] int", COUNTS),
                          ("g", "vector[3]", "vector[3]", V3)],
    "dirichlet_multinomial": [("n", None, "array[3] int", COUNTS),
                              ("alpha", "vector<lower=0>[3]", "vector[3]",
                               POS3)],
    "wiener": [("y", "real", "real", 1.2), ("alpha", "real", "real", 1.5),
               ("tau", "real", "real", 0.3), ("beta", "real", "real", 0.5),
               ("delta", "real", "real", 0.4)],
}
# These were wrong only when every argument was data.
ALL_DATA_ONLY = ("lkj_corr", "lkj_corr_cholesky", "multinomial",
                 "multinomial_logit", "dirichlet_multinomial", "wiener")
FIXTURE_DENSITIES = {
    "propto_data_arguments": [
        "wishart", "inv_wishart", "wishart_cholesky", "inv_wishart_cholesky",
        "lkj_cov", "lkj_corr", "lkj_corr_cholesky", "multinomial",
        "multinomial_logit", "dirichlet_multinomial", "wiener"],
    "propto_multi_gp": ["multi_gp", "multi_gp_cholesky"],
}


def fixture(names):
    """(Stan source, JSON data) for one fixture."""
    data_decl, par_decl, model, data = [], [], [], {}
    for name in names:
        args = DENSITIES[name]
        free = [i for i, a in enumerate(args) if a[1] is not None]
        splits = [()] if name in ALL_DATA_ONLY else [
            c for r in range(len(free) + 1)
            for c in itertools.combinations(free, r)]
        model.append(f"  // {name}")
        for split in splits:
            tag = "".join("p" if i in split else "d" for i in range(len(args)))
            used = []
            for i, (arg, par_type, data_type, value) in enumerate(args):
                if i in split:
                    var = f"{arg}_{tag}_{name}"
                    par_decl.append(f"  {par_type} {var};")
                else:
                    var = f"{arg}_{name}"
                    if var not in data:
                        data_decl.append(f"  {data_type} {var};")
                        data[var] = value
                used.append(var)
            model.append(f"  {used[0]} ~ {name}({', '.join(used[1:])});")
    src = ("// Each data/parameter split of these densities under ~. Stan Math"
           " drops\n// the terms whose arguments are all data, so stanli has "
           "to as well.\n"
           "data {\n" + "\n".join(data_decl) + "\n}\nparameters {\n" +
           "\n".join(par_decl) + "\n}\nmodel {\n" + "\n".join(model) + "\n}\n")
    return src, json.dumps(data, indent=1) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="fail if a checked-in fixture differs")
    args = ap.parse_args()
    stale = []
    for name, densities in FIXTURE_DENSITIES.items():
        for suffix, text in zip((".stan", ".json"), fixture(densities)):
            path = FIXTURES / f"{name}{suffix}"
            if args.check:
                if not path.exists() or path.read_text() != text:
                    stale.append(path.name)
            else:
                path.write_text(text)
    if stale:
        print("stale, rerun tools/gen_propto_fixtures.py: " + " ".join(stale))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
