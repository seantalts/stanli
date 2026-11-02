#!/usr/bin/env python3
"""Small programs from stanc3's good tests that stanli used to refuse.

Each case runs one program through stanli_check at point 0 and checks the
write_array values, or the message the evaluation or the data load throws.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

check = str(pathlib.Path(sys.argv[1]).resolve())

CASES = [
    ("and/or of transformed data assigned to a real",
     """transformed data { int t = 1; int f = 0; }
        parameters { real y; }
        transformed parameters {
          real a = t && t;
          real o = f || t;
          real n = f && t;
        }
        model { y ~ normal(0, 1); }""",
     {}, {"values": [0.1, 1, 1, 0]}),
    ("min/max of ints assigned to a real",
     """data { int d; array[3] int a; }
        parameters { real y; }
        transformed parameters {
          real hi = max(d, 7);
          real lo = min(d, 7);
          real ahi = max(a);
          real alo = min(a);
        }
        model { y ~ normal(0, 1); }""",
     {"d": 3, "a": [4, -2, 9]}, {"values": [0.1, 7, 3, 9, -2]}),
]


def run(source, data):
    with tempfile.TemporaryDirectory() as tmp:
        stan = pathlib.Path(tmp) / "model.stan"
        stan.write_text(source)
        js = pathlib.Path(tmp) / "data.json"
        js.write_text(json.dumps(data))
        proc = subprocess.run([check, str(stan), str(js), "--point", "0",
                               "--wa-values"],
                              capture_output=True, text=True, timeout=300)
    return proc.stdout + proc.stderr


failures = 0
for name, source, data, want in CASES:
    out = run(source, data)
    lines = {}
    for line in out.splitlines():
        key, _, rest = line.partition(" ")
        lines.setdefault(key, rest)
    ok = True
    if "values" in want:
        got = [float(v) for v in lines.get("WAVALS", "").split()]
        ok = "OK" in lines and len(got) == len(want["values"]) and all(
            abs(g - w) <= 1e-12 * max(1.0, abs(w))
            for g, w in zip(got, want["values"]))
    if "eval_fail" in want:
        ok = want["eval_fail"] in lines.get("EVAL_FAIL", "")
    if "compile_fail" in want:
        ok = want["compile_fail"] in lines.get("COMPILE_FAIL", "")
    if "stdout" in want:
        ok = ok and want["stdout"] in out
    if not ok:
        failures += 1
        print(f"FAIL {name}\n{out}")
print(f"{len(CASES) - failures}/{len(CASES)} cases OK")
sys.exit(1 if failures else 0)
