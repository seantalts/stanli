#!/usr/bin/env python3
"""Post-run diagnostics follow the recorded sampling protocol and fail closed."""
import json
import math
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from corpus_diagnostic_jobs import jobs_for


def fixture(directory, seeds=(7, 19), draws=40):
    (directory / 'manifest.json').write_text(json.dumps({
        'run_id': 'synthetic-diagnostics',
        'identity': {'config': {'seeds': list(seeds), 'iter_sampling': draws}}}))
    (directory / 'example').mkdir()
    (directory / 'inputs').mkdir()
    (directory / 'inputs/example.stan').write_text(
        'parameters {real theta; cholesky_factor_corr[2] L;} model {}')
    (directory / 'example/example.hpp').write_text(
        'void get_param_names() {names__ = std::vector<std::string>{"theta", "L"};}')
    events = []
    for engine in ('stanli', 'cmdstan'):
        for seed in seeds:
            path = (directory / f'stanli-{seed}.csv' if engine == 'stanli' else
                    directory / 'example' / f'sample-{seed}.csv')
            path.write_text('theta,L.1.1,divergent__,treedepth__,energy__\n' + ''.join(
                f'{math.sin(i+seed)},1,0,3,{1+i%7}\n' for i in range(draws)))
            events.append({'engine': engine, 'seed': seed, 'status': 'ok',
                           'stdout': str(path.relative_to(directory))})
    record = directory / 'example.result.json'
    record.write_text(json.dumps({'model': 'example', 'sampling': events}))
    return record


class CorpusDiagnosticJobsTests(unittest.TestCase):
    def test_job_keeps_manifest_seeds_draws_and_fixed_parameter_columns(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            fixture(directory)
            job, = jobs_for(directory)
            self.assertEqual(job['expected_seeds'], [7, 19])
            self.assertEqual(job['iter_sampling'], 40)
            self.assertEqual(job['columns'], ['theta'])
            self.assertEqual(job['constants'], {'L.1.1': 1})
            self.assertEqual(len(job['files']['stanli']), 2)

    def test_censored_chain_keeps_expected_denominator(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            record_path = fixture(directory)
            record = json.loads(record_path.read_text())
            record['sampling'][-1]['status'] = 'timeout'
            record_path.write_text(json.dumps(record))
            job, = jobs_for(directory)
            self.assertEqual(len(job['files']['cmdstan']), 1)
            self.assertEqual(job['expected_seeds'], [7, 19])

    def test_duplicate_or_unexpected_events_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            path = fixture(directory)
            original = json.loads(path.read_text())
            for event in ({**original['sampling'][0]},
                          {**original['sampling'][0], 'seed': 999},
                          {**original['sampling'][0], 'engine': 'unknown'}):
                path.write_text(json.dumps({**original, 'sampling': original['sampling'] + [event]}))
                with self.assertRaisesRegex(ValueError, 'duplicate or unexpected'):
                    jobs_for(directory)

    def test_invalid_manifest_protocol_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            fixture(directory)
            path = directory / 'manifest.json'
            manifest = json.loads(path.read_text())
            for seeds, draws in (([7, 7], 40), ([True, 19], 40), ([7], 40),
                                 ([7, 19], 0), ([7, 19], 1.5)):
                manifest['identity']['config'] = {'seeds': seeds, 'iter_sampling': draws}
                path.write_text(json.dumps(manifest))
                with self.assertRaises(ValueError):
                    jobs_for(directory)


class CorpusDiagnosticRTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rscript = shutil.which('Rscript')
        if not cls.rscript:
            raise unittest.SkipTest('Rscript is not installed')
        probe = subprocess.run([cls.rscript, '-e',
            'quit(status=if (requireNamespace("jsonlite", quietly=TRUE) && '
            'requireNamespace("posterior", quietly=TRUE)) 0L else 1L)'],
            capture_output=True, text=True, timeout=30)
        if probe.returncode:
            raise unittest.SkipTest('R jsonlite/posterior packages are not installed')

    def summarize(self, directory, jobs, script='summarize_corpus_bench.R'):
        path = directory / 'jobs.json'
        path.write_text(json.dumps(jobs))
        output = directory / 'diagnostics.json'
        result = subprocess.run([self.rscript, str(ROOT / 'tools' / script), str(path), str(output)],
                                capture_output=True, text=True, timeout=30)
        return result, output

    def test_nondefault_protocol_and_censoring_end_to_end(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            path = fixture(directory)
            result, output = self.summarize(directory, jobs_for(directory))
            self.assertEqual(result.returncode, 0, result.stderr)
            engines = json.loads(output.read_text())['example']['engines']
            for engine in engines.values():
                self.assertEqual(engine['status'], 'complete')
                self.assertEqual(engine['chains'], 2)
                self.assertEqual(engine['draws'], 80)
                self.assertEqual(engine['fixed_entries_omitted'], 1)
            record = json.loads(path.read_text())
            record['sampling'][-1]['status'] = 'timeout'
            path.write_text(json.dumps(record))
            result, output = self.summarize(directory, jobs_for(directory))
            self.assertEqual(result.returncode, 0, result.stderr)
            engines = json.loads(output.read_text())['example']['engines']
            self.assertEqual(engines['cmdstan'], {'status': 'incomplete', 'chains': 1})
            self.assertEqual(engines['stanli']['status'], 'complete')

    def test_bad_draw_count_missing_diagnostics_and_fixed_values_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            fixture(directory)
            jobs = jobs_for(directory)
            path = directory / 'example/sample-7.csv'
            original = path.read_text()
            for invalid in ('\n'.join(original.splitlines()[:-1]) + '\n',
                            original.replace('energy__', 'unrelated'),
                            original.replace(',1,0,3,', ',2,0,3,'),
                            original.replace(',1,0,3,', ',1,nan,3,')):
                path.write_text(invalid)
                result, _ = self.summarize(directory, jobs)
                self.assertNotEqual(result.returncode, 0, 'corrupt CSV unexpectedly accepted')

    def test_legacy_wrapper_preserves_archived_four_chain_protocol(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            fixture(directory, seeds=(1, 2, 3, 4), draws=1000)
            jobs = jobs_for(directory)
            for job in jobs:
                del job['expected_seeds']
                del job['iter_sampling']
            result, output = self.summarize(directory, jobs, 'summarize_rethinking_bench.R')
            self.assertEqual(result.returncode, 0, result.stderr)
            engines = json.loads(output.read_text())['example']['engines']
            self.assertEqual(engines['stanli']['chains'], 4)
            self.assertEqual(engines['stanli']['draws'], 4000)


if __name__ == '__main__':
    unittest.main()
