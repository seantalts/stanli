#!/usr/bin/env python3
"""Subprocess-isolated structural covariance screen for issue #656.

Generated programs are synthetic diagnostic inputs. A passing runtime screen
is not CmdStan parity; independently check suspicious and representative cases.
"""
import argparse
import ctypes as c
import hashlib
import json
import math
import os
import pathlib
import platform
import resource
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve()
REPO = HERE.parents[2]
FUNCTION = r'''
functions {
  matrix assemble(vector x, real amplitude, real rate, real jitter) {
    int N = rows(x);
    matrix[N,N] K;
    for (i in 1:N) {
      K[i,i] = amplitude + jitter;
      if (i < N) for (j in (i+1):N) {
        K[i,j] = amplitude * exp(-rate * square(x[i]-x[j]));
        K[j,i] = K[i,j];
      }
    }
    return K;
  }
  matrix factor(vector x, real amplitude, real rate, real jitter) {
    return cholesky_decompose(assemble(x, amplitude, rate, jitter));
  }
  array[] matrix make_array(vector x, real amplitude, real rate, real jitter) {
    int N = rows(x);
    array[2] matrix[N,N] A;
    A[1] = assemble(x, amplitude, rate, jitter);
    A[2] = A[1];
    return A;
  }
}
'''
DATA = '''
data {
  int<lower=0> N;
  vector[N] x;
  vector[N] y;
  real<lower=0> jitter;
  int<lower=0,upper=1> selector;
  array[N] int<lower=1,upper=max(1,N)> permutation;
}
'''
PARAMS = '''
parameters {
  real<lower=0> amplitude;
  real<lower=0> rate;
  vector[N] z;
}
'''
PRIOR = 'amplitude ~ exponential(1); rate ~ exponential(1); z ~ std_normal();'
BUILDERS = {
    'udf': 'K = assemble(x, amplitude, rate, jitter);',
    'inline': '''
      for (i in 1:N) {
        K[i,i] = amplitude + jitter;
        if (i < N) for (j in (i+1):N) {
          K[i,j] = amplitude * exp(-rate * square(x[i]-x[j]));
          K[j,i] = K[i,j];
        }
      }''',
    'block_slice': '''
      matrix[N,2] B;
      B[,1] = rep_vector(amplitude, N);
      B[,2] = rate * x;
      K = rep_matrix(0.0, N, N);
      if (N > 0) K[1:N,1:N] = add_diag(B * B', jitter);''',
    'block_rows': '''
      K = rep_matrix(0.0, N, N);
      for (i in 1:N) {
        K[i,] = to_row_vector(amplitude * exp(-rate * square(x - x[i])));
      }
      K = add_diag(K, jitter);''',
    'array': '''
      array[2] matrix[N,N] A;
      A[1] = assemble(x, amplitude, rate, jitter);
      A[2] = A[1];
      K = A[2];''',
    'udf_array': '''
      array[2] matrix[N,N] A = make_array(x, amplitude, rate, jitter);
      K = A[2];''',
    'data_branch': '''
      if (selector == 1) K = assemble(x, amplitude, rate, jitter);
      else K = add_diag(((amplitude * x) * (amplitude * x)'), jitter);''',
    'parameter_branch': '''
      if (amplitude > 1.02) K = assemble(x, amplitude, rate, jitter);
      else K = add_diag(((amplitude * x) * (amplitude * x)'), jitter);''',
    'builtin': '''K = add_diag(gp_exp_quad_cov(to_array_1d(x), amplitude, rate), jitter);''',
    'builtin_equivalent': '''K = add_diag(gp_exp_quad_cov(to_array_1d(x), sqrt(amplitude), inv_sqrt(2*rate)), jitter);''',
    'permuted_writes': '''
      for (i in 1:N) for (j in i:N) {
        int ii = permutation[i];
        int jj = permutation[j];
        K[ii,jj] = amplitude * exp(-rate * square(x[ii]-x[jj]));
        K[jj,ii] = K[ii,jj];
      }
      K = add_diag(K, jitter);''',
    'nested_local': '''
      K = rep_matrix(0.0, N, N);
      for (i in 1:N) {
        vector[N] d;
        for (j in 1:N) d[j] = exp(-rate * square(x[i]-x[j]));
        K[i,] = amplitude * to_row_vector(d);
      }
      K = add_diag(K, jitter);''',
}
VARIANTS = list(BUILDERS) + ['nested_factor', 'transformed_data', 'gq_udf', 'gq_array', 'gq_parameter_branch']


