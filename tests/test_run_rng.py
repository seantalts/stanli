#!/usr/bin/env python3
"""CLI CSV and the shipped C ABI must share the same full-output RNG schedule."""
import csv
import io
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
cli = str(Path(sys.argv[1]).resolve())
capi = str(Path(sys.argv[2]).resolve())


def equal(a, b):
    return ((math.isnan(a) and math.isnan(b))
            or struct.pack("d", a) == struct.pack("d", b))


with tempfile.TemporaryDirectory() as directory:
    data = Path(directory) / "data.json"
    data.write_text("{}")
    for interpreted in (False, True):
        if interpreted:
            os.environ["STANLI_WA_FORCE_INTERP"] = "1"
        else:
            os.environ.pop("STANLI_WA_FORCE_INTERP", None)
        for fixture in ("gq_scalar_rng", "gq_rng_stream", "gq_rng_stream_reject"):
            mir = ROOT / "tests/fixtures" / (fixture + ".tmir.sexp")
            serial = None
            for threads in (1, 2):
                reference = subprocess.run([capi, str(mir), str(threads)],
                                           capture_output=True, timeout=60)
                assert reference.returncode == 0, reference.stderr.decode()
                expected = list(csv.reader(io.StringIO(reference.stdout.decode())))
                rows = (30 + 2) // 3 + (41 + 2) // 3
                count = 2 * rows
                assert len(expected) == count
                width = len(expected[0]) - 7
                assert width > 0
                source = mir.with_name(fixture + ".stan")
                command = [cli, str(source), str(data), "--seed", "23",
                           "--chains", "2", "--num-threads", str(threads),
                           "--warmup", "30", "--samples", "41", "--thin", "3",
                           "--save-warmup", "--max-depth", "5", "--init-radius", "0",
                           "--sampler-stats"]
                if fixture != "gq_rng_stream_reject":
                    command.append("--summary")
                result = subprocess.run(command, capture_output=True, timeout=60)
                assert result.returncode == 0, result.stderr.decode()
                records = list(csv.reader(io.StringIO(result.stdout.decode())))
                assert len(records) == count + 1
                assert len(records[0]) == width + 7
                for i, (record, want) in enumerate(zip(records[1:], expected)):
                    assert len(record) == len(want) == width + 7
                    assert all(equal(float(a), float(b)) for a, b in zip(record, want)), (fixture, interpreted, threads, i)
                if serial is None:
                    serial = result.stdout
                else:
                    assert result.stdout == serial, "parallel output changed chain ordering or RNG"
                # Stats and summary flags must be observational.
                bare_command = [x for x in command if x not in ("--sampler-stats", "--summary")]
                bare = subprocess.run(bare_command, capture_output=True, check=True, timeout=60)
                stripped = list(csv.reader(io.StringIO(bare.stdout.decode())))
                assert stripped == [row[7:] for row in records]
            print(f"PASS CLI/C API RNG: {fixture}, interpreted={interpreted}")

    # All header probes reject. The CLI must discover the schema at a later
    # real draw, pad earlier failures, and preserve the reference RNG schedule.
    # Rejection occurs after the only RNG call, so it cannot change that trace.
    os.environ["STANLI_WA_FORCE_INTERP"] = "1"
    template = """parameters { real x; }
model { x ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(x, 1);
  REJECTION
  real after = before + 1;
}
"""
    reference = Path(directory) / "reference.stan"
    late = Path(directory) / "late.stan"
    reference.write_text(template.replace("REJECTION", ""))
    late.write_text(template.replace("REJECTION", 'if (x < 1) reject("late columns");'))
    command = [cli, str(reference), str(data), "--seed", "23", "--warmup", "0",
               "--samples", "100", "--chains", "2", "--num-threads", "2",
               "--init-radius", "0", "--max-depth", "1"]
    for stats in (False, True):
        flags = ["--sampler-stats"] if stats else []
        want = subprocess.run(command + flags, capture_output=True, check=True, timeout=60)
        command[1] = str(late)
        got = subprocess.run(command + flags, capture_output=True, timeout=60)
        assert got.returncode == 0, got.stderr.decode()
        command[1] = str(reference)
        a = list(csv.reader(io.StringIO(want.stdout.decode())))
        b = list(csv.reader(io.StringIO(got.stdout.decode())))
        assert a[0] == b[0] and len(a) == len(b) == 201
        offset = 7 if stats else 0
        failed = succeeded = 0
        for expected, actual in zip(a[1:], b[1:]):
            assert len(actual) == len(expected)
            assert actual[:offset] == expected[:offset]
            if float(expected[offset]) < 1:
                assert all(math.isnan(float(x)) for x in actual[offset:])
                failed += 1
            else:
                assert actual == expected
                succeeded += 1
        assert failed and succeeded and float(a[1][offset]) < 1
    print("PASS late interpreter columns and rejected prefixes, with/without stats")
