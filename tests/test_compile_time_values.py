#!/usr/bin/env python3
"""Values that lowering computes at compile time agree with CmdStan.

Each model in tests/compile_time_values/ is compiled with the passes
listed for it. log_prob and its gradient at mu = -0.04 (stanli_check point
1) must match CmdStan built from the same source with stanc's defaults.

  test_compile_time_values.py STANLI_CHECK STANLI_COMPILE
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

DIRECTORY = Path(__file__).resolve().parent / "compile_time_values"

# (lp, gradient) from CmdStan 2.40 with tools/ref_driver.cpp at point 1.
# O1 may copy the literal in propto_var_local_branch into the `_lupdf`
# call, and CmdStan then drops the term, so only its unoptimized MIR has
# CmdStan's unoptimized answer.
CMDSTAN = {
    "normalized_udf_lupdf": ((0.5476172947105451, 0.04), ("none", "o1")),
    "propto_var_local_branch": ((0.99919999999999998, 0.04), ("none",)),
}


def ordered(x):
    bits = struct.unpack("<q", struct.pack("<d", x))[0]
    return bits if bits >= 0 else -(bits & 0x7FFFFFFFFFFFFFFF)


def close(got, want):
    return abs(ordered(got) - ordered(want)) <= 10


def main(check, compile_):
    failures = 0
    with tempfile.TemporaryDirectory() as work:
        data = Path(work) / "data.json"
        data.write_text("{}\n")
        for name, (want, pass_sets) in CMDSTAN.items():
            stan = DIRECTORY / f"{name}.stan"
            for passes in pass_sets:
                mir = Path(work) / f"{name}-{passes}.mir"
                with mir.open("w") as out:
                    subprocess.run([compile_, "--passes", passes, str(stan)],
                                   stdout=out, check=True)
                result = subprocess.run(
                    [check, str(stan), str(data), "--mir", str(mir),
                     "--point", "1"], capture_output=True, text=True)
                line = next((l for l in result.stdout.splitlines()
                             if l.startswith(("OK ", "COMPILE_FAIL",
                                              "EVAL_FAIL"))), result.stdout)
                fields = line.split()
                got = (tuple(float(x) for x in fields[1:3])
                       if fields[:1] == ["OK"] and len(fields) == 3 else None)
                if got is None or not all(map(close, got, want)):
                    failures += 1
                    print(f"FAIL {name} --passes {passes}: got {line.strip()}, "
                          f"CmdStan lp {want[0]!r} gradient {want[1]!r}")
                else:
                    print(f"ok {name} --passes {passes}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
