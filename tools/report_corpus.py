#!/usr/bin/env python3
"""Export a completed corpus sweep, keeping failed seeds in its denominators.

Usage: report_corpus.py RUN_DIRECTORY DIAGNOSTICS_JSON OUTPUT_DIRECTORY
This is post-processing only. All durations come from the retained run events.
"""
import csv
import json
import math
import pathlib
import statistics
import sys

from corpus_inventory import local_cases

REPO = pathlib.Path(__file__).resolve().parent.parent
ENGINES = ('stanli', 'cmdstan')
AUDITED_RUN = 'fae5494c296cd547'
# Practical elapsed-time target; independent of the experiment timeout and diagnostics.
MIN_CLI_RATIO = 0.8


def environment_note(manifest):
    if manifest['run_id'] == AUDITED_RUN:
        return ('Fresh Release build on Apple M3 Ultra, 96 GiB RAM, macOS ARM64. Runtime and compiler '
                'sources match main `2ae6c1d0`; the manifest records the exact branch, dependencies, and executable hashes.')
    identity = manifest['identity']
    machine = identity['machine']
    return (f"Recorded platform: {machine['platform']}; {machine['logical_cpus']} logical CPUs. "
            f"Source checkout: `{identity['stanli_source']['head']}`. "
            'The manifest records tracked differences, configuration, dependencies, and executable hashes.')


def collection(name, declared=None):
    """Use recorded source metadata, with local lookup for historical runs."""
    if declared is not None:
        if not isinstance(declared, str) or not declared:
            raise ValueError('Invalid recorded collection for ' + name)
        return declared
    case = local_cases().get(name)
    return case.collection if case is not None else 'unclassified'


def summarize_engine(runs, seeds):
    if any(r['status'] == 'ok' and (not math.isfinite(r['elapsed_s']) or r['elapsed_s'] <= 0)
           for r in runs):
        raise ValueError('Successful runs need finite, positive elapsed times')
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


