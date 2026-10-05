import sys, pathlib, subprocess, tempfile, json, concurrent.futures, math
R = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(R / 'tools'))
import verify_refs as V
pdb = R / 'deps' / 'posteriordb'
refs, _ = V.replay_refs(V.native_platform(), V.runtime_compiler(R / 'build-off' / 'stanli_check'))
tmp = pathlib.Path(tempfile.mkdtemp(prefix='onoff_'))

def run(binary, stan, dj, pt):
    try:
        p = subprocess.run([str(R / binary / 'stanli_check'), str(stan), str(dj), '--point', str(pt)],
                           capture_output=True, text=True, cwd=R, timeout=600)
    except subprocess.TimeoutExpired:
        return None
    got = V.parse_status(p.stdout)
    if not got or got[0] != 'OK': return got[:1] or ['CRASH']
    return [float(x) for x in got[1:]]

def one(model):
    ref = refs[model]
    stan, dj = V.model_files(model, ref, pdb, tmp)
    res = []
    for pt in V.POINTS:
        r = ref['points'].get(str(pt))
        off = run('build-off', stan, dj, pt); on = run('build-on', stan, dj, pt)
        if not (isinstance(off, list) and isinstance(on, list)):
            res.append({'pt': pt, 'status': f'off={off if not isinstance(off,list) else "ok"} on={on if not isinstance(on,list) else "ok"}'})
            continue
        d = {'pt': pt, 'n': len(off), 'status': 'ok'}
        worst_rel, worst_ulp, nchg = 0.0, 0, 0
        for a, b in zip(off, on):
            if a != b: nchg += 1
            rel, ulp = V.pair_dev(a, b)
            worst_rel = max(worst_rel, rel); worst_ulp = max(worst_ulp, ulp)
        d.update(on_vs_off_rel=worst_rel, on_vs_off_ulp=worst_ulp, nchanged=nchg)
        if r and 'values' in r and len(r['values']) == len(off):
            rv = [float(x) for x in r['values']]
            d['off_vs_cmd_rel'], d['off_vs_cmd_ulp'] = V.worst_pair(rv, off)
            d['on_vs_cmd_rel'], d['on_vs_cmd_ulp'] = V.worst_pair(rv, on)
        res.append(d)
    return model, res

models = sorted(refs)
if len(sys.argv) > 1: models = sys.argv[1:]
out = {}
with concurrent.futures.ThreadPoolExecutor(int(__import__('os').environ.get('JOBS', 6))) as ex:
    for model, res in ex.map(one, models):
        out[model] = res
        print(model, flush=True)
json.dump(out, open(R / 'hp_pe' / 'corpus_onoff.json', 'w'), indent=1)
