#!/usr/bin/env python3
"""Build the McElreath report from a completed, immutable corpus run.

Usage: report_rethinking.py RUN_DIRECTORY DIAGNOSTICS_JSON REPLAY_LOG OUTPUT_DIR
Requires reportlab. No benchmarks run here. Retain the run directory as evidence.
"""
import csv
import hashlib
import json
import math
import pathlib
import re
import shutil
import statistics
import sys
from xml.sax.saxutils import escape
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.pagesizes import A4
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak

REPO = pathlib.Path(__file__).resolve().parent.parent
RUN, DIAGNOSTICS, REPLAY, OUT = map(pathlib.Path, sys.argv[1:])
OUT.mkdir(parents=True, exist_ok=True)
manifest = json.loads((RUN / 'manifest.json').read_text())
assert manifest['run_id'] == 'fae5494c296cd547', 'This report narrative is tied to its audited run; review it before using other results.'
assert manifest['identity']['config']['cmdstan_runtime_multiple'] == 3
assert manifest['identity']['config']['seeds'] == [1, 2, 3, 4]
assert manifest['identity']['config']['iter_sampling'] == 1000
assert manifest['identity']['config']['iter_warmup'] == 1000
records = {p.stem.removesuffix('.result'): json.loads(p.read_text()) for p in RUN.glob('*.result.json')}
diag = json.loads(DIAGNOSTICS.read_text())
assert all(d.get('run_id') == manifest['run_id'] for d in diag.values()), 'Diagnostics belong to a different run.'
events = [json.loads(line) for line in (RUN / 'events.jsonl').read_text().splitlines()]
replay = REPLAY.read_text()
assert '61/61 models within 1e-09' in replay
errors = {}
for line in replay.splitlines():
    match = re.match(r'((?:ch|extra_)\S+)\t([\deE.+-]+)\t', line)
    if match: errors[match[1]] = float(match[2])
assert len(errors) == 61 and len(records) == 199
assert "ch14_m14_11" not in errors
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
    rows.append(row)
book = [r for r in rows if r['file'].startswith('ch')]
extra = [r for r in rows if not r['file'].startswith('ch')]
assert len(book) == 61 and len(extra) == 1
paired = [r for r in book if r['stanli_median_s'] is not None and r['cmdstan_median_s'] is not None]
clear = [r for r in book if all(r['diagnostics']['engines'][e]['status'] == 'complete' and not r['diagnostics']['engines'][e]['screening_flag'] for e in ('stanli', 'cmdstan'))]
faster = sum(r['stanli_first_fit_s'] < r['cmdstan_first_fit_s'] for r in paired)
ratio = statistics.median(r['cmdstan_first_fit_s'] / r['stanli_first_fit_s'] for r in paired)
summary = dict(book_call_sites=len(book), complete_in_both=len(paired), diagnostic_screen_clear_in_both=len(clear),
               estimated_first_fit_faster=faster, estimated_first_fit_median_ratio=ratio,
               worst_scaled_numerical_error=max(errors.values()), supplemental_fixtures=len(extra),
               numerically_verified_fixtures=len(errors), preparation_failure='ch14_m14_11')
json_data = dict(summary=summary, run_manifest=manifest, method='Median of four independent single-chain CLI runs; first-fit CmdStan estimate adds matching Stan-to-C++ and C++ build events. No aggregate from surviving seeds.', rows=rows)
(OUT / 'rethinking-results.json').write_text(json.dumps(json_data, indent=2, allow_nan=False) + '\n')
shutil.copy2(REPLAY, OUT / 'numerical-replay.txt')
shutil.copy2(RUN / 'manifest.json', OUT / 'benchmark-manifest.json')
shutil.copy2(DIAGNOSTICS, OUT / 'sampling-diagnostics.json')


def number(v):
    if v is None: return '-'
    return f'{v:.3f}' if v < 1 else f'{v:.2f}' if v < 10 else f'{v:.1f}' if v < 100 else f'{v:.0f}'


