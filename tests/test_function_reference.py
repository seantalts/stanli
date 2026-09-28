"""Check compiled standalone C-ABI results against independent CmdStan rows."""
import ctypes as c
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

def child(library):
    os.environ['STANLI_EXECUTION_REPORT'] = '1'
    lib = c.CDLL(library)
    ptr = c.c_void_p
    doubles, ints, dims = c.POINTER(c.c_double), c.POINTER(c.c_int), c.POINTER(c.c_int64)
    class Argument(c.Structure):
        _fields_ = [('name',c.c_char_p),('is_int',c.c_int),('reals',doubles),
                    ('ints',ints),('size',c.c_size_t),('dims',dims),('dim_size',c.c_size_t)]
    Writer = c.CFUNCTYPE(c.c_int,ptr,c.c_int,doubles,c.c_size_t,ints,c.c_size_t,dims,c.c_size_t)
    lib.stanli_function_new_from_mir.argtypes = [c.c_char_p,c.c_char_p,c.c_char_p,c.c_size_t]
    lib.stanli_function_new_from_mir.restype = ptr
    lib.stanli_function_free.argtypes = [ptr]
    lib.stanli_function_call_values.argtypes = [ptr,c.POINTER(Argument),c.c_size_t,
                                               Writer,ptr,c.c_char_p,c.c_size_t]
    reference = json.loads(FIXTURE.with_suffix('.ref.json').read_text())
    err = c.create_string_buffer(8192)
    handle = lib.stanli_function_new_from_mir(FIXTURE.with_suffix('.tmir.sexp').read_bytes(),
                                             b'combined',err,len(err))
    assert handle, err.value
    result = {}
    @Writer
    def writer(_, is_int, reals, size, integer, integer_size, shape, rank):
        result.update(is_int=is_int, values=list(reals[:size]), integer_size=integer_size,
                      dims=list(shape[:rank]))
        return 0
    rows = []
    try:
        for point in ('0','1','2','0'):
            x = c.c_double(float(reference['points'][point]['wa']['values'][0]))
            arg = Argument(b'x',0,c.pointer(x),None,1,None,0)
            assert lib.stanli_function_call_values(handle,c.byref(arg),1,writer,None,
                                                   err,len(err)) == 0, err.value
            rows.append(dict(point=point,**result))
    finally:
        lib.stanli_function_free(handle)
    print(json.dumps(rows))

def ordered(value):
    bits = struct.unpack('>Q',struct.pack('>d',value))[0]
    return ~bits & ((1<<64)-1) if bits>>63 else bits | (1<<63)

def main(library):
    reference = json.loads(FIXTURE.with_suffix('.ref.json').read_text())
    for suffix,key in (('.stan','source_sha256'),('.json','data_sha256')):
        assert hashlib.sha256(FIXTURE.with_suffix(suffix).read_bytes().replace(b'\r\n',b'\n')).hexdigest() == reference[key]
    run = subprocess.run([sys.executable,__file__,library,'--child'],capture_output=True,
                         text=True,check=True)
    reports = [json.loads(line) for line in run.stderr.splitlines() if line.startswith('{')]
    selections = [r for r in reports if r.get('kind') == 'execution_function_selection']
    traces = [r for r in reports if r.get('kind') == 'execution_trace']
    assert len(selections) == len(traces) == 4, run.stderr
    assert all(r['value_engine'] == 'register_program' for r in selections), run.stderr
    assert all(not r['interpreter_events'] for r in traces), run.stderr
    worst = 0
    for row in json.loads(run.stdout):
        assert row['is_int'] == row['integer_size'] == 0 and row['dims'] == [10], row
        expected = reference['points'][row['point']]['wa']['values'][1:]
        assert len(expected) == len(row['values']) == 10
        for want,got in zip(expected,row['values']):
            want = float(want)
            assert math.isfinite(want) and math.isfinite(got)
            distance = abs(ordered(want)-ordered(got))
            assert distance <= 10, (row['point'],want,got,distance)
            worst = max(worst,distance)
    print(f'Compiled standalone API vs CmdStan: 40 values, max {worst} ULP, no MIR entries')

if __name__ == '__main__':
    child(sys.argv[1]) if len(sys.argv)>2 else main(sys.argv[1])
