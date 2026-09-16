#!/usr/bin/env python3
"""Export a completed teaching sweep, keeping failed seeds in its denominators.

Usage: report_teaching.py RUN_DIRECTORY DIAGNOSTICS_JSON OUTPUT_DIRECTORY
This is post-processing only. All durations come from the retained run events.
"""
import csv
import json
import pathlib
import statistics
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
ENGINES = ('stanli', 'cmdstan')
AUDITED_RUN = 'fae5494c296cd547'


def environment_note(manifest):
    if manifest['run_id'] == AUDITED_RUN:
        return ('Fresh Release build on Apple M3 Ultra, 96 GiB RAM, macOS ARM64. Runtime and compiler '
                'sources match main `2ae6c1d0`; the manifest records the exact branch, dependencies, and executable hashes.')
    identity = manifest['identity']
    machine = identity['machine']
    return (f"Recorded platform: {machine['platform']}; {machine['logical_cpus']} logical CPUs. "
            f"Source checkout: `{identity['stanli_source']['head']}`. "
            'The manifest records tracked differences, configuration, dependencies, and executable hashes.')


def collection(name):
    if (REPO / 'tests/educational/models' / name / 'model.stan').is_file():
        return 'educational'
    for group in ('rethinking', 'brms'):
        if (REPO / 'tests' / group / (name + '.stan')).is_file():
            return group
    raise ValueError('Not a teaching fixture: ' + name)


def summarize_engine(runs, seeds):
    if sorted(r['seed'] for r in runs) != sorted(seeds):
        return dict(status='incomplete', completed=sum(r['status'] == 'ok' for r in runs))
    successful = [r['elapsed_s'] for r in runs if r['status'] == 'ok']
    if len(successful) != len(seeds):
        return dict(status='incomplete', completed=len(successful))
    return dict(status='complete', completed=len(seeds), median_s=statistics.median(successful),
                min_s=min(successful), max_s=max(successful), sum_s=sum(successful))


def compile_time(record, events):
    translation = [e for e in events if e['phase'] == record['model'] + '/stanc-cpp'
                   and e['status'] == 'ok']
    build = record['row'].get('cmdstan_build_s')
    if len(translation) != 1 or build is None:
        return None
    return translation[0]['elapsed_s'] + build


def row_for(record, diagnostic, events, seeds):
    row = dict(model=record['model'], collection=collection(record['model']),
               status=record['status'], failure_reason=record['row'].get('note', ''),
               inputs=record['inputs'], cmdstan_compile_s=compile_time(record, events),
               gradient={key: record['row'].get(key) for key in
                         ('stanli_ns_grad', 'stanli_ns_grad_mad', 'cmdstan_ns_grad',
                          'cmdstan_ns_grad_mad', 'paired_speedup', 'paired_speedup_mad')},
               engines={}, runs=[{k: event[k] for k in
                   ('engine', 'seed', 'status', 'elapsed_s', 'timeout_s', 'id', 'stdout',
                    'stderr', 'validation_error', 'error') if k in event}
                   for event in record['sampling']])
    for engine in ENGINES:
        runs = [r for r in record['sampling'] if r['engine'] == engine]
        result = summarize_engine(runs, seeds)
        result['diagnostics'] = diagnostic['engines'][engine]
        if result['status'] == 'complete':
            compilation = row['cmdstan_compile_s'] if engine == 'cmdstan' else 0
            result['estimated_first_fit_s'] = (result['median_s'] + compilation
                                               if compilation is not None else None)
            ess = result['diagnostics'].get('ess_bulk_min')
            result['min_bulk_ess_per_cli_second'] = ess / result['sum_s'] if ess is not None else None
        row['engines'][engine] = result
    return row


def complete(row):
    return all(row['engines'][e]['status'] == 'complete' for e in ENGINES)


def screen(result):
    d = result['diagnostics']
    return ('clear' if not d['screening_flag'] else 'review') if d['status'] == 'complete' else 'incomplete'


def summarize_group(rows):
    pairs = [r for r in rows if complete(r)]
    ratios = [r['engines']['cmdstan']['median_s'] / r['engines']['stanli']['median_s'] for r in pairs]
    first = [r for r in pairs if all(r['engines'][e].get('estimated_first_fit_s') is not None for e in ENGINES)]
    return dict(fixtures=len(rows), completed_in_both=len(pairs), incomplete=len(rows) - len(pairs),
                stanli_lower_cli_time=sum(r > 1 for r in ratios),
                median_cmdstan_over_stanli_cli_ratio=statistics.median(ratios) if ratios else None,
                first_fit_pairs=len(first),
                stanli_lower_estimated_first_fit=sum(r['engines']['stanli']['estimated_first_fit_s'] <
                    r['engines']['cmdstan']['estimated_first_fit_s'] for r in first),
                diagnostic_screen_clear_in_both=sum(all(screen(r['engines'][e]) == 'clear' for e in ENGINES) for r in pairs))


