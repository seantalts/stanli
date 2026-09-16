#!/usr/bin/env python3
"""Build the McElreath report from a completed, immutable corpus run.

Usage: report_rethinking.py RUN_DIRECTORY DIAGNOSTICS_JSON REPLAY_LOG OUTPUT_DIR
Requires reportlab and numerical-errors.json in OUTPUT_DIR (from report_rethinking_numerics.py). No benchmarks run here.
"""
import csv
import json
import pathlib
import re
import shutil
import statistics
import sys
from xml.sax.saxutils import escape
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.pagesizes import A4
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak

REPO = pathlib.Path(__file__).resolve().parent.parent
RUN, DIAGNOSTICS, REPLAY, OUT = map(pathlib.Path, sys.argv[1:])
OUT.mkdir(parents=True, exist_ok=True)
manifest = json.loads((RUN / 'manifest.json').read_text())
assert manifest['run_id'] == 'fd5e0ecacddc7047', 'This report narrative is tied to its audited run; review it before using other results.'
assert manifest['identity']['config']['cmdstan_runtime_multiple'] == 3
assert manifest['identity']['config']['seeds'] == [1, 2, 3, 4]
assert manifest['identity']['config']['iter_sampling'] == 1000
assert manifest['identity']['config']['iter_warmup'] == 1000
records = {p.stem.removesuffix('.result'): json.loads(p.read_text()) for p in RUN.glob('*.result.json')}
diag = json.loads(DIAGNOSTICS.read_text())
assert all(d.get('run_id') == manifest['run_id'] for d in diag.values()), 'Diagnostics belong to a different run.'
events = [json.loads(line) for line in (RUN / 'events.jsonl').read_text().splitlines()]
replay = REPLAY.read_text()
assert '62/62 models within 1e-09' in replay
errors = {}
for line in replay.splitlines():
    match = re.match(r'((?:ch|extra_)\S+)\t([\deE.+-]+)\t', line)
    if match: errors[match[1]] = float(match[2])
assert len(errors) == 62 and len(records) == 199
assert "ch14_m14_11" in errors
compared_values = int(re.search(r"\((\d+) values compared\)", replay)[1])
numerical = json.loads((OUT / 'numerical-errors.json').read_text())
assert numerical['run_id'] == manifest['run_id']
assert numerical['runtime_revision'].startswith('6e462c2e')
assert set(numerical['models']) == set(errors)
assert sum(m[g]['count'] for m in numerical['models'].values()
           for g in ('log_density', 'gradient', 'outputs')) == compared_values
inventory = list(csv.DictReader((REPO / 'tests/rethinking/inventory.tsv').open(), delimiter='\t'))
rows = []
for item in inventory:
    name = item['file']; record = records[name]
    row = dict(item, status=record['status'], numeric_error=errors.get(name), diagnostics=diag[name])
    stanc = next((e['elapsed_s'] for e in events if e['phase'] == name + '/stanc-cpp' and e['status'] == 'ok'), None)
    build = record['row'].get('cmdstan_build_s')
    row['cmdstan_compile_s'] = stanc + build if stanc is not None and build is not None else None
    row['failure_reason'] = record['row'].get('note', '')
    row['stanli_compile_s'] = None  # included in each stanli CLI duration
    for engine in ('stanli', 'cmdstan'):
        runs = [e for e in record['sampling'] if e['engine'] == engine]
        assert len(runs) in (0, 4)
        row[engine + '_runs'] = [{k: e[k] for k in ('seed', 'status', 'elapsed_s', 'timeout_s') if k in e} for e in runs]
        values = [e['elapsed_s'] for e in runs if e['status'] == 'ok']
        complete = len(values) == 4
        row[engine + '_median_s'] = statistics.median(values) if complete else None
        row[engine + '_min_s'] = min(values) if complete else None
        row[engine + '_max_s'] = max(values) if complete else None
        row[engine + '_first_fit_s'] = (row[engine + '_median_s'] + (row['cmdstan_compile_s'] if engine == 'cmdstan' else 0)) if complete else None
    row['cli_speedup'] = (row['cmdstan_median_s'] / row['stanli_median_s']
                          if row['cmdstan_median_s'] is not None and row['stanli_median_s'] is not None else None)
    row['first_fit_speedup'] = (row['cmdstan_first_fit_s'] / row['stanli_first_fit_s']
                                if row['cmdstan_first_fit_s'] is not None and row['stanli_first_fit_s'] is not None else None)
    row['numerics'] = numerical['models'][name]
    rows.append(row)
