#!/usr/bin/env python3
"""Prepare post-run parameter diagnostics from a retained corpus benchmark.

Usage: corpus_diagnostic_jobs.py RUN_DIRECTORY OUTPUT_JSON
Uses the independently generated CmdStan header to identify declared parameters,
then matches their flat CSV columns. Does not run or change a benchmark.
"""
import csv
import json
import pathlib
import re
import sys


def parameter_names(header):
    match = re.search(r'get_param_names\(.*?names__\s*=\s*std::vector<std::string>\s*\{(.*?)\}',
                      header, re.S)
    if not match:
        raise ValueError('CmdStan header has no declared parameter-name vector')
    return re.findall(r'"([^"\\]+)"', match[1])


def fixed_matrix_entries(source, columns):
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    declarations = dict((name, kind) for kind, name in re.findall(
        r'\b(corr_matrix|cholesky_factor_corr|cholesky_factor_cov)\s*\[[^]]+\]\s*(\w+)', source))
    result = {}
    for column in columns:
        parts = re.split(r'[.\[\],]+', column.rstrip(']'))
        kind = declarations.get(parts[0])
        if kind is None or len(parts) < 3:
            continue
        i, j = map(int, parts[-2:])
        if kind == 'corr_matrix' and i == j:
            result[column] = 1
        elif kind.startswith('cholesky_factor') and i < j:
            result[column] = 0
        elif kind == 'cholesky_factor_corr' and i == j == 1:
            result[column] = 1
    return result


def jobs_for(directory):
    manifest = json.loads((directory / 'manifest.json').read_text())
    run_id = manifest['run_id']
    config = manifest['identity']['config']
    seeds = config['seeds']
    samples = config['iter_sampling']
    if (not isinstance(seeds, list) or len(seeds) < 2 or
            any(type(seed) is not int or seed < 1 for seed in seeds) or
            len(set(seeds)) != len(seeds)):
        raise ValueError('Manifest must declare at least two distinct positive seeds')
    if type(samples) is not int or samples < 1:
        raise ValueError('Manifest must declare a positive retained-draw count')
    jobs = []
    for file in sorted(directory.glob('*.result.json')):
        record = json.loads(file.read_text())
        name = record['model']
        files = {engine: [] for engine in ('stanli', 'cmdstan')}
        seen = set()
        for event in record['sampling']:
            pair = (event['engine'], event['seed'])
            if pair in seen or pair[0] not in files or pair[1] not in seeds:
                raise ValueError(f'{name}: duplicate or unexpected engine/seed event: {pair}')
            seen.add(pair)
            if event['status'] == 'ok':
                path = (directory / event['stdout'] if event['engine'] == 'stanli' else
                        directory / name / f"sample-{event['seed']}.csv")
                files[event['engine']].append(str(path.resolve()))
        first = next((p for values in files.values() for p in values), None)
        if first is None:
            jobs.append(dict(model=name, run_id=run_id, expected_seeds=seeds, iter_sampling=samples,
                             columns=[], constants={}, files=files))
            continue
        header = (directory / name / f'{name}.hpp').read_text()
        parameters = set(parameter_names(header))
        with open(first) as stream:
            columns = next(csv.reader(line for line in stream if line.strip() and not line.startswith('#')))
        columns = [c for c in columns if re.split(r'[.\[]', c)[0] in parameters]
        if not columns:
            raise ValueError(f'{name}: no parameter columns found')
        source = (directory / 'inputs' / f'{name}.stan').read_text()
        constants = fixed_matrix_entries(source, columns)
        jobs.append(dict(model=name, run_id=run_id, expected_seeds=seeds, iter_sampling=samples,
                         columns=[c for c in columns if c not in constants],
                         constants=constants, files=files))
    return jobs


if __name__ == '__main__':
    directory, output = map(pathlib.Path, sys.argv[1:])
    output.write_text(json.dumps(jobs_for(directory), indent=2) + '\n')
