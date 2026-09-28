#!/usr/bin/env python3
"""Measure shared-corpus native model phases through the public C API.

Usage: bench_model_phases.py /absolute/libstanli.dylib manifest.json model
Run baseline/candidate in alternating fresh processes. Returns one JSON sample;
source compilation is warmed, ctypes overhead is included, peak RSS is whole
process memory, and short inference is illustrative (100 warmup + 100 draws).
"""
import ctypes as c, json, pathlib, resource, sys, time
libpath, manifest_path, name = sys.argv[1:]
lib=c.CDLL(libpath)
ptr=c.c_void_p; doubles=c.POINTER(c.c_double)
for fn in ('stanli_model_new','stanli_model_new_from_stan'):
    f=getattr(lib,fn);f.argtypes=[c.c_char_p,c.c_char_p,c.c_char_p,c.c_size_t];f.restype=ptr
lib.stanli_model_free.argtypes=[ptr]
lib.stanli_n_unconstrained.argtypes=[ptr];lib.stanli_n_unconstrained.restype=c.c_int64
lib.stanli_wa_n_columns.argtypes=[ptr];lib.stanli_wa_n_columns.restype=c.c_int64
lib.stanli_wa_row.argtypes=[ptr,doubles,doubles]
lib.stanli_grad.argtypes=[ptr,doubles,doubles,doubles]
lib.stanli_sample.argtypes=[ptr,c.c_uint32,c.c_int,c.c_int,c.c_double,doubles,c.c_char_p,c.c_size_t]
lib.stanli_stan_to_mir.argtypes=[c.c_char_p,c.c_char_p,c.c_size_t];lib.stanli_stan_to_mir.restype=ptr
lib.stanli_string_free.argtypes=[ptr]
entry = next(row for row in json.loads(pathlib.Path(manifest_path).read_text())
             if row['model'] == name)
source=pathlib.Path(entry['source']).read_bytes()
mir=pathlib.Path(entry['mir']).read_bytes()
data=pathlib.Path(entry['data']).read_bytes()
err=c.create_string_buffer(8192)
def measure(f,n):
    start=time.perf_counter_ns()
    for _ in range(n): f()
    return (time.perf_counter_ns()-start)/n/1000

def warm_measure(f):
    start=time.perf_counter_ns()
    while time.perf_counter_ns()-start < 200_000_000: f()
    start=time.perf_counter_ns(); n=0
    while time.perf_counter_ns()-start < 250_000_000: f(); n+=1
    return (time.perf_counter_ns()-start)/n/1000

def compile_source():
    p=lib.stanli_stan_to_mir(source,err,len(err));assert p,err.value
    lib.stanli_string_free(p)
compile_source()
source_us=warm_measure(compile_source)
def prep():
    p=lib.stanli_model_new(mir,data,err,len(err));assert p,err.value
    return p
p=prep();lib.stanli_model_free(p)
prep_us=warm_measure(lambda:lib.stanli_model_free(prep()))
p=prep()
n=lib.stanli_n_unconstrained(p);w=lib.stanli_wa_n_columns(p)
q=(c.c_double*n)(*[0.1]*n);grad=(c.c_double*n)();lp=c.c_double();out=(c.c_double*w)()
def gradient(): assert lib.stanli_grad(p,q,c.byref(lp),grad)==0
first_grad_us=measure(gradient,1);grad_us=warm_measure(gradient)
check_values=[lp.value,*grad]
def row(): assert lib.stanli_wa_row(p,q,out)==0
first_row_us=measure(row,1);row_us=warm_measure(row)
draws=(c.c_double*(100*n))()
start=time.perf_counter_ns()
assert lib.stanli_sample(p,1234,100,100,0.8,draws,err,len(err))==0,err.value
# Include output generation for every posterior draw; sampling excludes it.
for i in range(100):
    q=(c.c_double*n)(*draws[i*n:(i+1)*n]);row()
inference_us=(time.perf_counter_ns()-start)/1000
lib.stanli_model_free(p)
print(json.dumps(dict(model=name,check_values=check_values,source_us=source_us,prep_us=prep_us,first_grad_us=first_grad_us,grad_us=grad_us,first_row_us=first_row_us,row_us=row_us,inference_us=inference_us,peak_rss=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,library_bytes=pathlib.Path(libpath).stat().st_size)))