book = [r for r in rows if r['file'].startswith('ch')]
extra = [r for r in rows if not r['file'].startswith('ch')]
assert len(book) == 61 and len(extra) == 1
paired = [r for r in book if r['stanli_median_s'] is not None and r['cmdstan_median_s'] is not None]
faster = sum(r['stanli_first_fit_s'] < r['cmdstan_first_fit_s'] for r in paired)
ratio = statistics.median(r['cmdstan_first_fit_s'] / r['stanli_first_fit_s'] for r in paired)
runtime_faster = sum(r['stanli_median_s'] < r['cmdstan_median_s'] for r in paired)
runtime_ratio = statistics.median(r['cmdstan_median_s'] / r['stanli_median_s'] for r in paired)
summary = dict(book_call_sites=len(book), complete_in_both=len(paired),
               estimated_first_fit_faster=faster, estimated_first_fit_median_ratio=ratio,
               runtime_faster=runtime_faster, runtime_median_ratio=runtime_ratio,
               worst_scaled_numerical_error=max(errors.values()), supplemental_fixtures=len(extra),
               numerically_compared_fixtures=len(errors), preparation_failure=None,
               max_absolute_errors={g: max(r['numerics'][g]['max_absolute'] for r in rows)
                                    for g in ('log_density', 'gradient', 'outputs')})
json_data = dict(summary=summary, run_manifest=manifest, method='Median of four independent single-chain CLI runs; first-fit CmdStan estimate adds matching Stan-to-C++ and C++ build events. No aggregate from surviving seeds.', rows=rows)
(OUT / 'rethinking-results.json').write_text(json.dumps(json_data, indent=2, allow_nan=False) + '\n')
for source, name in [(REPLAY, 'numerical-replay.txt'),
                     (RUN / 'manifest.json', 'benchmark-manifest.json'),
                     (DIAGNOSTICS, 'sampling-diagnostics.json')]:
    if source.resolve() != (OUT / name).resolve():
        shutil.copy2(source, OUT / name)


def speedup(value):
    return '-' if value is None else f'{value:.2f}x'


def absolute(value):
    return '0' if value == 0 else f'{value:.2e}'


fields = ['file', 'model', 'code_box', 'status', 'numeric_error', 'cli_speedup', 'first_fit_speedup', 'stanli_median_s', 'stanli_min_s', 'stanli_max_s',
          'cmdstan_median_s', 'cmdstan_min_s', 'cmdstan_max_s', 'cmdstan_compile_s', 'stanli_first_fit_s', 'cmdstan_first_fit_s',
          'stanli_rhat_max', 'cmdstan_rhat_max',
          'stanli_ess_bulk_min', 'cmdstan_ess_bulk_min', 'stanli_divergences', 'cmdstan_divergences', 'failure_reason']
for group in ('log_density', 'gradient', 'outputs'):
    fields += [group + '_max_absolute', group + '_max_ulp', group + '_count']
with (OUT / 'rethinking-timings.csv').open('w', newline='') as stream:
    writer = csv.DictWriter(stream, fieldnames=fields); writer.writeheader()
    for row in rows:
        flat = {k: row.get(k) for k in fields}
        for group, metrics in row['numerics'].items():
            flat.update({group + '_' + key: value for key, value in metrics.items()})
        for engine in ('stanli', 'cmdstan'):
            for key in ('rhat_max', 'ess_bulk_min', 'divergences'):
                flat[engine + '_' + key] = row['diagnostics']['engines'][engine].get(key)
        writer.writerow(flat)

