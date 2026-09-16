#!/usr/bin/env python3
"""Record descriptive errors and paired values for the Rethinking report.

No reference is rewritten. Use the same frozen checker and inputs as the timing
run; the build identity must contain the checker's SHA256.
"""
import argparse
import concurrent.futures
import gzip
import hashlib
import json
import math
import pathlib
import subprocess

from verify_refs import parse_status, parse_wa, ulp_distance


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def errors(reference, observed):
    if len(reference) != len(observed) or not all(math.isfinite(x) for x in reference + observed):
        raise ValueError('Expected paired finite numerical values')
    return dict(count=len(reference),
                max_absolute=max((abs(a - b) for a, b in zip(reference, observed)), default=0),
                max_ulp=max((ulp_distance(a, b) for a, b in zip(reference, observed)), default=0))


def export(run, references, build_identity, check, output, jobs=4):
    manifest = json.loads((run / 'manifest.json').read_text())
    identity = json.loads(build_identity.read_text())
    expected = [value for name, value in identity['files'].items()
                if pathlib.Path(name).resolve() == check.resolve()]
    if expected != [sha(check)]:
        raise ValueError('Checker does not match the recorded build identity')
    refs = json.loads(gzip.decompress(references.read_bytes()))
    models = refs['models']
    if len(models) != 62 or not all(n.startswith(('ch', 'extra_')) for n in models):
        raise ValueError('Expected all 62 Rethinking reference fixtures')

    def replay(name):
        inputs = manifest['identity']['inputs'][name]
        stan, data = (run / 'inputs' / (name + suffix) for suffix in ('.stan', '.json'))
        if sha(stan) != inputs['stan'] or sha(data) != inputs['data']:
            raise ValueError('Frozen input mismatch: ' + name)
        points = {}
        combined = {k: dict(count=0, max_absolute=0, max_ulp=0)
                    for k in ('log_density', 'gradient', 'outputs')}
        for point in ('0', '1', '2'):
            ref = models[name]['points'][point]
            proc = subprocess.run([str(check.resolve()), str(stan.resolve()), str(data.resolve()),
                                   '--point', point, '--wa-values'], text=True,
                                  capture_output=True, timeout=300, check=True)
            status, wa = parse_status(proc.stdout), parse_wa(proc.stdout)
            if not status or status[0] != 'OK' or wa is None or wa[0] != ref['wa']['names']:
                raise ValueError(f'Numerical replay or output-name mismatch: {name} point {point}')
            reference = list(map(float, ref['values']))
            observed = list(map(float, status[1:]))
            groups = {'log_density': (reference[:1], observed[:1]),
                      'gradient': (reference[1:], observed[1:]),
                      'outputs': (list(map(float, ref['wa']['values'])), list(map(float, wa[1])))}
            details = {}
            for group, (rv, ov) in groups.items():
                measured = errors(rv, ov)
                details[group] = dict(reference=rv, stanli=ov, **measured)
                combined[group]['count'] += measured['count']
                for metric in ('max_absolute', 'max_ulp'):
                    combined[group][metric] = max(combined[group][metric], measured[metric])
            points[point] = dict(output_names=wa[0], values=details)
        return name, combined, points

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        measured = sorted(pool.map(replay, sorted(models)))
    if sha(check) != expected[0]:
        raise ValueError('Checker changed during replay')
    provenance = dict(run_id=manifest['run_id'], runtime_revision=identity['head'],
                      checker_sha256=expected[0], reference_sha256=sha(references),
                      reference_toolchain=refs['recorded'], points=[0, 1, 2])
    summary = dict(**provenance, models={n: e for n, e, _ in measured})
    raw = dict(**provenance, models={n: p for n, _, p in measured})
    output.mkdir(parents=True, exist_ok=True)
    (output / 'numerical-errors.json').write_text(json.dumps(summary, indent=2, allow_nan=False) + '\n')
    (output / 'numerical-values.json.gz').write_bytes(gzip.compress(
        (json.dumps(raw, sort_keys=True, allow_nan=False) + '\n').encode(), mtime=0))
    print(json.dumps({g: max(e[g]['max_absolute'] for _, e, _ in measured)
                      for g in ('log_density', 'gradient', 'outputs')}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'references', 'build_identity', 'check', 'output'):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    export(**vars(args))