def model_source(variant):
    if variant == 'transformed_data':
        return FUNCTION + DATA + '''transformed data {
          matrix[N,N] K = assemble(x, 1.0, 1.0, jitter);
          matrix[N,N] L = cholesky_decompose(K);
        }''' + PARAMS + '''transformed parameters { vector[N] mu = L*z; }
        model { ''' + PRIOR + ''' y ~ normal(mu,1); }
        generated quantities { real checksum = sum(L); }
        '''
    if variant == 'nested_factor':
        return FUNCTION + DATA + PARAMS + '''transformed parameters {
          matrix[N,N] L = factor(x, amplitude, rate, jitter);
          vector[N] mu = L*z;
        } model { ''' + PRIOR + ''' y ~ normal(mu,1); }
        generated quantities { real checksum = sum(L); }
        '''
    is_gq = variant.startswith('gq_')
    builder = BUILDERS[variant[3:] if is_gq else variant]
    if is_gq:
        return FUNCTION + DATA + PARAMS + 'model { ' + PRIOR + ' y ~ normal(z,1); }' + '''
        generated quantities { matrix[N,N] K; ''' + builder + '''
          matrix[N,N] L = cholesky_decompose(K);
          vector[N] mu = L*z;
          real checksum = sum(L);
        }'''
    return FUNCTION + DATA + PARAMS + 'transformed parameters { matrix[N,N] K;' + builder + '''
      matrix[N,N] L = cholesky_decompose(K);
      vector[N] mu = L*z;
    } model { ''' + PRIOR + ''' y ~ normal(mu,1); }
    generated quantities { real checksum = sum(L); }
    '''


def sha_bytes(value):
    return hashlib.sha256(value).hexdigest()


def values_summary(values):
    values = list(values)
    return dict(count=len(values), finite=sum(math.isfinite(v) for v in values),
                sha256=sha_bytes(struct.pack(f'{len(values)}d', *values)),
                first=values[:4], last=values[-4:])


def point(n, variant):
    return [(0.1 + .05*(i%7) - .15*(i%3)) if variant == 0 else
            .02*((i%5)-2) if variant == 1 else 0.0 for i in range(n)]