def timing(r, engine):
    value = r[engine + '_median_s']
    if value is None:
        runs = r[engine + '_runs']
        if not runs:
            return 'not measured'
        capped = sum(e['status'] == 'timeout' for e in runs)
        return f'capped ({capped}/4)' if capped else 'not completed'
    return f"{number(value)} [{number(r[engine + '_min_s'])}-{number(r[engine + '_max_s'])}]"


def flag(r, engine):
    d = r['diagnostics']['engines'][engine]
    if d['status'] != 'complete': return 'incomplete'
    return 'review' if d['screening_flag'] else 'clear'


fields = ['file', 'model', 'code_box', 'status', 'numeric_error', 'stanli_median_s', 'stanli_min_s', 'stanli_max_s',
          'cmdstan_median_s', 'cmdstan_min_s', 'cmdstan_max_s', 'cmdstan_compile_s', 'stanli_first_fit_s', 'cmdstan_first_fit_s',
          'stanli_diagnostic_screen', 'cmdstan_diagnostic_screen', 'stanli_rhat_max', 'cmdstan_rhat_max',
          'stanli_ess_bulk_min', 'cmdstan_ess_bulk_min', 'stanli_divergences', 'cmdstan_divergences', 'failure_reason']
with (OUT / 'rethinking-timings.csv').open('w', newline='') as stream:
    writer = csv.DictWriter(stream, fieldnames=fields); writer.writeheader()
    for row in rows:
        flat = {k: row.get(k) for k in fields}
        for engine in ('stanli', 'cmdstan'):
            flat[engine + '_diagnostic_screen'] = flag(row, engine)
            for key in ('rhat_max', 'ess_bulk_min', 'divergences'):
                flat[engine + '_' + key] = row['diagnostics']['engines'][engine].get(key)
        writer.writerow(flat)

pin = 'ac1b3b2cda83f3e14096e2d997a6e30ad109eeee'
source_url = f'https://github.com/rmcelreath/rethinking/blob/{pin}/book_code_boxes.txt'
stan_url = 'https://mc-stan.org/docs/2_39/cmdstan-guide/diagnose_utility.html'
incomplete = [r for r in book if r not in paired]
slower = [r['model'] for r in paired if r['stanli_first_fit_s'] >= r['cmdstan_first_fit_s']]
intro = [
    f"Stanli runs Stan programs without a separate C++ compilation for each model. We evaluated the unchanged Stan programs and data generated by all 61 ulam() call sites in the second-edition code supplement. On the current build, 60 book call sites passed the numerical replay; m14.11 could not finish model preparation within five minutes. {len(paired)} book call sites completed all four timed seeds in both engines.",
    f"Including model compilation, the estimated time to a first fit was shorter with stanli for {faster} of the {len(paired)} book call sites that completed in both engines. Much of this advantage comes from avoiding a separate C++ compilation. Full results include every incomplete case; completion and diagnostic flags must be considered alongside elapsed time.",
]
numerics = f"At three fixed parameter vectors per fixture, we compared the log posterior, every derivative, and constrained/model-output values. Sixty-one of 62 fixtures passed, including the separate hurdle example: 32,316 values were compared, with a largest scaled discrepancy of {max(errors.values()):.2e}. The tolerance was 10^-9. The remaining fixture, m14.11, timed out during preparation, before the comparison (issue #372). Pointwise agreement is not a proof of equality everywhere."
quality = f'{len(clear)} of the 61 book call sites completed and met the diagnostic screen in both engines. The others had diagnostic flags or did not complete. The book includes deliberately difficult models, so elapsed time alone is not evidence of reliable inference. We did not retune individual models to make diagnostics pass.'
method = 'Each engine used four independent seeds, with 1,000 warmup iterations and 1,000 retained draws per seed. Timings below describe one chain, not a parallel four-chain fit. Each stanli CLI run includes Stan translation, model preparation, warmup, sampling, generated quantities and CSV output. CmdStan CLI times start from its compiled model; compilation is shown separately.'
output_method = 'CSV precision follows the CLI defaults: eight significant digits for CmdStan and 17 for stanli. The numerical oracle uses separate high-precision values; sampling diagnostics use these retained CSVs.'
examples = [
 ('ch11_m11_4', 'Chimpanzee logistic regression'),
 ('ch12_m12_4', 'Ordered response, cutpoints only'),
 ('ch12_m12_5', 'Ordered response with predictors'),
 ('ch12_m12_6', 'Monotonic education effect'),
 ('ch13_m13_4nc', 'Non-centered multilevel model'),
 ('ch14_m14_8', 'Gaussian-process model'),
 ('ch15_m15_2', 'Measurement-error model'),
 ('ch16_m16_1', 'Nonlinear height-weight model'),
]
lookup = {r['file']: r for r in rows}
case_notes = []
for r in incomplete:
    if not r['stanli_runs'] and not r['cmdstan_runs']:
        case_notes.append(r['model'] + ': no sampling comparison; preparation or the preceding numerical/timing gate did not finish. See the retained failure log.')
    else:
        statuses = []
        for engine in ('stanli', 'cmdstan'):
            for run in r[engine + '_runs']:
                if run['status'] != 'ok':
                    limit = f" ({run['timeout_s']:.3g}-second limit)" if 'timeout_s' in run else ''
                    statuses.append(f"{engine} seed {run['seed']}: {run['status']}{limit}")
        case_notes.append(r['model'] + ': ' + '; '.join(statuses) + '.')
