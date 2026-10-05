import sys, subprocess, statistics, pathlib, random, os, math, tempfile, json
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hp_models as H
from hp_models import mp, mpf, R

def hp_at(model, u):
    fn, n = H.build(model)
    uu = [mpf(x) for x in u]
    val = fn(uu); g = []
    for i in range(n):
        o = [0] * n; o[i] = 1
        g.append(H.diff(lambda *a: fn(list(a)), uu, tuple(o)))
    return [val] + g

def run(binary, model, u):
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        f.write('\n'.join(repr(x) for x in u)); name = f.name
    env = dict(os.environ, STANLI_CHECK_PARAMS_FILE=name)
    out = subprocess.run([f'{R}/{binary}/stanli_check', f'{R}/tests/brms/{model}.stan', f'{R}/tests/brms/{model}.json', '--point', '0'],
                         capture_output=True, text=True, cwd=R, env=env).stdout
    os.unlink(name)
    f = [l for l in out.splitlines() if l.startswith('OK')][-1].split()
    return [float(x) for x in f[1:]]

def ue(x, h):
    f = float(h); u = abs(math.nextafter(f, math.inf) - f)
    return float(abs(mpf(x) - h) / u) if u else 0.0

def main(model, npts, sd, seed):
    rng = random.Random(seed)
    fn, n = H.build(model)
    rows = []
    for k in range(npts):
        u = [H.eval_point(i, 0) + rng.gauss(0, sd) for i in range(n)]
        h = hp_at(model, u)
        off = run('build-off', model, u); on = run('build-on', model, u)
        eo = [ue(a, b) for a, b in zip(off, h)]; en = [ue(a, b) for a, b in zip(on, h)]
        # scaled error: abs err / max(|h|, 1) is dominated by big comps; also norm-wise
        gs = max(abs(float(v)) for v in h[1:])
        so = max(abs(a - float(b)) for a, b in zip(off[1:], h[1:])) / gs
        sn = max(abs(a - float(b)) for a, b in zip(on[1:], h[1:])) / gs
        rows.append(dict(max_off=max(eo), max_on=max(en), med_off=statistics.median(eo), med_on=statistics.median(en),
                         lp_off=eo[0], lp_on=en[0], nchg=sum(1 for a, b in zip(off, on) if a != b),
                         norm_off=so / 2.0 ** -52, norm_on=sn / 2.0 ** -52, comps_off=eo, comps_on=en))
    return rows

if __name__ == '__main__':
    model = sys.argv[1]; npts = int(sys.argv[2]); sd = float(sys.argv[3]); seed = int(sys.argv[4])
    rows = main(model, npts, sd, seed)
    json.dump(rows, open(f'{R}/hp_pe/multipoint_{model}_{sd}_{seed}.json', 'w'))
    def S(key): return [r[key] for r in rows]
    allo = [x for r in rows for x in r['comps_off']]; alln = [x for r in rows for x in r['comps_on']]
    print(f'{model}: {npts} random points, sd={sd}, seed={seed}; {len(allo)} components each')
    print('  all components ULP:   off max %.2f median %.2f mean %.2f | on max %.2f median %.2f mean %.2f' % (max(allo), statistics.median(allo), statistics.mean(allo), max(alln), statistics.median(alln), statistics.mean(alln)))
    print('  per-point max ULP:    off median %.2f mean %.2f | on median %.2f mean %.2f' % (statistics.median(S('max_off')), statistics.mean(S('max_off')), statistics.median(S('max_on')), statistics.mean(S('max_on'))))
    print('  per-point norm-wise error (units of 2^-52 x max|grad|): off median %.2f mean %.2f | on median %.2f mean %.2f' % (statistics.median(S('norm_off')), statistics.mean(S('norm_off')), statistics.median(S('norm_on')), statistics.mean(S('norm_on'))))
    w = sum(1 for r in rows if r['max_on'] < r['max_off'] - 0.5); l = sum(1 for r in rows if r['max_on'] > r['max_off'] + 0.5)
    print('  points where on has smaller max-ULP: %d, larger: %d, tie: %d; points with any value changed: %d' % (w, l, npts - w - l, sum(1 for r in rows if r['nchg'])))
    w = sum(1 for r in rows if r['norm_on'] < r['norm_off']); print('  norm-wise: on better at %d of %d points' % (w, npts))
    print('  lp ULP: off max %.2f mean %.2f | on max %.2f mean %.2f' % (max(S('lp_off')), statistics.mean(S('lp_off')), max(S('lp_on')), statistics.mean(S('lp_on'))))
    print('  points with max ULP > 10: off %d, on %d' % (sum(1 for x in S('max_off') if x > 10), sum(1 for x in S('max_on') if x > 10)))