def worker(args):
    resource.setrlimit(resource.RLIMIT_CORE, (0,0))
    resource.setrlimit(resource.RLIMIT_CPU, (20,20))
    lib = c.CDLL(str(args.runtime.resolve()))
    lib.stanli_build_id.restype = c.c_char_p
    lib.stanli_stan_to_mir_with_opts.argtypes = [c.c_char_p,c.c_void_p,c.c_size_t,c.c_int,c.c_char_p,c.c_size_t]
    lib.stanli_stan_to_mir_with_opts.restype = c.c_void_p
    lib.stanli_string_free.argtypes = [c.c_void_p]
    lib.stanli_model_new_with_opts.argtypes = [c.c_char_p,c.c_char_p,c.c_void_p,c.c_char_p,c.c_size_t]
    lib.stanli_model_new_with_opts.restype = c.c_void_p
    lib.stanli_model_free.argtypes = [c.c_void_p]
    lib.stanli_n_unconstrained.argtypes = [c.c_void_p]
    lib.stanli_n_unconstrained.restype = c.c_int64
    lib.stanli_grad_err.argtypes = [c.c_void_p,c.POINTER(c.c_double),c.POINTER(c.c_double),c.POINTER(c.c_double),c.c_char_p,c.c_size_t]
    lib.stanli_wa_n_columns.argtypes = [c.c_void_p]
    lib.stanli_wa_n_columns.restype = c.c_int64
    lib.stanli_wa_row.argtypes = [c.c_void_p,c.POINTER(c.c_double),c.POINTER(c.c_double)]
    lib.stanli_wa_column_name.argtypes = [c.c_void_p,c.c_int64]
    lib.stanli_wa_column_name.restype = c.c_char_p
    lib.stanli_warnings.argtypes = [c.c_void_p]
    lib.stanli_warnings.restype = c.c_char_p
    err = c.create_string_buffer(8192)
    report = dict(runtime=lib.stanli_build_id().decode(), variant=args.variant, stage='compile',
                  library_peak_rss_bytes=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    def checkpoint(stage):
        report['stage']=stage
        if args.checkpoint:args.checkpoint.write_text(json.dumps(report))
    checkpoint('compile')
    source = model_source(args.variant).encode()
    report['model_sha256'] = sha_bytes(source)
    t=time.perf_counter()
    text = lib.stanli_stan_to_mir_with_opts(source,None,0,0,err,len(err))
    report['compile_seconds']=time.perf_counter()-t
    report['compiler_peak_rss_bytes']=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if not text:
        report['error']=err.value.decode(); print(json.dumps(report),flush=True); return
    mir=c.string_at(text);lib.stanli_string_free(text)
    checkpoint('prepare')
    t=time.perf_counter()
    model=lib.stanli_model_new_with_opts(mir,args.data.read_bytes(),None,err,len(err))
    report['prepare_seconds']=time.perf_counter()-t
    report['preparation_peak_rss_bytes']=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if not model:
        report['error']=err.value.decode(); print(json.dumps(report),flush=True); return
    report['warnings']=lib.stanli_warnings(model).decode()
    n=lib.stanli_n_unconstrained(model);report['dimension']=n
    cols=lib.stanli_wa_n_columns(model);report['output_columns']=cols
    if args.full:report['wa_names']=[lib.stanli_wa_column_name(model,i).decode() for i in range(cols)]
    report['points']=[]
    for p in range(3):
        q=(c.c_double*n)(*point(n,p));g=(c.c_double*n)();lp=c.c_double();err.value=b''
        checkpoint('gradient:'+str(p))
        status=lib.stanli_grad_err(model,q,c.byref(lp),g,err,len(err))
        row=dict(point=p,grad_status=status,grad_error=err.value.decode())
        if not status:row['lp']=lp.value;row['gradient']=list(g)
        if cols:
            checkpoint('write_array:'+str(p))
            out=(c.c_double*cols)();row['wa_status']=lib.stanli_wa_row(model,q,out)
            if row['wa_status']==0:
                row['wa_summary']=values_summary(out)
                if args.full:row['wa_values']=list(out)
        report['points'].append(row)
    checkpoint('done')
    report['peak_rss_bytes']=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    lib.stanli_model_free(model)
    print(json.dumps(report),flush=True)


def run_bounded(cmd, timeout):
    process=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    start=time.monotonic()
    while True:
        try:
            out,err=process.communicate(timeout=.1)
            return subprocess.CompletedProcess(cmd,process.returncode,out,err)
        except subprocess.TimeoutExpired:
            if time.monotonic()-start>timeout:
                process.kill();out,err=process.communicate()
                raise subprocess.TimeoutExpired(cmd,timeout,out,err)
            rss=subprocess.run(['/bin/ps','-o','rss=','-p',str(process.pid)],capture_output=True,text=True)
            if rss.stdout.strip() and int(rss.stdout.strip())*1024>512*1024**2:
                process.kill();out,err=process.communicate()
                return subprocess.CompletedProcess(cmd,process.returncode,out,err+'\nRSS_GUARD: exceeded 512 MiB')


def screen(args):
    args.work.mkdir(parents=True,exist_ok=True)
    result=dict(source_sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip(),
                harness_sha256=sha_bytes(HERE.read_bytes()), platform=platform.platform(),
                runtime_sha256=sha_bytes(args.runtime.read_bytes()), timeout_seconds=args.timeout,
                cpu_limit_seconds=20,rss_memory_limit_bytes=512*1024**2,cases=[])
    variants=args.variants or VARIANTS
    for variant in variants:
        (args.work/(variant+'.stan')).write_text(model_source(variant))
        for n in args.sizes:
            for jitter in args.jitters:
                for selector in ([0,1] if variant=='data_branch' else [1]):
                    name=f'{variant}-n{n}-j{jitter}-s{selector}'
                    path=args.work/(name+'.json')
                    data=dict(N=n,x=[i/max(1,n-1) for i in range(n)],y=[math.sin(i) for i in range(n)],
                              jitter=jitter,selector=selector,permutation=list(range(n,0,-1)))
                    path.write_text(json.dumps(data))
                    cmd=[sys.executable,str(HERE),'--worker','--runtime',str(args.runtime),'--variant',variant,'--data',str(path),'--checkpoint',str(args.work/(name+'-checkpoint.json'))]
                    if args.full:cmd+=['--full']
                    row=dict(name=name,variant=variant,N=n,jitter=jitter,selector=selector,data=data)
                    try:
                        r=run_bounded(cmd,args.timeout)
                        row.update(returncode=r.returncode,stderr=r.stderr)
                        if r.stdout:
                            try:row['result']=json.loads(r.stdout)
                            except json.JSONDecodeError:row['stdout']=r.stdout
                    except subprocess.TimeoutExpired as e:
                        row.update(timeout=True,stdout=str(e.stdout),stderr=str(e.stderr))
                    (args.work/(name+'-result.json')).write_text(json.dumps(row,indent=2)+'\n')
                    result['cases'].append(row)
                    errors=[]
                    r=row.get('result',{})
                    if r.get('stage')!='done':errors.append(r.get('stage','subprocess'))
                    for p in r.get('points',[]):
                        if p['grad_status']:errors.append('grad:'+str(p['grad_status']))
                        if p.get('wa_status',0):errors.append('write_array')
                        if p.get('wa_summary',{}).get('finite')!=p.get('wa_summary',{}).get('count'):errors.append('nonfinite_output')
                    print(name,','.join(errors) or 'ok',r.get('error',''),flush=True)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+'\n')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--runtime',type=pathlib.Path,required=True)
    ap.add_argument('--work',type=pathlib.Path,default=REPO/'.cache/issue656/round2')
    ap.add_argument('--output',type=pathlib.Path,default=REPO/'.cache/issue656/round2.json')
    ap.add_argument('--timeout',type=int,default=30)
    ap.add_argument('--variants',nargs='+',choices=VARIANTS)
    ap.add_argument('--sizes',nargs='+',type=int,default=[0,1,2,35,36,64])
    ap.add_argument('--jitters',nargs='+',type=float,default=[.1,1e-8,1e-14])
    ap.add_argument('--worker',action='store_true')
    ap.add_argument('--variant',choices=VARIANTS)
    ap.add_argument('--data',type=pathlib.Path)
    ap.add_argument('--full',action='store_true')
    ap.add_argument('--checkpoint',type=pathlib.Path)
    args=ap.parse_args()
    worker(args) if args.worker else screen(args)


if __name__=='__main__':main()