incomplete_text = ' '.join(case_notes) or 'All four seeds completed for every book call site.'
clear_counts = {e: sum(r['diagnostics']['engines'][e]['status'] == 'complete' and
                        not r['diagnostics']['engines'][e]['screening_flag'] for r in book)
                for e in ('stanli', 'cmdstan')}
complete_counts = {e: sum(r['diagnostics']['engines'][e]['status'] == 'complete' for r in book)
                   for e in ('stanli', 'cmdstan')}
screen_text = (f"Under this screen, {clear_counts['stanli']} of {complete_counts['stanli']} complete stanli book fits and "
               f"{clear_counts['cmdstan']} of {complete_counts['cmdstan']} CmdStan book fits were clear; {len(clear)} were clear in both. "
               "Several flagged cases are intentionally difficult teaching examples. Runtime is the cost of a fixed sampling budget, not time to a specified inferential accuracy.")
# Editable report accompanies the PDF; all numeric cells use the same rows.
md = ['# Rethinking models: numerical agreement and time to draws', '', 'Prepared for Richard McElreath | 15 September 2026', '']
md += [intro[0], '', intro[1]] + ['', '## Numerical agreement', '', numerics, '', '## Sampling quality', '', quality, '', '## Measurement', '', method, '', output_method, '']
md += ['Apple M3 Ultra, 96 GiB RAM, macOS ARM64; Release build; one engine at a time. Rethinking 2.42, CmdStan 2.39.0, posterior 1.7.0.', '',
       'The stanli limit for each seed was the smaller of 3x the matching CmdStan CLI time and 900 seconds. CmdStan ran first to establish that limit. Sampling order was therefore not counterbalanced. CmdStan itself had a 900-second limit. A capped seed prevents an aggregate timing for that engine; the successful seeds are not averaged on their own.', '',
       'Common sampler settings were target acceptance 0.8, maximum tree depth 10 and random initialization. Model-specific chain counts and tuning choices in the book were not reproduced. C++ compilation was measured once per model, with the toolchain already installed; no cold-cache or installation-time claim is made.', '',
       'Diagnostic screen: no post-warmup divergences or maximum-tree-depth hits; every nonconstant variable in the parameters block has finite R-hat <= 1.01 and bulk ESS >= 400. Fixed matrix entries are verified and excluded. A clear screen is not a general validation of the posterior. See [Stan diagnostics guidance](' + stan_url + ').', '',
       'The current build is based on main 2ae6c1d0, with runtime/compiler sources unchanged in the PR. The full run manifest identifies the branch and dependency hashes. Earlier numbers from be0a0c8d used a different runtime and timeout-polling method and are superseded; they are not mixed into this table.', '',
       '## Full timing appendix', '', 'Seconds: median [minimum-maximum] across four seeds. CmdStan compilation includes the recorded Stan-to-C++ translation and C++ build. For a first-fit estimate add compilation to CmdStan runtime; stanli preparation is already included. These are sums of measured stages, not a directly timed combined invocation.', '',
       '| Book model | R code | stanli runtime | CmdStan runtime | CmdStan compile | Screen S/C |',
       '| --- | --- | ---: | ---: | ---: | --- |']
