#!/usr/bin/env python3
"""Phase measurement must leave the seeded chain and generated CSV unchanged."""
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from check_corpus_sampling import read_timings

model = REPO / "tests/educational/models/aalto_bern"
command = [sys.argv[1], str(model / "model.stan"), str(model / "data.json"),
           "--seed", "17", "--warmup", "100", "--samples", "100", "--sampler-stats"]
plain = subprocess.run(command, capture_output=True, check=True, timeout=60)
with tempfile.TemporaryDirectory() as temporary:
    path = pathlib.Path(temporary) / "timings.json"
    timed = subprocess.run(command + ["--timings-json", str(path)],
                           capture_output=True, check=True, timeout=60)
    assert plain.stdout == timed.stdout, "--timings-json changed seeded sampler or generated outputs"
    read_timings(path)
print("PASS: phase timings preserve CSV bytes")
