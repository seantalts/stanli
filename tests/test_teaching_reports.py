"""Regression checks for censoring and selecting diagnostic parameter columns."""
import importlib.util
import json
import pathlib
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent.parent


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


report = module('report_teaching')
jobs = module('teaching_diagnostic_jobs')
docs = module('gen_docs')


class TeachingReports(unittest.TestCase):
    def test_new_run_does_not_inherit_audited_hardware_claims(self):
        manifest = dict(run_id='a-different-run', identity=dict(
            machine=dict(platform='Linux x86_64', logical_cpus=8),
            stanli_source=dict(head='new-revision')))
        note = report.environment_note(manifest)
        self.assertIn('Linux x86_64', note)
        self.assertIn('new-revision', note)
        self.assertNotIn('Apple', note)
        self.assertNotIn('2ae6c1d0', note)

    def test_teaching_rows_do_not_change_posteriordb_headline(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'docs').mkdir()
            rows = ['model\tstanli_ns_grad\tcmdstan_ns_grad', 'pdb_a\t1\t2', 'pdb_b\t1\t4']
            for group in ('rethinking', 'brms', 'educational', 'stanc3'):
                directory = (root / 'tests/educational/models' / group if group == 'educational'
                             else root / 'tests' / group)
                directory.mkdir(parents=True)
                (directory / ('model.stan' if group == 'educational' else group + '.stan')).touch()
                rows.append(group + '\t1\t1000')
            (root / 'docs' / 'corpus-bench.tsv').write_text('\n'.join(rows) + '\n')
            with mock.patch.object(docs, 'REPO', root):
                self.assertEqual(docs.corpus_stats(), (2, 3, 2))

    def test_real_fixture_directory_layouts(self):
        self.assertEqual(report.collection('aalto_bern'), 'educational')
        self.assertEqual(report.collection('ch09_m9_1'), 'rethinking')
        self.assertEqual(report.collection('sw_gaussian'), 'brms')

    def test_export_rejects_missing_models_and_different_run_diagnostics(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = pathlib.Path(temporary)
            manifest = dict(run_id='new-run', identity=dict(
                config=dict(corpus='teaching', sampling=True), inputs=dict(aalto_bern={})))
            (run / 'manifest.json').write_text(json.dumps(manifest))
            diagnostics = run / 'diagnostics.json'
            diagnostics.write_text(json.dumps(dict(aalto_bern=dict(run_id='old-run'))))
            with self.assertRaisesRegex(ValueError, 'unfinished'):
                report.export(run, diagnostics, run / 'out')
            (run / 'aalto_bern.result.json').write_text(json.dumps(dict(model='aalto_bern')))
            with self.assertRaisesRegex(ValueError, 'this exact benchmark run'):
                report.export(run, diagnostics, run / 'out')

    def test_timeout_is_not_a_fast_partial_mean(self):
        runs = [dict(seed=i, status='ok', elapsed_s=float(i)) for i in range(1, 5)]
        runs[-1].update(status='timeout', elapsed_s=900)
        result = report.summarize_engine(runs, [1, 2, 3, 4])
        self.assertEqual(result, dict(status='incomplete', completed=3))
        self.assertNotIn('median_s', result)

    def test_missing_or_duplicate_seeds_are_incomplete(self):
        for seeds in ([1, 2, 3], [1, 2, 3, 3]):
            runs = [dict(seed=i, status='ok', elapsed_s=1) for i in seeds]
            self.assertEqual(report.summarize_engine(runs, [1, 2, 3, 4])['status'], 'incomplete')

    def test_serial_total_and_median_have_different_boundaries(self):
        runs = [dict(seed=i, status='ok', elapsed_s=t) for i, t in enumerate([1, 2, 8, 9], 1)]
        result = report.summarize_engine(runs, [1, 2, 3, 4])
        self.assertEqual(result['median_s'], 5)
        self.assertEqual(result['sum_s'], 20)
        self.assertEqual((result['min_s'], result['max_s']), (1, 9))

    def test_compile_time_excludes_gradient_driver_build(self):
        record = dict(model='example', row=dict(cmdstan_build_s=4))
        events = [dict(phase='example/stanc-cpp', status='ok', elapsed_s=.1),
                  dict(phase='example/build-gradient', status='ok', elapsed_s=90)]
        self.assertEqual(report.compile_time(record, events), 4.1)
        record['row'] = {}
        self.assertIsNone(report.compile_time(record, events))

    def test_declared_parameters_exclude_optional_output_blocks(self):
        header = '''void get_param_names(std::vector<std::string>& names__,
          const bool include_tparams__ = true, const bool include_gqs__ = true) {
          names__ = std::vector<std::string>{"mu", "theta", "L"};
          if (include_tparams__) names__.emplace_back("eta");
          if (include_gqs__) names__.emplace_back("log_lik");
        }'''
        self.assertEqual(jobs.parameter_names(header), ['mu', 'theta', 'L'])

    def test_fixed_entries_in_arrays_use_last_two_indices(self):
        source = '''parameters {
          array[2] cholesky_factor_corr[3] L;
          cholesky_factor_cov[3, 2] V;
          corr_matrix[3] C;
          matrix[3, 3] M; // M can sample to a constant; still diagnose it.
        }'''
        columns = ['L.2.1.1', 'L.2.1.2', 'L.2.2.1', 'L.2.2.2',
                   'V.1.1', 'V.1.2', 'V.3.2', 'C.2.2', 'C.1.2', 'M.1.1']
        self.assertEqual(jobs.fixed_matrix_entries(source, columns),
                         {'L.2.1.1': 1, 'L.2.1.2': 0, 'V.1.2': 0, 'C.2.2': 1})


if __name__ == '__main__':
    unittest.main()