for row in rows:
    md.append(f"| {row['model']} | {row['code_box']} | {timing(row, 'stanli')} | {timing(row, 'cmdstan')} | {number(row['cmdstan_compile_s'])} | {flag(row,'stanli')}/{flag(row,'cmdstan')} |")
md += ['', '## Incomplete cases', '', incomplete_text, '', 'Issue [#372](https://github.com/seantalts/stanli/issues/372) records the m14.11 preparation failure. It remains in the inventory and its references are preserved. Issue [#373](https://github.com/seantalts/stanli/issues/373) tracks the ordered-regression performance gap.', '', '## Scope and reproducibility', '',
       'The 61 book call sites include repeated fits and correspond to 58 distinct Stan/data pairs. The supplemental hurdle model makes 62 fixtures and 59 distinct pairs. Chapters 4-8 and 10 have no ulam() calls in this supplement. quap(), direct stan() calls, exercises and lecture-only models are outside this corpus.', '',
       '[Pinned book supplement](' + source_url + '). The generator preserves the Stan text and processed data; simulation seeds and data licensing are recorded in tests/rethinking/PROVENANCE.md. Both engines receive exactly the same input bytes.', '',
       f"Run ID: `{manifest['run_id']}`. See `benchmark-manifest.json`, `rethinking-results.json`, `sampling-diagnostics.json`, `numerical-replay.txt` and `rethinking-timings.csv` for identities, numeric results and diagnostics. Raw per-seed CSVs and command logs are retained in the original run directory.", '']
(OUT / 'rethinking-report.md').write_text('\n'.join(md).rstrip() + '\n')

# PDF: a short summary, methods/diagnostics, then the complete timing appendix.
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
    canvas.drawString(45,24,'Stanli | Statistical Rethinking, second edition | 15 September 2026')
    canvas.drawRightString(A4[0]-45,24,str(doc.page))


story = [para('Rethinking models', 'ReportTitle'), para('Numerical agreement and time to draws<br/>Prepared for Richard McElreath | 15 September 2026', 'Deck')]
for text in intro: story.append(para(text))
story += [para('Numerical agreement', 'Heading2'), para(numerics), para('Examples: estimated time to a first fit', 'Heading2'),
          para('Seconds for one chain. CmdStan includes its recorded model-compilation stages; stanli preparation is already included. Screen S/C means stanli/CmdStan; review flags a diagnostic. See the appendix for all models and the four-seed ranges.', 'SmallReport')]
data = [['Book model / example', 'stanli', 'CmdStan', 'Screen S/C']]
for name,label in examples:
    r=lookup[name]
    data.append([r['model']+' - '+label, number(r['stanli_first_fit_s']), number(r['cmdstan_first_fit_s']), flag(r,'stanli')+'/'+flag(r,'cmdstan')])
