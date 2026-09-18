#!/usr/bin/env python3
"""The opt-in CLI provisions one worker team per chain and preserves CSV output."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'data.json'
    n = 16384
    path.write_text(json.dumps(dict(N=n, P=3, active_slice=0, kind=1,
                                   chunk=0, start=1, end=n,
                                   x=[.1]*n, y=[.3]*n)))
    command = [binary, str(repo/'tests/fixtures/reduce_sum_parallel_probe.stan'),
               str(path), '--chains', '2', '--threads-per-chain', '4',
               '--warmup', '5', '--samples', '5', '--max-depth', '3']
    def run(threads):
        return subprocess.run(command + ['--num-threads', str(threads)],
                              capture_output=True, text=True, check=True, timeout=60)
    serial, parallel = run(1), run(2)
    assert serial.stdout == parallel.stdout, 'chain scheduling changed draws'
    assert len(parallel.stdout.strip().splitlines()) == 11, 'missing header/draws'
    assert '1 retained reductions; up to 8 active sampling threads' in parallel.stderr
    invalid = subprocess.run(command + ['--threads-per-chain', '0'],
                             capture_output=True, text=True, timeout=60)
    assert invalid.returncode == 2 and 'must be positive' in invalid.stderr
print('PASS: within-chain CLI teams, sampling, CSV, and validation')
