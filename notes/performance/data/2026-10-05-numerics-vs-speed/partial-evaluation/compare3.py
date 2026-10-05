import sys, subprocess, statistics, pathlib, os
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hp_models as H
from hp_models import mp, mpf, uerr, R
MODELS_ = ['s2_gev', 's2_me2_nomecor', 'sw_me']

def run(binary, model, pt):
    out = subprocess.run([f'{R}/{binary}/stanli_check', f'{R}/tests/brms/{model}.stan',
                          f'{R}/tests/brms/{model}.json', '--point', str(pt)],
                         capture_output=True, text=True, cwd=R).stdout
    f = [l for l in out.splitlines() if l.startswith('OK')][-1].split()
    return [float(x) for x in f[1:]]

def main():
    verbose = '-v' in sys.argv
    allE = {}
    for model in MODELS_:
        E = {'cmdstan': [], 'off': [], 'on': []}
        per_pt = {}
        for pt in (0, 1, 2):
            h = H.hp(model, pt)
            ref = [float(s) for s in H.MODELS[model]['points'][str(pt)]['values']]
            off = run('build-off', model, pt); on = run('build-on', model, pt)
            assert len(ref) == len(off) == len(on) == len(h)
            e = {'cmdstan': [uerr(a, b) for a, b in zip(ref, h)],
                 'off': [uerr(a, b) for a, b in zip(off, h)],
                 'on': [uerr(a, b) for a, b in zip(on, h)]}
            per_pt[pt] = e
            for k in E: E[k] += e[k]
            if verbose:
                for i in range(len(h)):
                    print(model, pt, i, mp.nstr(h[i], 20), 'ulp cmd %.2f off %.2f on %.2f' % (e['cmdstan'][i], e['off'][i], e['on'][i]),
                          'DIFF' if off[i] != on[i] else '')
        print('###', model, '(%d components x 3 points)' % len(h))
        print('%-8s %8s %8s %8s' % ('', 'cmdstan', 'off', 'on'))
        for pt in (0, 1, 2):
            e = per_pt[pt]
            print('pt%d max   %8.2f %8.2f %8.2f' % (pt, max(e['cmdstan']), max(e['off']), max(e['on'])))
            print('pt%d median%8.2f %8.2f %8.2f' % (pt, statistics.median(e['cmdstan']), statistics.median(e['off']), statistics.median(e['on'])))
            print('pt%d mean  %8.2f %8.2f %8.2f' % (pt, statistics.mean(e['cmdstan']), statistics.mean(e['off']), statistics.mean(e['on'])))
        print('all max    %8.2f %8.2f %8.2f' % (max(E['cmdstan']), max(E['off']), max(E['on'])))
        print('all median %8.2f %8.2f %8.2f' % (statistics.median(E['cmdstan']), statistics.median(E['off']), statistics.median(E['on'])))
        print('all mean   %8.2f %8.2f %8.2f' % (statistics.mean(E['cmdstan']), statistics.mean(E['off']), statistics.mean(E['on'])))
        n = len(E['on'])
        print('components where on closer than off: %d, farther: %d, equal(<0.5ulp diff): %d (of %d)' % (
            sum(1 for a, b in zip(E['on'], E['off']) if b - a >= 0.5), sum(1 for a, b in zip(E['on'], E['off']) if a - b >= 0.5),
            sum(1 for a, b in zip(E['on'], E['off']) if abs(a - b) < 0.5), n))
        print('components where on worse than 10ulp: %d, off worse than 10 ulp: %d' % (sum(1 for a in E['on'] if a > 10), sum(1 for a in E['off'] if a > 10)))
if __name__ == "__main__":
    main()
