#!/usr/bin/env python3
"""Benchmark the public native standalone-function ABI in one fresh process.

Usage: bench_function_paths.py /absolute/libstanli.dylib affine|branch_exit|sized
The result writer deliberately does no copying. ctypes/callback overhead remains
included. sized additionally measures twelve integer specializations in rotation.
Run alternating baseline/candidate processes; report medians and dispersion.
"""
import ctypes as c
import json
from pathlib import Path
import resource
import sys
import time

libpath, name = sys.argv[1:]
lib = c.CDLL(libpath)
ptr = c.c_void_p
doubles = c.POINTER(c.c_double)
ints = c.POINTER(c.c_int)
dims = c.POINTER(c.c_int64)
class Argument(c.Structure):
    _fields_ = [('name', c.c_char_p), ('is_int', c.c_int), ('reals', doubles),
                ('ints', ints), ('size', c.c_size_t), ('dims', dims),
                ('dim_size', c.c_size_t)]
Writer = c.CFUNCTYPE(c.c_int, ptr, c.c_int, doubles, c.c_size_t, ints, c.c_size_t,
                    dims, c.c_size_t)
lib.stanli_function_new_from_mir.argtypes = [c.c_char_p,c.c_char_p,c.c_char_p,c.c_size_t]
lib.stanli_function_new_from_mir.restype = ptr
lib.stanli_function_free.argtypes = [ptr]
lib.stanli_function_call_values.argtypes = [ptr,c.POINTER(Argument),c.c_size_t,
                                          Writer,ptr,c.c_char_p,c.c_size_t]
mir = Path('tests/fixtures/standalone_compiled.tmir.sexp').read_bytes()
err = c.create_string_buffer(8192)
keep = []
def argument(name, values, dimensions=(), integer=False):
    data = ((c.c_int if integer else c.c_double)*len(values))(*values)
    shape = (c.c_int64*len(dimensions))(*dimensions)
    keep.extend((data, shape))
    return Argument(name.encode(),int(integer),None if integer else data,
                    data if integer else None,len(values),shape,len(dimensions))
if name == 'affine':
    args = (Argument*3)(argument('x',range(32),(32,)),argument('a',[.5]),argument('b',[.1]))
elif name == 'branch_exit':
    args = (Argument*1)(argument('x',[.2]))
elif name == 'sized':
    args = (Argument*2)(argument('x',[.2]),argument('n',[8],integer=True))
else:
    raise ValueError(name)
@Writer
def writer(*_):
    return 0

def measure(fn):
    start = time.perf_counter_ns()
    while time.perf_counter_ns()-start < 200_000_000:
        fn()
    start = time.perf_counter_ns()
    count = 0
    while time.perf_counter_ns()-start < 250_000_000:
        fn()
        count += 1
    return (time.perf_counter_ns()-start)/count/1000

def create():
    f = lib.stanli_function_new_from_mir(mir,name.encode(),err,len(err))
    assert f, err.value
    return f
handle_us = measure(lambda:lib.stanli_function_free(create()))
f = create()
def call():
    assert lib.stanli_function_call_values(f,args,len(args),writer,None,err,len(err)) == 0, err.value
start = time.perf_counter_ns()
call()
first_us = (time.perf_counter_ns()-start)/1000
warm_us = measure(call)
churn_us = None
if name == 'sized':
    cursor = 0
    def churn():
        global cursor
        args[1].ints[0] = cursor % 12
        cursor += 1
        call()
    churn_us = measure(churn)
lib.stanli_function_free(f)
print(json.dumps(dict(function=name,handle_us=handle_us,first_call_us=first_us,
                     warm_call_us=warm_us,churn_call_us=churn_us,
                     peak_rss=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                     library_bytes=Path(libpath).stat().st_size)))