def row_for(record, diagnostic, events, seeds, declared_collection=None):
    row = dict(model=record['model'], collection=collection(record['model'], declared_collection),
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
    row['cmdstan_over_stanli_cli_ratio'] = cli_ratio(row)
    row['cli_target'] = cli_target(row)
    return row


def complete(row):
    return all(row['engines'][e]['status'] == 'complete' for e in ENGINES)


def cli_ratio(row):
    if not complete(row):
        return None
    return row['engines']['cmdstan']['median_s'] / row['engines']['stanli']['median_s']


def cli_target(row):
    ratio = cli_ratio(row)
    if ratio is None:
        return 'unmeasured'
    return 'met' if ratio >= MIN_CLI_RATIO else 'below'


def screen(result):
    d = result['diagnostics']
    return ('clear' if not d['screening_flag'] else 'review') if d['status'] == 'complete' else 'incomplete'


def summarize_group(rows):
    pairs = [r for r in rows if complete(r)]
    ratios = [cli_ratio(r) for r in pairs]
    first = [r for r in pairs if all(r['engines'][e].get('estimated_first_fit_s') is not None for e in ENGINES)]
    return dict(fixtures=len(rows), completed_in_both=len(pairs), incomplete=len(rows) - len(pairs),
                stanli_lower_cli_time=sum(r > 1 for r in ratios),
                cli_target_met=sum(r >= MIN_CLI_RATIO for r in ratios),
                cli_target_below=sum(r < MIN_CLI_RATIO for r in ratios),
                cli_target_unmeasured=len(rows) - len(pairs),
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
    if not config['sampling']:
        raise ValueError('Expected a sampling run; use corpus_table.py for gradient-only reports')
    records = [json.loads(p.read_text()) for p in sorted(run.glob('*.result.json'))]
    if (len({r['model'] for r in records}) != len(records) or
            {r['model'] for r in records} != set(manifest['identity']['inputs'])):
        raise ValueError('Run is unfinished: some manifest inputs have no result')
    diagnostics = json.loads(diagnostics.read_text())
    if any(diagnostics.get(r['model'], {}).get('run_id') != manifest['run_id'] for r in records):
        raise ValueError('Diagnostics must come from this exact benchmark run')
    for record in records:
        name = record['model']
        if record.get('row', {}).get('run_id') != manifest['run_id']:
            raise ValueError(f'{name}: result belongs to a different benchmark run')
        expected = manifest['identity']['inputs'][name]
        for field in ('stan', 'data'):
            expected_hash = expected.get(field)
            if (not isinstance(expected_hash, str) or not expected_hash or
                    record.get('inputs', {}).get(field) != expected_hash):
                raise ValueError(f'{name}: result {field} hash does not match the manifest')
    events = [json.loads(line) for line in (run / 'events.jsonl').read_text().splitlines()]
    rows = [row_for(r, diagnostics[r['model']], events, config['seeds'],
                    manifest['identity']['inputs'][r['model']].get('collection'))
            for r in records]
    groups = {g: summarize_group([r for r in rows if r['collection'] == g])
              for g in sorted({r['collection'] for r in rows})}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'corpus-results.json').write_text(json.dumps(
        dict(run_id=manifest['run_id'], manifest=manifest, min_cli_ratio=MIN_CLI_RATIO,
             summary=summarize_group(rows), collections=groups, rows=rows),
        indent=2, allow_nan=False) + '\n')
    fields = ['collection', 'model', 'status', 'failure_reason', 'cmdstan_compile_s',
              'cmdstan_over_stanli_cli_ratio', 'cli_target']
    for engine in ENGINES:
        fields += [engine + '_' + k for k in ('median_s', 'min_s', 'max_s', 'estimated_first_fit_s',
                   'min_bulk_ess_per_cli_second', 'screen', 'rhat_max', 'ess_bulk_min',
                   'ess_tail_min', 'divergences', 'max_depth_hits', 'ebfmi_min')]
    fields += ['stanli_ns_grad', 'stanli_ns_grad_mad', 'cmdstan_ns_grad', 'cmdstan_ns_grad_mad',
               'paired_speedup', 'paired_speedup_mad']
    with (output / 'corpus-timings.csv').open('w', newline='',
                                               encoding='utf-8') as stream:
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
    md = ['# Corpus model performance', '', environment_note(manifest), '',
          f"Run `{manifest['run_id']}` includes every one of its {len(rows)} fixtures. Failures and timeouts remain in the tables.", '',
          '## Run summary', '',
          '| Collection | Fixtures | Completed in both | Lower Stanli CLI time | Median CLI sampling time ratio (CmdStan/Stanli) | Screen clear in both |',
          '| --- | ---: | ---: | ---: | ---: | ---: |']
    for group, summary in [('All selected fixtures', summarize_group(rows)), *groups.items()]:
        n = summary['completed_in_both']
        md.append(f"| {group} | {summary['fixtures']} | {n} | {summary['stanli_lower_cli_time']}/{n} | "
                  f"{number(summary['median_cmdstan_over_stanli_cli_ratio'])} | {summary['diagnostic_screen_clear_in_both']}/{n} |")
    md += ['', 'The summary compares end-to-end CLI sampling time, including Stanli preparation. '
           'A ratio above one means less elapsed time for Stanli. Each model has equal weight in the median; '
           'these ratios describe the completed fixtures only. Diagnostic flags are not removed from the timing summary.', '',
           '## Practical timing target', '',
           f'The target is CmdStan/Stanli ≥ {MIN_CLI_RATIO:g}: Stanli takes at most '
           f'{1 / MIN_CLI_RATIO:g}× CmdStan’s complete CLI time. This is a timing target, '
           'not a claim of numerical correctness or reliable inference. Capped and failed runs remain '
           'unmeasured; the timeout is not the success threshold.', '',
           '| Collection | Target met | Below target | Unmeasured |',
           '| --- | ---: | ---: | ---: |']
    for group, summary in [('All selected fixtures', summarize_group(rows)), *groups.items()]:
        md.append(f"| {group} | {summary['cli_target_met']}/{summary['fixtures']} | "
                  f"{summary['cli_target_below']} | {summary['cli_target_unmeasured']} |")
    md += ['',
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
           ((f"CmdStan ran first for each seed. Stanli's cap was min({config['cmdstan_runtime_multiple']:g} × that CmdStan CLI time, "
             f"{config['sample_timeout']} seconds). ") if config.get('cmdstan_runtime_multiple') is not None else
            f"Engine order alternated across seeds; each run had a {config['sample_timeout']} second cap. ") +
           "A failed, invalid, or capped seed prevents an aggregate for that engine. "
           'A preceding preparation or gradient-gate failure leaves sampling unmeasured. No partial-seed averages are substituted.', '',
           'Screen clear: no retained-draw divergences or depth hits, finite R-hat ≤ 1.01 and bulk ESS ≥ 400 for every '
           'nonconstant variable in the parameters block. Structural fixed matrix entries are checked and omitted. The CSV also records tail ESS, '
           'minimum E-BFMI, and minimum bulk ESS divided by the sum of the serial CLI durations. '
           'These are descriptive diagnostics; fixed-budget runtime is not time to equal inferential accuracy.', '',
           f"Gradient results in the CSV use {config['rounds']} alternating pairs at the same parameter point. "
           '`paired_speedup` is the CmdStan/Stanli gradient time ratio; values above one favor Stanli. Each pair must satisfy '
           'the full density/gradient scaled-error gate of 1e-9. The separate three-point numerical replay and its '
           'known exceptions are described in the [testing guide](../../TESTING.md).', '',
           '## Full appendix', '',
           '| Collection / fixture | Stanli CLI | CmdStan CLI | CLI ratio C/S | Timing target | CmdStan compile | Screen S/C |',
           '| --- | ---: | ---: | ---: | --- | ---: | --- |']
    for row in rows:
        md.append(f"| {row['collection']} / {row['model']} | {time_cell(row, 'stanli')} | {time_cell(row, 'cmdstan')} | "
                  f"{number(row['cmdstan_over_stanli_cli_ratio'])} | {row['cli_target']} | "
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
           'python3 tools/corpus_diagnostic_jobs.py RUN_DIRECTORY /tmp/corpus-jobs.json',
           'Rscript tools/summarize_corpus_bench.R /tmp/corpus-jobs.json /tmp/corpus-diagnostics.json',
           'python3 tools/report_corpus.py RUN_DIRECTORY /tmp/corpus-diagnostics.json output/corpus-performance',
           '```', '', 'The jobs file contains absolute CSV paths; regenerate it after relocating the evidence directory. '
           'The exporter refuses an unfinished sweep. See the [benchmark method](../../docs/benchmarks.md#how-we-measure).', '']
    (output / 'README.md').write_text('\n'.join(md), encoding='utf-8')
    return groups


if __name__ == '__main__':
    print(json.dumps(export(*map(pathlib.Path, sys.argv[1:])), indent=2))
