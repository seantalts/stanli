#!/usr/bin/env python3
"""Writes the .stan files of tests/stanguide from a checkout of stan-dev/docs.

Each program is a Stan code block of the Stan User's Guide (src/stan-users-guide/*.qmd),
or two blocks of one chapter joined in order, with the edits listed in EDITS
and nothing else. The guide's prose is not copied. The checkout must be at
the commit below; the data files come from tools/gen_stanguide_data.R.

    git clone https://github.com/stan-dev/docs && git -C docs checkout b07b2a99457cd04cdcded3ea9e85def099290328
    python3 tools/gen_stanguide_models.py docs [outdir]     # default tests/stanguide
"""
import pathlib
import re
import subprocess
import sys

COMMIT = "b07b2a99457cd04cdcded3ea9e85def099290328"
BLOCK = re.compile(r"```\{?\.?stan[^\n]*\n(.*?)```", re.S)

# model id -> (chapter, [Stan code block numbers in that chapter, counting
# from 0 over every ```stan block], [(old, new) text edits])
MODELS = {
    "changepoint": ("latent-discrete", [0], []),
    "cjs_history": ("latent-discrete", [5], []),
    "map_rect_logistic": ("parallelization", [19], []),
    "integrate_1d_normal": ("one-dimensional-integrals", [0, 1], []),
    "ode_adjoint": ("odes", [7], [
        ("ode_rk45(sho, y0, t0, ts, theta);", "@ADJOINT@"),
    ]),
    "copula": ("copulas", [None], [
        ("  vector<lower=0>[D] y[N];  // data",
         "  array[N] vector<lower=0>[D] y;  // data"),
    ]),
}
# Blocks picked by a string they contain rather than a number, for the
# chapters where counting blocks is fragile.
PICK = {
    "copula": ["exponential_cdf(y[n, d]"],
}
ADJOINT_CHAPTER = "odes"
ADJOINT_MARK = "ode_adjoint_tol_ctl(sho, y0, t0, ts,"
LITERALS = (("relative_tolerance", "1e-6"), ("absolute_tolerance", "1e-6"),
            ("max_num_steps", "1000000"))


def blocks(chapter):
    text = (SRC / "src" / "stan-users-guide" / f"{chapter}.qmd").read_text()
    return [m.group(1) for m in BLOCK.finditer(text)]


def adjoint_call(chapter_blocks):
    code = next(b for b in chapter_blocks if ADJOINT_MARK in b)
    call = code[code.index(ADJOINT_MARK):code.rindex(";") + 1]
    call = re.sub(r"\s*//[^\n]*", "", call)
    for name, value in LITERALS:
        call = call.replace(name, value)
    return re.sub(r"\s+", " ", call).replace("( ", "(")


def build(model):
    chapter, numbers, edits = MODELS[model]
    chapter_blocks = blocks(chapter)
    if model in PICK:
        parts = [next(b for b in chapter_blocks if key in b) for key in PICK[model]]
    else:
        parts = [chapter_blocks[n] for n in numbers]
    if model == "integrate_1d_normal":
        parts[0] = "functions {\n" + "".join("  " + l if l.strip() else l
                                             for l in parts[0].splitlines(True)) + "}\n"
    code = "\n".join(p.rstrip("\n") + "\n" for p in parts)
    for old, new in edits:
        if old not in code:
            raise SystemExit(f"{model}: edit target not found: {old!r}")
        if new == "@ADJOINT@":
            new = adjoint_call(blocks(ADJOINT_CHAPTER))
        code = code.replace(old, new)
    return code


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    SRC = pathlib.Path(sys.argv[1])
    head = subprocess.run(["git", "-C", str(SRC), "rev-parse", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    if head != COMMIT:
        raise SystemExit(f"{SRC} is at {head}, expected {COMMIT}")
    out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "tests/stanguide")
    out.mkdir(parents=True, exist_ok=True)
    for model in MODELS:
        (out / f"stanguide_{model}.stan").write_text(build(model))
