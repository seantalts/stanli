#!/usr/bin/env python3
"""Reproduce the developer-only scalar structured-callback solve comparison.

Build bench_structured_callback first. Every timing requires bitwise local and
solver agreement plus equal solver callback counts. No runtime path is changed.
"""
import argparse, pathlib, subprocess, json, os, re, statistics
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build",default="build-release")
parser.add_argument("--output",required=True)
parser.add_argument("--repeats",type=int,default=6)
args=parser.parse_args()
if args.repeats<1:parser.error("--repeats must be positive")
r=pathlib.Path(__file__).resolve().parents[1];w=pathlib.Path(args.output).resolve();w.mkdir(parents=True,exist_ok=True);rows=[]
build=pathlib.Path(args.build).resolve()
for rep in range(args.repeats):
 for n in [8,128,2048]:
  d=w/f'N{n}.json';d.write_text(json.dumps({'N':n}))
  for case in ['loop','branch']:
   ode='ode_constant_loop' if case=='loop' else 'ode_structured_callback_branch'
   modes=['generated','structured','segments'] if case=='loop' else ['structured','segments']
   if rep%2:modes.reverse()
   for mode in modes:
    env=os.environ.copy()
    env.pop('STANLI_CALLBACK_PROBE_SEGMENTS',None)
    env.pop('STANLI_STRUCTURED_FRAMES',None)
    if mode=='segments':env['STANLI_CALLBACK_PROBE_SEGMENTS']='1'
    cmd=[str(build/'bench_structured_callback'),str(r/f'tests/fixtures/{ode}.tmir.sexp'),str(d),'--provider','generated' if mode=='generated' else 'structured','--iterations',str(20 if n<2048 else 3),'--batches','2','--warmup-ms','50','--require-exact']
    if mode!='generated':cmd+=['--structured-mir',str(r/f'tests/fixtures/structured_callback_{case}.tmir.sexp')]
    p=subprocess.run(cmd,env=env,text=True,capture_output=True,check=True,timeout=60)
    (w/f'paired-{case}-{n}-{mode}-{rep}.log').write_text(p.stdout+p.stderr)
    m=re.search(r'median oracle_ns=([\d.]+) candidate_ns=([\d.]+)',p.stdout);assert m,p.stdout
    prep=re.search(r'structured_preparation_ns=(\d+)',p.stdout)
    row=dict(repeat=rep,case=case,N=n,mode=mode,oracle_ns=float(m[1]),candidate_ns=float(m[2]),prep_ns=int(prep[1])if prep else None,exact=True)
    rows.append(row);(w/'solve-raw.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(case,n,mode,rep,round(row['candidate_ns']/1000,2),flush=True)
summary={}
for case in ['loop','branch']:
 for n in [8,128,2048]:
  for mode in ['generated','structured','segments']:
   a=[x for x in rows if x['case']==case and x['N']==n and x['mode']==mode]
   if not a:continue
   met={}
   for k in ['oracle_ns','candidate_ns','prep_ns']:
    v=[x[k]for x in a if x[k]is not None]
    if v:
     mid=statistics.median(v);met[k]={'median':mid,'mad':statistics.median(abs(t-mid)for t in v)}
   summary[f'{case}-{n}-{mode}']=met
(w/'solve-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