story += [table(data,[260,65,65,115]), Spacer(1,10), para('Sampling quality matters', 'Heading2'), para(quality)]
story += [PageBreak(), para('How to read the results', 'ReportTitle'), para(method), para(output_method, 'SmallReport'),
          para('Hardware and software', 'Heading2'), para('Apple M3 Ultra, 96 GiB RAM, macOS ARM64; Release build; one engine at a time. The toolchain was already installed. Rethinking 2.42, CmdStan 2.39.0 and posterior 1.7.0 were used. Runtime/compiler sources match main 2ae6c1d0; the manifest records all build identities. These are desktop measurements, not predicted laptop timings.'),
          para('The scaled error is abs(a-b) / max(1, abs(a), abs(b)). Log-posterior and derivative comparisons use the same unconstrained parameter values and include the transformation Jacobian.', 'SmallReport'), para('Fixed settings and stopping rule', 'Heading2'), para('Seeds 1-4; target acceptance 0.8; maximum tree depth 10; random initialization. Every model used the same iteration budget. The book\'s model-specific tuning and chain-count choices were not reproduced.'),
          para('For each seed, CmdStan ran first. Stanli was stopped at the smaller of three times that CmdStan CLI runtime and 900 seconds. CmdStan also had a 900-second ceiling. C++ compilation did not count toward the relative cap. This reference-first order was required by the cap and was not counterbalanced.'),
          para('Incomplete cases', 'Heading2'), para(incomplete_text),
          para('Diagnostic screen', 'Heading2'), para('We checked the four runs as four chains. A clear screen requires no post-warmup divergences or maximum-tree-depth hits, finite R-hat no greater than 1.01, and bulk effective sample size of at least 400 for every nonconstant variable in the parameters block. Fixed correlation-matrix entries were verified and omitted. A flag calls for review; a clear screen is not proof that inference is reliable.'),
          para(screen_text),
          para('Scope and sources', 'Heading2'), para('All 61 ulam() call sites in chapters 4-16 are included, representing 58 distinct Stan/data pairs. Repeated fits remain listed. The supplemental hurdle fixture is separate. quap(), direct stan() calls, exercises and lecture-only examples are outside this inventory.'),
          para(f'<link href="{source_url}" color="#234e59">Pinned second-edition code supplement (rethinking 2.42)</link><br/><link href="{stan_url}" color="#234e59">Stan: convergence and efficiency diagnostics</link><br/><link href="https://github.com/seantalts/stanli/issues/372" color="#234e59">Preparation timeout (#372)</link> | <link href="https://github.com/seantalts/stanli/issues/373" color="#234e59">Ordered-regression timing (#373)</link>', 'SmallReport'),
          para('The accompanying CSV and JSON files contain every timing, diagnostic result and build/input identity. Run ID: '+manifest['run_id']+'. Numerical replay compared 32,316 values across 61 fixtures, including model outputs. The m14.11 preparation failure is tracked in issue #372. Inputs were identical across engines; generated Stan programs were not rewritten.', 'SmallReport')]
for page_index, chunk in enumerate((book[:31],book[31:])):
    story += [PageBreak(), para('Full timing appendix' + (' (continued)' if page_index else ''), 'ReportTitle'),
              para('Seconds: median [minimum-maximum] across four seeds. One chain has 1,000 warmup iterations and 1,000 retained draws. S/C = stanli/CmdStan diagnostic screen.', 'SmallReport')]
    data = [['Book model', 'R code', 'stanli runtime', 'CmdStan runtime', 'CmdStan compile', 'Screen S/C']]
    for r in chunk:
        data.append([r['model'],r['code_box'],timing(r,'stanli'),timing(r,'cmdstan'),number(r['cmdstan_compile_s']),flag(r,'stanli')+'/'+flag(r,'cmdstan')])
    story.append(table(data,[67,43,108,108,68,111]))
    story += [Spacer(1,8),para('To estimate a first fit, add CmdStan compilation to its runtime; stanli preparation is already included. Compilation was measured once, including Stan-to-C++ translation and the C++ build. First-fit totals are sums of measured stages. Four-seed ranges describe run-to-run variation, not confidence intervals.', 'SmallReport')]
    if page_index:
        r=extra[0]
        story += [para('Supplemental fixture (not a book model)', 'Heading2'),
                  para('Hurdle Poisson: stanli '+timing(r,'stanli')+' s; CmdStan '+timing(r,'cmdstan')+' s; CmdStan compilation '+number(r['cmdstan_compile_s'])+' s. Screen: '+flag(r,'stanli')+'/'+flag(r,'cmdstan')+'.', 'SmallReport')]
pdf_path = OUT.parent / 'pdf' / 'rethinking-report.pdf'
pdf_path.parent.mkdir(parents=True, exist_ok=True)
SimpleDocTemplate(str(pdf_path), pagesize=A4, rightMargin=45, leftMargin=45,
                  topMargin=42, bottomMargin=48, title='Rethinking models: numerical agreement and time to draws',
                  author='Stanli evaluation', subject='Complete Rethinking corpus numerical and sampling comparison').build(story, onFirstPage=footer, onLaterPages=footer)
print(json.dumps(summary,indent=2))
print(pdf_path)
