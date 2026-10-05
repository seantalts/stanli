import sys, subprocess, statistics, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hp_models as H
from hp_models import mp, mpf, R
import math
def run_wa(binary, model, pt):
    p = subprocess.run([f'{R}/{binary}/stanli_check', f'{R}/tests/brms/{model}.stan', f'{R}/tests/brms/{model}.json', '--point', str(pt), '--wa-values'],
                       capture_output=True, text=True, cwd=R)
    names = vals = None
    for l in p.stdout.splitlines():
        if l.startswith('WANAMES '): names = l[8:].split(',')
        if l.startswith('WAVALS'): vals = [float(x) for x in l[6:].split()]
    return names, vals
def ue(x, h):
    f = float(h)
    u = abs(math.nextafter(f, math.inf) - f)
    if u == 0: return 0.0 if x == 0 else float('inf')
    return float(abs(mpf(x) - h) / u)
verbose = '-v' in sys.argv
for model in ['s2_gev', 's2_me2_nomecor', 'sw_me']:
    print('###', model, 'write_array values')
    allE = {'cmdstan': [], 'off': [], 'on': []}; allB = {'cmdstan': [], 'off': [], 'on': []}
    for pt in (0, 1, 2):
        h = H.hp_wa(model, pt)
        pr = H.MODELS[model]['points'][str(pt)]['wa']
        names = pr['names'].split(',') if isinstance(pr['names'], str) else pr['names']
        ref = [float(x) for x in pr['values']]
        n0, off = run_wa('build-off', model, pt); n1, on = run_wa('build-on', model, pt)
        assert n0 == n1 == names or print(n0[:8], names[:8])
        miss = [n for n in names if n not in h]
        assert not miss, miss[:5]
        dbl = dict(zip(names, ref)); hB = H.hp_wa(model, pt, doubles=dbl)
        E = {'cmdstan': [], 'off': [], 'on': []}; EB = {'cmdstan': [], 'off': [], 'on': []}
        for i, n in enumerate(names):
            eB = (ue(ref[i], hB[n]), ue(off[i], hB[n]), ue(on[i], hB[n]))
            for k, v in zip(EB, eB): EB[k].append(v)
            if verbose and max(eB) > 3: print('   B', model, pt, n, 'cmd %.2f off %.2f on %.2f' % eB)
            e = (ue(ref[i], h[n]), ue(off[i], h[n]), ue(on[i], h[n]))
            for k, v in zip(E, e): E[k].append(v)
            if verbose and (max(e) > 3 or off[i] != on[i]) : print(model, pt, n, mp.nstr(h[n], 18), 'cmd %.2f off %.2f on %.2f' % e, 'DIFF' if off[i] != on[i] else '')
        for k in E: allE[k] += E[k]
        print('pt%d  refB (from double inputs): max cmd %.2f off %.2f on %.2f' % (pt, max(EB['cmdstan']), max(EB['off']), max(EB['on'])))
        for k in EB: allB[k] += EB[k]
        print('pt%d  n=%d  max cmd %.2f off %.2f on %.2f | median cmd %.2f off %.2f on %.2f | changed on-vs-off: %d' % (
            pt, len(names), max(E['cmdstan']), max(E['off']), max(E['on']), statistics.median(E['cmdstan']), statistics.median(E['off']), statistics.median(E['on']),
            sum(1 for a, b in zip(off, on) if a != b)))
    print('all max (ref A: exact from u) cmd %.2f off %.2f on %.2f' % tuple(max(allE[k]) for k in ('cmdstan', 'off', 'on')))
    print('all max (ref B: from double inputs) cmd %.2f off %.2f on %.2f' % tuple(max(allB[k]) for k in ('cmdstan', 'off', 'on')))