pin = 'ac1b3b2cda83f3e14096e2d997a6e30ad109eeee'
source_url = f'https://github.com/rmcelreath/rethinking/blob/{pin}/book_code_boxes.txt'
intro = [
    'We compared the unchanged Stan programs and processed data from all 61 ulam() call sites in the second-edition book supplement. Every call site completed four sampling seeds in both engines. A separate hurdle example is also included.',
    f'The median end-to-end sampling speedup was {runtime_ratio:.2f}x (CmdStan time / Stanli time). All {runtime_faster} book call sites had a ratio above 1. Stanli preparation is included; the CmdStan model was already compiled. A value of 2x means Stanli took half as long.',
]
maxima = summary['max_absolute_errors']
numerics = (f'At three fixed parameter vectors per fixture, we compared {compared_values:,} values across 62 fixtures. '
            f'The largest absolute differences were {absolute(maxima["log_density"])} for log density, '
            f'{absolute(maxima["gradient"])} for a gradient component, and {absolute(maxima["outputs"])} for a model output. '
            'The tables give the model-specific maxima.')
method = ('Sampling speedup = median CmdStan CLI time / median Stanli CLI time, using four seeds per engine. '
          'Each run has 1,000 warmup iterations and 1,000 retained draws. Stanli time includes Stan translation, '
          'preparation, warmup, sampling, generated quantities and CSV output. CmdStan time starts from its compiled model. '
          'The ratio describes this fixed sampling budget; it does not compare time to an equal effective sample size.')
error_method = ('Absolute difference = |Stanli value - CmdStan value|. Log density includes the parameter-transform Jacobian. '
                'The gradient column takes the maximum over all unconstrained parameter derivatives; the output column '
                'covers constrained parameters, transformed parameters and generated quantities. Each maximum spans all three '
                'fixed parameter vectors. These are comparisons at identical parameter values, not differences between random posterior draws.')
ulp_method = ('The supporting CSV also reports ULP distance: the number of representable double-precision steps between '
              'the two values. ULP distance depends on the scale of the values and can be large near zero despite a tiny '
              'absolute difference. The report therefore uses absolute differences; zero means the compared values were equal.')
output_method = ('Numerical comparisons use the separately recorded high-precision CmdStan values and the frozen Stanli '
                 'checker from the timing build. The retained per-seed CSVs use the CLI defaults: eight significant digits '
                 'for CmdStan and 17 for Stanli. The supporting CSV lists divergence counts, maximum R-hat and minimum bulk '
                 'ESS for each engine.')
settings = ('Seeds 1-4; target acceptance 0.8; maximum tree depth 10; random initialization. Every model used the same '
            'iteration budget. The book\'s model-specific tuning and chain-count choices were not reproduced. '
            'CmdStan ran first for each seed. Stanli had a limit of three times that CmdStan CLI duration or 900 seconds, '
            'whichever was smaller; CmdStan also had a 900-second limit. All book runs completed. Sampling order was not counterbalanced.')
hardware = ('Apple M3 Ultra, 96 GiB RAM, macOS ARM64; Release build; one engine at a time. Rethinking 2.42, CmdStan 2.39.0 '
            'and posterior 1.7.0. Runtime/compiler revision 6e462c2e; run '+manifest['run_id']+'. '
            'The toolchain was already installed. The manifest records executable, dependency and input identities.')
scope = ('All 61 ulam() call sites in the chapters 4-16 supplement are listed, representing 58 distinct Stan/data pairs. '
         'Repeated fits remain listed. The separate hurdle example brings the total to 62 fixtures. Chapters 4-8 and 10 '
         'have no ulam() calls in this supplement. quap(), direct stan() calls, exercises and lecture-only models are outside this inventory.')
examples = [
 ('ch11_m11_4', 'Chimpanzee logistic regression'),
 ('ch12_m12_5', 'Ordered response with predictors'),
 ('ch12_m12_6', 'Monotonic education effect'),
 ('ch13_m13_4nc', 'Non-centered multilevel model'),
 ('ch14_m14_8', 'Gaussian-process model'),
 ('ch14_m14_11', 'Phylogenetic GP, 151 species'),
 ('ch15_m15_2', 'Measurement-error model'),
 ('ch16_m16_1', 'Nonlinear height-weight model'),
]
lookup = {r['file']: r for r in rows}
first_fit = (f'Including the separately measured CmdStan translation and C++ build gives a median estimated first-fit '
             f'speedup of {ratio:.2f}x across the 61 book call sites. These estimates add measured stages; they are not '
             'directly timed combined runs. Installation is excluded. The main tables use the already-compiled CmdStan comparison.')