def number(value):
    if value is None:
        return '—'
    return f'{value:.3f}' if value < 1 else f'{value:.2f}' if value < 100 else f'{value:.1f}'


def time_cell(row, engine):
    result = row['engines'][engine]
    if result['status'] == 'complete':
        return f"{number(result['median_s'])} [{number(result['min_s'])}–{number(result['max_s'])}]"
    runs = [r for r in row['runs'] if r['engine'] == engine]
    if not runs:
        return 'not measured'
    capped = sum(r['status'] == 'timeout' for r in runs)
    return f'capped {capped}/{len(runs)}' if capped else 'not completed'


def export(run, diagnostics, output):
    manifest = json.loads((run / 'manifest.json').read_text())
    config = manifest['identity']['config']
    if config['corpus'] != 'teaching' or not config['sampling']:
        raise ValueError('Expected a complete teaching sampling run')
    records = [json.loads(p.read_text()) for p in sorted(run.glob('*.result.json'))]
    if {r['model'] for r in records} != set(manifest['identity']['inputs']):
        raise ValueError('Run is unfinished: some manifest inputs have no result')
    diagnostics = json.loads(diagnostics.read_text())
    if any(diagnostics.get(r['model'], {}).get('run_id') != manifest['run_id'] for r in records):
        raise ValueError('Diagnostics must come from this exact benchmark run')
    events = [json.loads(line) for line in (run / 'events.jsonl').read_text().splitlines()]
    rows = [row_for(r, diagnostics[r['model']], events, config['seeds']) for r in records]
    groups = {g: summarize_group([r for r in rows if r['collection'] == g])
              for g in ('educational', 'rethinking', 'brms')}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'teaching-results.json').write_text(json.dumps(
        dict(run_id=manifest['run_id'], manifest=manifest, collections=groups, rows=rows),
        indent=2, allow_nan=False) + '\n')
    fields = ['collection', 'model', 'status', 'failure_reason', 'cmdstan_compile_s']
    for engine in ENGINES:
        fields += [engine + '_' + k for k in ('median_s', 'min_s', 'max_s', 'estimated_first_fit_s',
                   'min_bulk_ess_per_cli_second', 'screen', 'rhat_max', 'ess_bulk_min',
                   'ess_tail_min', 'divergences', 'max_depth_hits', 'ebfmi_min')]
    fields += ['stanli_ns_grad', 'stanli_ns_grad_mad', 'cmdstan_ns_grad', 'cmdstan_ns_grad_mad',
               'paired_speedup', 'paired_speedup_mad']
    with (output / 'teaching-timings.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator='\n')
        writer.writeheader()
        for row in rows:
            flat = {k: row.get(k) for k in fields}
            flat.update(row['gradient'])
            for engine in ENGINES:
                result = row['engines'][engine]
                for key, value in result.items():
                    if engine + '_' + key in fields:
                        flat[engine + '_' + key] = value
                for key, value in result['diagnostics'].items():
                    if engine + '_' + key in fields:
                        flat[engine + '_' + key] = value
                flat[engine + '_screen'] = screen(result)
            writer.writerow(flat)
    md = ['# Teaching model performance', '', environment_note(manifest), '',
          f"Run `{manifest['run_id']}` includes every one of its {len(rows)} fixtures. Failures and timeouts remain in the tables.", '',
          '## Collection summary', '',
          '| Collection | Fixtures | Completed in both | Lower Stanli CLI time | Median CmdStan/Stanli CLI ratio | Screen clear in both |',
          '| --- | ---: | ---: | ---: | ---: | ---: |']
    for group, summary in groups.items():
        n = summary['completed_in_both']
        md.append(f"| {group} | {summary['fixtures']} | {n} | {summary['stanli_lower_cli_time']}/{n} | "
                  f"{number(summary['median_cmdstan_over_stanli_cli_ratio'])} | {summary['diagnostic_screen_clear_in_both']}/{n} |")
    md += ['', 'A ratio above one means less elapsed time for Stanli. Each model has equal weight in the median; '
           'these ratios describe the completed fixtures only. Diagnostic flags are not removed from the timing summary.', '',
           '## What was measured', '',
           f"Each engine ran {len(config['seeds'])} independent single-chain seeds, with {config['iter_warmup']} warmup iterations "
           f"and {config['iter_sampling']} retained draws per seed, target acceptance 0.8, tree depth 10, and random initialization. "
           'The table reports median [minimum–maximum] CLI seconds. Stanli includes model preparation; CmdStan starts from '
           'a compiled model. Both include generated quantities and CSV output. Toolchain installation is excluded.', '',
           ('CSV precision follows the CLI defaults: eight significant digits for CmdStan 2.39 and 17 for Stanli. '
            if manifest['run_id'] == AUDITED_RUN else 'CSV precision follows the recorded CLI defaults. ') +
           'The numerical oracle uses separate high-precision values; sampling diagnostics use these retained CSVs.', '',
           'CmdStan compilation is measured once, including Stan translation and the C++ model build. Adding it to the '
           'median CmdStan CLI time gives an estimated first fit; this is a sum of measured stages. '
           'It is not a directly timed four-chain R fit or a cold-cache measurement.', '',
           f"CmdStan ran first for each seed. Stanli's cap was min({config['cmdstan_runtime_multiple']:g} × that CmdStan CLI time, "
           f"{config['sample_timeout']} seconds). A failed, invalid, or capped seed prevents an aggregate for that engine. "
           'A preceding preparation or gradient-gate failure leaves sampling unmeasured. No partial-seed averages are substituted.', '',
           'Screen clear: no retained-draw divergences or depth hits, finite R-hat ≤ 1.01 and bulk ESS ≥ 400 for every '
           'nonconstant variable in the parameters block. Structural fixed matrix entries are checked and omitted. The CSV also records tail ESS, '
           'minimum E-BFMI, and minimum bulk ESS divided by the sum of the four serial CLI durations. '
           'These are descriptive diagnostics; fixed-budget runtime is not time to equal inferential accuracy.', '',
           'Gradient results in the CSV use six alternating pairs at the same parameter point. Each pair must satisfy '
           'the full density/gradient scaled-error gate of 1e-9. The separate three-point numerical replay and its '
           'known exceptions are described in the [support guide](../../docs/teaching-support.md).', '',
           '## Full appendix', '',
           '| Collection / fixture | Stanli CLI | CmdStan CLI | CmdStan compile | Screen S/C |',
           '| --- | ---: | ---: | ---: | --- |']
    for row in rows:
        md.append(f"| {row['collection']} / {row['model']} | {time_cell(row, 'stanli')} | {time_cell(row, 'cmdstan')} | "
                  f"{number(row['cmdstan_compile_s'])} | {screen(row['engines']['stanli'])}/{screen(row['engines']['cmdstan'])} |")
    md += ['', '## Incomplete fixtures', '']
    for row in rows:
        if complete(row):
            continue
        failures = [f"{r['engine']} seed {r['seed']}: {r['status']}" +
                    (f" (limit {r['timeout_s']:.3g} s)" if 'timeout_s' in r else '')
                    for r in row['runs'] if r['status'] != 'ok']
        detail = '; '.join(failures) or row['failure_reason'] or row['status']
        md.append(f"- **{row['model']}**: {detail.replace(chr(10), ' ')}")
    if manifest['run_id'] == AUDITED_RUN:
        md += ['', 'The three GP failures (`i320_gp_expquad`, `s2_gp_by_gr`, `sw_gp`) exceeded this '
               'benchmark\'s strict 1e-9 gradient gate. They are the already documented ill-conditioned '
               'fixtures; the numerical replay applies its existing recorded exceptions. `s2_com_poisson` '
               'is the known support gap, and `s2_invgaussian` has an invalid shared reference point. '
               'See the [brms corpus notes](../../tests/brms/README.md). These models were not sampled in this run.', '',
               'Performance follow-ups: [Rethinking #373](https://github.com/seantalts/stanli/issues/373) '
               'and [brms #374](https://github.com/seantalts/stanli/issues/374). The current-build '
               'Rethinking preparation failure is [#372](https://github.com/seantalts/stanli/issues/372).']
    md += ['', '## Reproduce the tables', '',
           'Retain the original run directory, including model headers, inputs, logs and per-seed CSVs. After the '
           'timed sweep finishes, run:', '', '```sh',
           'python3 tools/teaching_diagnostic_jobs.py RUN_DIRECTORY /tmp/teaching-jobs.json',
           'Rscript tools/summarize_rethinking_bench.R /tmp/teaching-jobs.json /tmp/teaching-diagnostics.json',
           'python3 tools/report_teaching.py RUN_DIRECTORY /tmp/teaching-diagnostics.json output/teaching-performance',
           '```', '', 'The jobs file contains absolute CSV paths; regenerate it after relocating the evidence directory. '
           'The exporter refuses an unfinished sweep. See the [benchmark protocol](../../docs/benchmark-protocol.md).', '']
    (output / 'README.md').write_text('\n'.join(md))
    return groups


if __name__ == '__main__':
    print(json.dumps(export(*map(pathlib.Path, sys.argv[1:])), indent=2))
