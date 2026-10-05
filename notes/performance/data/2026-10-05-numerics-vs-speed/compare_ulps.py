import hp_reference as H, subprocess, os, statistics, math
from hp_reference import mp, MODELS, uerr
R = H.R
def run(model, pt, off):
    env = dict(os.environ)
    if off: env.update(X_NO_PART_SHARED='1', X_NO_RR_SHARED='1')
    out = subprocess.run([f'{R}/build-rel/stanli_check', f'{R}/tests/brms/{model}.stan', f'{R}/tests/brms/{model}.json', '--point', str(pt)], capture_output=True, text=True, env=env, cwd=R).stdout
    f = [l for l in out.splitlines() if l.startswith('OK')][-1].split()
    return [float(x) for x in f[1:]]
names = {'s2_discrete_weibull': ['lp', 'g_b', 'g_Intercept', 'g_log_shape'],
         's2_hurdle_negbin': ['lp', 'g_b', 'g_Intercept', 'g_log_shape', 'g_logit_hu']}
allerr = {}
for model in names:
    print('###', model)
    print('%-12s %2s %-28s %8s %8s %8s  %s' % ('comp', 'pt', 'hp (25 digits)', 'cmdstan', 'check_on', 'check_off', 'most accurate'))
    E = {'cmdstan': [], 'on': [], 'off': []}
    for pt in (0, 1, 2):
        h = H.hp(model, pt)
        ref = [float(s) for s in MODELS[model]['points'][str(pt)]['values']]
        on = run(model, pt, False); off = run(model, pt, True)
        for i, n in enumerate(names[model]):
            e = [uerr(ref[i], h[i]), uerr(on[i], h[i]), uerr(off[i], h[i])]
            for k, v in zip(E, e): E[k].append(v)
            best = min(range(3), key=lambda j: e[j])
            tag = 'tie' if max(e) - min(e) < 0.5 else ['cmdstan', 'on', 'off'][best]
            print('%-12s %2d %-28s %8.2f %8.2f %8.2f  %s' % (n, pt, mp.nstr(h[i], 25), e[0], e[1], e[2], tag))
    for k in E:
        print('  %-8s max %.2f median %.2f' % (k, max(E[k]), statistics.median(E[k])))