appendix_note = ('Speedup: CmdStan/Stanli median complete CLI time. Error columns: maximum absolute difference across '
                 'three parameter vectors. The full-precision values, per-seed times, ULP distances and sampler measurements '
                 'are retained in the accompanying CSV and JSON files.')


def numeric_cells(row):
    return [absolute(row['numerics'][g]['max_absolute']) for g in ('log_density', 'gradient', 'outputs')]


md = ['# Rethinking models: speedup and numerical differences', '',
      'Prepared for Richard McElreath | 16 September 2026', '', *[x for text in intro for x in (text, '')],
      '## Numerical differences', '', numerics, '', '## How to read the results', '', method, '', error_method, '', ulp_method, '',
      '## Measurement conditions', '', hardware, '', settings, '', output_method, '',
      '## Estimated first-fit speedup', '', first_fit, '',
      '## Full speedup and numerics appendix', '', appendix_note, '',
      '| Book model | R code | Sampling speedup | Log density max abs. difference | Gradient max abs. difference | Output max abs. difference |',
      '| --- | --- | ---: | ---: | ---: | ---: |']
for row in book:
    md.append('| ' + ' | '.join([row['model'], row['code_box'], speedup(row['cli_speedup']), *numeric_cells(row)]) + ' |')
r = extra[0]
md += ['', '## Supplemental fixture (not a book model)', '',
       f'Hurdle Poisson: {speedup(r["cli_speedup"])} sampling speedup; maximum absolute differences '
       f'{numeric_cells(r)[0]} (log density), {numeric_cells(r)[1]} (gradient), {numeric_cells(r)[2]} (output).', '',
       '## Scope and reproducibility', '', scope, '', '[Pinned book supplement](' + source_url + '). '
       'The generator preserves the Stan text and processed data. Simulation seeds and data licensing are recorded in '
       'tests/rethinking/PROVENANCE.md. Both engines receive identical input bytes.', '',
       'See `benchmark-manifest.json`, `rethinking-results.json`, `rethinking-timings.csv`, `numerical-errors.json` and '
       '`numerical-values.json.gz`. The last file retains every compared value from both engines. Raw sampling CSVs and '
       'command logs remain in the original run directory.', '']
(OUT / 'rethinking-report.md').write_text('\n'.join(md).rstrip() + '\n')

# PDF: summary, methods, then complete speedup and numerical results.
styles = getSampleStyleSheet()
styles.add(ParagraphStyle(name='ReportTitle', fontName='Helvetica-Bold', fontSize=21, leading=25, textColor=colors.HexColor('#173d47'), spaceAfter=8))
styles.add(ParagraphStyle(name='Deck', fontName='Helvetica', fontSize=10, leading=14, textColor=colors.HexColor('#566570'), spaceAfter=15))
styles.add(ParagraphStyle(name='BodyReport', fontName='Helvetica', fontSize=9.5, leading=13, spaceAfter=9))
styles.add(ParagraphStyle(name='SmallReport', fontName='Helvetica', fontSize=8, leading=10.5, spaceAfter=6))
styles.add(ParagraphStyle(name='TableReport', fontName='Helvetica', fontSize=8, leading=10))
styles.add(ParagraphStyle(name='TableHead', fontName='Helvetica-Bold', fontSize=8, leading=10, textColor=colors.white))
styles['Heading2'].fontName = 'Helvetica-Bold'; styles['Heading2'].fontSize = 12
styles['Heading2'].textColor = colors.HexColor('#173d47'); styles['Heading2'].spaceAfter = 8


def para(text, style='BodyReport'):
    return Paragraph(text.replace('10^-13', '10<super>-13</super>').replace('10^-9', '10<super>-9</super>'), styles[style])


def table(data, widths, head=True):
    cells = [[para(escape(str(v)), 'TableHead' if head and i == 0 else 'TableReport') for v in row] for i, row in enumerate(data)]
    result = Table(cells, colWidths=widths, repeatRows=1 if head else 0, hAlign='LEFT')
    spec = [('VALIGN', (0,0), (-1,-1), 'TOP'), ('LEFTPADDING',(0,0),(-1,-1),6), ('RIGHTPADDING',(0,0),(-1,-1),6),
            ('TOPPADDING',(0,0),(-1,-1),4), ('BOTTOMPADDING',(0,0),(-1,-1),4),
            ('LINEBELOW',(0,-1),(-1,-1),0.4,colors.HexColor('#ccd5d8'))]
    if head:
        spec += [('BACKGROUND',(0,0),(-1,0),colors.HexColor('#234e59')),
                 ('ROWBACKGROUNDS',(0,1),(-1,-1),[colors.white,colors.HexColor('#f0f4f5')])]
    result.setStyle(TableStyle(spec)); return result


