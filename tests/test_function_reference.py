"""Check compiled standalone C-ABI results against independent CmdStan rows."""
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/fixtures/standalone_function_reference'

def ordered(value):
    bits = struct.unpack('>Q',struct.pack('>d',value))[0]
    return ~bits & ((1<<64)-1) if bits>>63 else bits | (1<<63)

def main(host):
    reference = json.loads(FIXTURE.with_suffix('.ref.json').read_text())
    for suffix,key in (('.stan','source_sha256'),('.json','data_sha256')):
        assert hashlib.sha256(FIXTURE.with_suffix(suffix).read_bytes().replace(b'\r\n',b'\n')).hexdigest() == reference[key]
    points = ('0','1','2','0')
    values = [str(reference['points'][point]['wa']['values'][0]) for point in points]
    run = subprocess.run([host,str(FIXTURE.with_suffix('.tmir.sexp')),*values],
                         capture_output=True,text=True,
                         env={**os.environ,'STANLI_EXECUTION_REPORT':'1'})
    assert run.returncode == 0, (run.returncode,run.stdout,run.stderr)
    reports = [json.loads(line) for line in run.stderr.splitlines() if line.startswith('{')]
    selections = [r for r in reports if r.get('kind') == 'execution_function_selection']
    traces = [r for r in reports if r.get('kind') == 'execution_trace']
    assert len(selections) == len(traces) == 4, run.stderr
    assert all(r['value_engine'] == 'register_program' for r in selections), run.stderr
    assert all(not r['interpreter_events'] for r in traces), run.stderr
    worst = 0
    rows = [json.loads(line) for line in run.stdout.splitlines()]
    assert len(rows) == len(points), run.stdout
    for point,row in zip(points,rows):
        assert row['is_int'] == row['integer_size'] == 0 and row['dims'] == [10], row
        expected = reference['points'][point]['wa']['values'][1:]
        assert len(expected) == len(row['values']) == 10
        for want,got in zip(expected,row['values']):
            want = float(want)
            assert math.isfinite(want) and math.isfinite(got)
            distance = abs(ordered(want)-ordered(got))
            assert distance <= 10, (point,want,got,distance)
            worst = max(worst,distance)
    print(f'Compiled standalone API vs CmdStan: 40 values, max {worst} ULP, no MIR entries')

if __name__ == '__main__':
    main(sys.argv[1])
