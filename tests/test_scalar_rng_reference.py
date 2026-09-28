"""Replay complete stochastic rows independently recorded from CmdStan."""
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from verify_refs import parse_status, parse_wa


def ordered(value):
    bits = struct.unpack(">Q", struct.pack(">d", value))[0]
    return (~bits & ((1 << 64) - 1)) if bits >> 63 else bits | (1 << 63)


def main(check, fixture="gq_scalar_rng_complete", expected_interpreter="0"):
    source = ROOT / "tests/fixtures" / (fixture + ".stan")
    reference = json.loads(source.with_suffix(".ref.json").read_text())
    for path, key in [(source, "source_sha256"), (source.with_suffix(".json"), "data_sha256")]:
        assert hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest() == reference[key]
    assert set(reference["points"]) == {"0", "1", "2"}
    assert reference["seed"] == 1234 and reference["chain"] == 0
    worst = 0
    count = 0
    for point, row in reference["points"].items():
        out = subprocess.run([check, str(source), str(source.with_suffix(".json")),
                              "--mir", str(source.with_suffix(".tmir.sexp")), "--wa-values", "--point", point],
                             check=True, text=True, capture_output=True)
        assert ("INTERP " in out.stderr) == (expected_interpreter == "1"), out.stderr
        fields = parse_status(out.stdout)
        assert fields[0] == "OK", out.stdout
        wa = parse_wa(out.stdout)
        assert wa and wa[0] == row["wa"]["names"], out.stdout
        for expected, got in [(row["values"], fields[1:]), (row["wa"]["values"], wa[1])]:
            assert len(expected) == len(got)
            for a, b in zip(expected, got):
                a, b = float(a), float(b)
                assert math.isfinite(a) and math.isfinite(b)
                ulp = abs(ordered(a) - ordered(b))
                assert ulp <= 10, (point, a, b, ulp)
                worst = max(worst, ulp)
                count += 1
    print(f"CmdStan {fixture} reference: {count} values, max {worst} ULP")


if __name__ == "__main__":
    main(*sys.argv[1:])