def footer(canvas, doc):
    canvas.setStrokeColor(colors.HexColor('#ccd5d8')); canvas.line(45,36,A4[0]-45,36)
    canvas.setFont('Helvetica',7.5); canvas.setFillColor(colors.HexColor('#566570'))
    canvas.drawString(45,24,'Stanli | Statistical Rethinking, second edition | 16 September 2026')
    canvas.drawRightString(A4[0]-45,24,str(doc.page))


story = [para('Rethinking models', 'ReportTitle'),
         para('Speedup and numerical differences<br/>Prepared for Richard McElreath | 16 September 2026', 'Deck')]
for text in intro: story.append(para(text))
story += [para('Numerical differences', 'Heading2'), para(numerics),
          para('Examples', 'Heading2'), para('Speedup is CmdStan/Stanli complete CLI time. Numerical columns show maximum absolute differences.', 'SmallReport')]
data = [['Book model / example', 'Sampling speedup', 'Log density difference', 'Gradient difference']]
for name, label in examples:
    r = lookup[name]
    data.append([r['model'] + ' - ' + label, speedup(r['cli_speedup']), *numeric_cells(r)[:2]])
story += [table(data, [253, 76, 88, 88]), Spacer(1, 10),
          para('Estimated first-fit speedup', 'Heading2'), para(first_fit),
          para('The appendix lists every book call site and the separate hurdle fixture. Sampler measurements and original times remain in the supporting data.', 'SmallReport')]
story += [PageBreak(), para('How to read the results', 'ReportTitle'),
          para('Sampling speedup', 'Heading2'), para(method),
          para('Absolute numerical differences', 'Heading2'), para(error_method), para(ulp_method),
          para('Measurement conditions', 'Heading2'), para(hardware), para(settings),
          para('Supporting measurements', 'Heading2'), para(output_method),
          para('Scope and sources', 'Heading2'), para(scope),
          para(f'<link href="{source_url}" color="#234e59">Pinned second-edition code supplement (rethinking 2.42)</link><br/>'
               'The accompanying CSV and JSON retain every speedup, numerical comparison, sampler measurement and build/input identity. '
               'numerical-values.json.gz contains the paired values from both engines.', 'SmallReport')]
for page_index, chunk in enumerate((book[:31], book[31:])):
    story += [PageBreak(), para('Full results' + (' (continued)' if page_index else ''), 'ReportTitle'),
              para(appendix_note, 'SmallReport')]
    data = [['Book model', 'R code', 'Sampling speedup', 'Log density max abs. diff.', 'Gradient max abs. diff.', 'Output max abs. diff.']]
    for r in chunk:
        data.append([r['model'], r['code_box'], speedup(r['cli_speedup']), *numeric_cells(r)])
    story.append(table(data, [76, 49, 89, 97, 97, 97]))
    if page_index:
        r = extra[0]
        story += [Spacer(1, 8), para('Supplemental fixture (not a book model)', 'Heading2'),
                  para(f'Hurdle Poisson: {speedup(r["cli_speedup"])} speedup. Maximum absolute differences: '
                       f'{numeric_cells(r)[0]} (log density), {numeric_cells(r)[1]} (gradient), {numeric_cells(r)[2]} (output).', 'SmallReport')]
pdf_path = OUT.parent / 'pdf' / 'rethinking-report.pdf'
pdf_path.parent.mkdir(parents=True, exist_ok=True)
SimpleDocTemplate(str(pdf_path), pagesize=A4, rightMargin=45, leftMargin=45,
                  topMargin=42, bottomMargin=48, title='Rethinking models: speedup and numerical differences',
                  author='Stanli evaluation', subject='Complete Rethinking corpus numerical and sampling comparison').build(story, onFirstPage=footer, onLaterPages=footer)
print(json.dumps(summary,indent=2))
print(pdf_path)
