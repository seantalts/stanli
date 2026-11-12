"""Regression checks for the generated-docs values."""
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import gen_docs as docs


class GenDocs(unittest.TestCase):
    def test_current_headline_includes_all_application_collections(self):
        rows = [dict(model='pdb_a', paired_speedup='2'),
                dict(model='pdb_b', paired_speedup='4')]
        rows.extend(dict(model=group, paired_speedup='10')
                    for group in ('rethinking', 'brms', 'educational'))
        rows.append(dict(model='failed', paired_speedup=''))
        self.assertEqual(docs.corpus_stats(rows), (5, 10, 5))

    def test_cmdstan_row_names_the_release_only_when_the_head_is_the_pin(self):
        pin = ('d' * 40, '2.40.0')
        self.assertEqual(docs.cmdstan_label('d' * 40, pin), '2.40.0, `' + 'd' * 40 + '`')
        self.assertEqual(docs.cmdstan_label('a' * 40, pin),
                         '`' + 'a' * 40 + '` (not the pinned 2.40.0)')
        self.assertEqual(docs.cmdstan_label('a' * 40, ('d' * 40, None)), '`' + 'a' * 40 + '`')

    def test_the_pin_is_read_from_the_pins_file(self):
        sha, version = docs.cmdstan_pin()
        self.assertRegex(sha, r'^[0-9a-f]{40}$')
        self.assertRegex(version, r'^\d+\.\d+\.\d+$')

    def test_toolchain_rows_come_from_the_current_manifest(self):
        values = docs.compute()
        manifest = docs.current_benchmark()[2]['identity']['toolchains']
        self.assertIn(manifest['cmdstan']['head'], values['benchmark_cmdstan'])
        self.assertEqual(values['benchmark_stan_math'], '`' + manifest['math']['head'] + '`')

    def test_source_row_comes_from_the_current_manifest(self):
        values = docs.compute()
        source = docs.current_benchmark()[2]['identity']['stanli_source']['head']
        self.assertEqual(values['benchmark_source'], '`' + source + '`')

    def test_total_sampling_median_adds_setup_to_each_models_gradient_count(self):
        row = dict(paired_speedup='2', stanli_compile_s='0.5', stanli_prep_s='0.5',
                   stanli_ns_grad=str(1e9 / 4000), cmdstan_stanc_s='1',
                   cmdstan_build_s='5', cmdstan_ns_grad=str(2e9 / 4000), gradient_count='4000')
        fallback = dict(row, gradient_count='', stanli_ns_grad=str(1e9 / 5000),
                        cmdstan_ns_grad=str(2e9 / 5000))
        failed = dict(row, paired_speedup='')
        missing = dict(row, cmdstan_build_s='')
        self.assertEqual(docs.total_sampling_median([row, fallback, failed, missing]), 4)


if __name__ == '__main__':
    unittest.main()
