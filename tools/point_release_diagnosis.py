#!/usr/bin/env python3
"""Record a bounded, independent Intel CmdStan diagnosis; never update gates."""
import argparse
import hashlib
import json
import math
import pathlib
import platform
import subprocess

from cmdstan_ref import compile_cmd
from verify_refs import load_refs, model_files, pair_dev, parse_status, parse_wa

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cmdstan', type=pathlib.Path, required=True)
    ap.add_argument('--pdb', type=pathlib.Path, required=True)
    ap.add_argument('--check', type=pathlib.Path, required=True)
    ap.add_argument('--stanc', type=pathlib.Path, required=True)
    ap.add_argument('--output', type=pathlib.Path, required=True)
    args = ap.parse_args()
    for key in vars(args):
        setattr(args, key, getattr(args, key).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    events = []

    def run(name, argv, timeout=600):
        result = subprocess.run(list(map(str, argv)), capture_output=True,
                                text=True, timeout=timeout)
        (args.output / (name + '.stdout')).write_text(result.stdout)
        (args.output / (name + '.stderr')).write_text(result.stderr)
        events.append(dict(name=name, argv=list(map(str, argv)),
                           returncode=result.returncode))
        (args.output / 'commands.json').write_text(json.dumps(events, indent=2))
        if result.returncode:
            raise RuntimeError(f'{name} exited {result.returncode}; see retained logs')
        return result.stdout

    def git(path):
        return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'],
                                       text=True).strip()

    refs, recorded = load_refs()
    ref = refs['kronecker_gp']
    source, data = model_files('kronecker_gp', ref, args.pdb, args.output)
    header = args.output / 'kronecker_gp.hpp'
    executable = args.output / 'cmdstan-reference'
    run('stanc', [args.stanc, source, '--o=' + str(header)])
    command = compile_cmd(args.cmdstan, header, ROOT / 'tools/ref_driver.cpp',
                          executable, opt='-O1', sundials=False)
    run('compile-reference', command, timeout=1800)
    rig = dict(platform=f'{platform.system()} {platform.machine()}',
               compiler=run('compiler', ['clang++', '--version']).strip(),
               stanli=git(ROOT), cmdstan=git(args.cmdstan),
               stan=git(args.cmdstan / 'stan'),
               math=git(args.cmdstan / 'stan/lib/stan_math'),
               posteriordb=git(args.pdb), stanc3=args.stanc.with_suffix('.src').read_text().strip(),
               flags=command, source_sha256=digest(source), data_sha256=digest(data),
               driver_sha256=digest(ROOT / 'tools/ref_driver.cpp'),
               header_sha256=digest(header), stanc_sha256=digest(args.stanc),
               reference_sha256=digest(executable), check_sha256=digest(args.check))
    (args.output / 'provenance.json').write_text(json.dumps(rig, indent=2))
    for pin in ('cmdstan', 'stan', 'math', 'stanc3'):
        if rig[pin] != recorded[pin]:
            raise RuntimeError(f'Independent oracle {pin} pin differs from recording')
    if rig['platform'] != 'Darwin x86_64':
        raise RuntimeError('This diagnostic requires native Intel macOS')

    def comparisons(a, b):
        if len(a) != len(b):
            return dict(shape_mismatch=[len(a), len(b)])
        rows = []
        for index, (av, bv) in enumerate(zip(a, b)):
            rel, ulp = pair_dev(float(av), float(bv))
            if rel:
                rows.append(dict(index=index, expected=str(av), actual=str(bv),
                                 scaled_error=rel if math.isfinite(rel) else 'infinity', ulp=ulp))
        rows.sort(key=lambda r: float(r['scaled_error']), reverse=True)
        return dict(values=len(a), differing=len(rows), largest=rows[:20])

    summary = dict(provenance=rig, points={})
    for point in range(3):
        oracle = run(f'cmdstan-point-{point}', [executable, data, point])
        current = run(f'stanli-point-{point}', [args.check, source, data,
                                              '--point', point, '--wa-values'])
        expected, actual = parse_status(oracle), parse_status(current)
        if expected[:1] != ['OK'] or actual[:1] != ['OK']:
            raise RuntimeError(f'Point {point}: evaluation failed; raw outputs retained')
        wa_expected, wa_actual = parse_wa(oracle), parse_wa(current)
        detail = dict(cmdstan_values=expected[1:], stanli_values=actual[1:],
                      same_platform=comparisons(expected[1:], actual[1:]),
                      cmdstan_vs_arm64=comparisons(ref['points'][str(point)]['values'], expected[1:]),
                      stanli_vs_arm64=comparisons(ref['points'][str(point)]['values'], actual[1:]),
                      cmdstan_wa=wa_expected, stanli_wa=wa_actual)
        if wa_expected and wa_actual:
            detail['wa_names_match'] = wa_expected[0] == wa_actual[0]
            detail['wa_comparison'] = comparisons(wa_expected[1], wa_actual[1])
        summary['points'][str(point)] = detail
        (args.output / 'summary.json').write_text(json.dumps(summary, indent=2))
        print(f'point {point}: ' + json.dumps(detail['same_platform']))


if __name__ == '__main__':
    main()
