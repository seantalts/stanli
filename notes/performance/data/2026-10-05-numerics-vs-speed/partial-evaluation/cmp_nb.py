import sys, statistics; sys.path.insert(0,'.')
from hp_nb import *
def hpvals(fn, n, pt, dps, gradfn=None):
    mp.dps = dps
    u = [mpf(eval_point(i, pt)) for i in range(n)]
    v = fn(u)
    if gradfn: g = gradfn(u)
    else:
        g = []
        for i in range(n):
            o = [0]*n; o[i] = 1
            g.append(mp.diff(lambda *a: fn(list(a)), u, tuple(o)))
    mp.dps = 70
    return [v] + g
def report(model, h_by_pt, pts=(0,1,2)):
    E = {'cmdstan': [], 'off': [], 'on': []}
    for pt in pts:
        h = h_by_pt[pt]
        ref = [float(s) for s in H.MODELS[model]['points'][str(pt)]['values']]
        off = run('build-off', model, pt); on = run('build-on', model, pt)
        assert len(ref)==len(off)==len(on)==len(h), (len(ref),len(off),len(on),len(h))
        e = {'cmdstan':[uerr(a,b) for a,b in zip(ref,h)], 'off':[uerr(a,b) for a,b in zip(off,h)], 'on':[uerr(a,b) for a,b in zip(on,h)]}
        # norm-wise gradient error in 2^-52 of max|h_grad|
        gm = max(abs(x) for x in h[1:])
        nw = {k: max(float(abs(mpf(x)-y)/gm) for x,y in zip(v[1:],h[1:]))/2**-52 for k,v in (('cmdstan',ref),('off',off),('on',on))}
        print(f'{model} pt{pt} lp ulp: cmd {e["cmdstan"][0]:.2f} off {e["off"][0]:.2f} on {e["on"][0]:.2f} | grad component max ulp: cmd {max(e["cmdstan"][1:]):.2f} off {max(e["off"][1:]):.2f} on {max(e["on"][1:]):.2f} | grad normwise(2^-52): cmd {nw["cmdstan"]:.2f} off {nw["off"]:.2f} on {nw["on"]:.2f}')
        for k in E: E[k] += e[k]
    print(model, 'all max  ', *['%s %.2f'%(k,max(v)) for k,v in E.items()])
    print(model, 'all median', *['%s %.2f'%(k,statistics.median(v)) for k,v in E.items()])
if __name__=='__main__':
    m = sys.argv[1]
    if m=='ch15_m15_8':
        fn,n = m15_8()
        h = {pt: hpvals(fn,n,pt,70) for pt in (0,1,2)}
        h2 = {pt: hpvals(fn,n,pt,110) for pt in (0,1,2)}
        for pt in h: print('self-check 70 vs 110 max rel', max(float(abs(a-b)/max(abs(b),mpf(10)**-40)) for a,b in zip(h[pt],h2[pt])))
        report(m,h)
    if m=='ch14_m14_10':
        import time
        out = {}
        for dps in (70, 110):
            mp.dps = dps; t=time.time()
            fn, n = m14_10_ref(None, dps)
            print('precompute', dps, time.time()-t, flush=True)
            out[dps] = {pt: hpvals(fn,n,pt,dps) for pt in (0,1,2)}
        mp.dps=70
        for pt in (0,1,2): print('self-check 70 vs 110 max rel', max(float(abs(a-b)/max(abs(b),mpf(10)**-40)) for a,b in zip(out[70][pt],out[110][pt])))
        report(m, out[70])
    if m=='ch14_m14_11':
        import time
        out = {}
        for dps in (70, 110):
            mp.dps = dps; t=time.time()
            lp, grad, n = m14_11_ref(dps)
            out[dps] = {}
            for pt in (0,1,2):
                u = [mpf(eval_point(i, pt)) for i in range(n)]
                out[dps][pt] = [lp(u)] + grad(u)
                print('dps', dps, 'pt', pt, time.time()-t, flush=True)
        mp.dps = 70
        for pt in (0,1,2): print('self-check 70 vs 110 max rel', max(float(abs(a-b)/max(abs(b),mpf(10)**-40)) for a,b in zip(out[70][pt],out[110][pt])))
        mp.dps = 50
        u = [mpf(eval_point(i, 0)) for i in range(n)]
        lp, grad, n = m14_11_ref(50)
        gd = [mp.diff(lambda *a: lp(list(a)), u, tuple(1 if j==i else 0 for j in range(n))) for i in range(n)]
        mp.dps = 70
        print('analytic vs mp.diff grad max rel', max(float(abs(a-b)/max(abs(b),1)) for a,b in zip(out[70][0][1:],gd)))
        report(m, out[70])
