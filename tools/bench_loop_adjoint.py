#!/usr/bin/env python3
"""Paired native complete-solve measurements for the developer loop probe.

An optional separately saved mapped-trace binary adds the second history design.
Every process requires exact local/solver values and derivatives and equal
callback counts before timing. These measurements exclude model preparation.
"""
import argparse
import json
import pathlib
import re
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default='build-release')
    parser.add_argument('--mapped-bin', type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    parser.add_argument('--repeats', type=int, default=6)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error('--repeats must be positive')
    root = pathlib.Path(__file__).resolve().parents[1]
    build = pathlib.Path(args.build).resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    modes = {'expanded': build / 'bench_loop_adjoint'}
    if args.mapped_bin:
        modes['mapped'] = args.mapped_bin.resolve()
    rows = []
    for rep in range(args.repeats):
        for n in (8, 128, 2048):
            data = out / f'N{n}.json'
            data.write_text(json.dumps({'N': n}))
            for case, stem in [('loop', 'ode_retained_loop'),
                               ('branch', 'ode_structured_callback_branch')]:
                order = list(modes)
                if rep % 2:
                    order.reverse()
                for mode in order:
                    command = [str(modes[mode]),
                               str(root / f'tests/fixtures/{stem}.tmir.sexp'),
                               str(data), '--provider', 'loop', '--iterations',
                               str(30 if n < 2048 else 3), '--batches', '2',
                               '--warmup-ms', '50', '--require-exact']
                    result = subprocess.run(command, text=True, capture_output=True,
                                            check=True, timeout=120, cwd=root)
                    (out / f'{case}-{n}-{mode}-{rep}.log').write_text(
                        result.stdout + result.stderr)
                    match = re.search(
                        r'median oracle_ns=([\d.]+) candidate_ns=([\d.]+)',
                        result.stdout)
                    if not match:
                        raise RuntimeError('missing benchmark result')
                    rows.append(dict(case=case, N=n, mode=mode, repeat=rep,
                                     oracle_ns=float(match[1]),
                                     candidate_ns=float(match[2]), exact=True))
                    (out / 'solve-raw.json').write_text(json.dumps(rows, indent=2) + '\n')
    summary = {}
    for key in sorted({(r['case'], r['N'], r['mode']) for r in rows}):
        group = [r for r in rows if (r['case'], r['N'], r['mode']) == key]
        metrics = {}
        for metric in ('oracle_ns', 'candidate_ns'):
            values = [r[metric] for r in group]
            mid = statistics.median(values)
            metrics[metric] = dict(median=mid, mad=statistics.median(
                abs(v - mid) for v in values))
        summary['-'.join(map(str, key))] = metrics
    (out / 'solve-summary.json').write_text(json.dumps(summary, indent=2) + '\n')


if __name__ == '__main__':
    main()
