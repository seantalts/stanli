import csv, io, math, subprocess, sys, time, pathlib
run, work = sys.argv[1], pathlib.Path(sys.argv[2])
def draws(model, fast):
    cmd=[run,str(work/model/"model.stan"),str(work/model/"data.json"),"--seed","7","--chains","4","--warmup","1000","--samples","1000"]+(["--fast-math"] if fast else [])
    t=time.time(); p=subprocess.run(cmd,capture_output=True,text=True); dt=time.time()-t
    lines=[l for l in p.stdout.splitlines() if l and not l.startswith("#")]
    rows=list(csv.reader(lines)); head=rows[0]; vals=[]
    for r in rows[1:]:
        try: vals.append([float(x) for x in r])
        except ValueError: pass
    return head, vals, dt, p.returncode
def stats(col):
    n=len(col); m=sum(col)/n; v=sum((x-m)**2 for x in col)/(n-1); return m, math.sqrt(v)
for model in sys.argv[3:]:
    h,d,td,rc1=draws(model,False); h2,f,tf,rc2=draws(model,True)
    if rc1 or rc2 or h!=h2 or not d or not f: print(model,"FAILED",rc1,rc2,len(d),len(f)); continue
    worst=0; name=""
    for j,c in enumerate(h):
        if c.endswith("__") and c!="lp__": continue
        m1,s1=stats([r[j] for r in d]); m2,s2=stats([r[j] for r in f])
        s=max(s1,s2,1e-300); z=abs(m1-m2)/s
        if z>worst: worst,name=z,c
    print(f"{model:28s} draws {len(d)}/{len(f)}  columns {len(h)}  worst |mean diff|/sd {worst:.3f} ({name})  wall default {td:.2f}s fast {tf:.2f}s")
