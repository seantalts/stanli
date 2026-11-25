"""OUT/report.md and OUT/scaling.md from the records run.py collects."""
from __future__ import annotations

import json
import re
from collections import Counter, defaultdict

from measure import normalized_text, ratio_text
from run import FLAG_RATIO, LOAD_WARNING, PREP_CLIFF_RATIO, PREP_CLIFF_S

OUTCOMES = ("invalid", "same_program", "fine", "flagged_cmdstan_equal",
            "cliff", "failure", "flagged", "valid", "error")


def known_reason(kind, rec, known):
    text = json.dumps(rec, sort_keys=True)
    for k in known:
        if k["kind"] == kind and re.search(k["match"], text):
            return k["reason"]
    return None


def mib(n):
    return "-" if n is None else f"{n / 2**20:.2f} MiB"


def arrow(a, b):
    return f"{a} -> {b}"


def load_lines(meta):
    peak = max(meta.get("load_1min") or [0])
    lines = [f"Peak 1-minute load during the run: {peak:.1f}."]
    if peak > LOAD_WARNING:
        lines.append(f"**Warning: the load passed {LOAD_WARNING:.0f}, so the "
                     "timings are noisy. Rerun flagged entries on a quiet "
                     "machine.**")
    return lines


def ops_of(rec, side):
    s = rec.get(f"{side}_program") or {}
    return s.get("ops"), s.get("wa_ops")


def ops_text(rec):
    """Final log_prob ops (the program that runs), then in parentheses the
    ops before the passes (which explain preparation time)."""
    b, v = rec.get("base_program") or {}, rec.get("variant_program") or {}
    if not b and not v:
        return "-"
    return (f"{b.get('ops')} -> {v.get('ops')} "
            f"({b.get('lowered_ops')} -> {v.get('lowered_ops')})")


def prep_text(rec):
    b, v = rec.get("base_program") or {}, rec.get("variant_program") or {}
    if b.get("prep_s") is None or v.get("prep_s") is None:
        return "-"
    return f"{b['prep_s'] * 1e3:.0f} -> {v['prep_s'] * 1e3:.0f} ms"


def note_of(r):
    parts = [r["reason"]] if r.get("reason") else []
    if r.get("near_equivalent"):
        parts.append("near_equivalent (relative "
                     f"{r['near_equivalent']['largest_relative_difference']:.1e})")
    if r.get("prep_cliff"):
        parts.append("prep_cliff")
    if r.get("differs") and r["outcome"] != "same_program":
        parts.append("differs: " + ", ".join(r["differs"]))
    parts += r.get("new_declines", [])
    if lowering_change(r):
        parts.append(lowering_change(r))
    return "; ".join(parts)


def unexplained(rec):
    (b, _), (v, _) = ops_of(rec, "base"), ops_of(rec, "variant")
    grew = b and v and v > 1.1 * b
    return (not rec.get("new_declines") and not grew
            and not lowering_change(rec))


def lowering_change(rec):
    """How the region opcodes differ from the base's, or ""."""
    parts = []
    for key, graph in (("lowering", "log_prob"), ("wa_lowering", "write_array")):
        a = set((rec.get("base_program") or {}).get(key) or {})
        b = set((rec.get("variant_program") or {}).get(key) or {})
        parts += [f"{graph} +{x}" for x in sorted(b - a)]
        parts += [f"{graph} -{x}" for x in sorted(a - b)]
    return ", ".join(parts)


def variant_report(records, meta, known):
    out = ["# Perturbation report", "",
           f"{len(records)} variants, stage `{meta['stage']}`, "
           f"{meta['seconds']:.0f} s.", *load_lines(meta), ""]
    cliffs = sorted((r for r in records if r["outcome"] == "cliff"),
                    key=lambda r: -r["cliff"])
    for title, rows in (
            ("Cliffs", [r for r in cliffs if not unexplained(r)]),
            ("Unexplained cliffs (no new decline reason, no op growth)",
             [r for r in cliffs if unexplained(r)])):
        out += [f"## {title}", ""]
        if not rows:
            out += ["None.", ""]
            continue
        out += ["| variant | class | dose | log_prob ops: final (before passes) "
                "| write_array ops | preparation "
                "| executor | stanli | CmdStan | cliff | stanli vs CmdStan "
                "| known |",
                "|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in rows:
            (bo, bw), (vo, vw) = ops_of(r, "base"), ops_of(r, "variant")
            out.append(
                f"| {r['seed']}/{r['variant']} | {r['class']} | {r['dose']} "
                f"| {ops_text(r)} | {arrow(bw, vw)} | {prep_text(r)} "
                f"| {arrow(mib(r['base_program'].get('executor_bytes')), mib(r['variant_program'].get('executor_bytes')))} "
                f"| {ratio_text(r['confirm']['ratio'])} "
                f"| {ratio_text(r['cmdstan']['ratio'])} "
                f"| {normalized_text(r['cliff'])} | {ratio_text(r['stanli_vs_cmdstan'])} "
                f"| {known_reason('cliff', r, known) or 'new'} |")
        out.append("")
        for r in rows:
            out.append(f"- **{r['seed']}/{r['variant']}**: "
                       f"{(r['meta'] or {}).get('description', '')}")
            for d in r.get("new_declines", []):
                out.append(f"  - new decline: {d}")
            if lowering_change(r):
                out.append(f"  - lowering: {lowering_change(r)}")
            if r.get("copies"):
                c = r["copies"]
                out.append(f"  - {c['n']} concurrent copies: base "
                           f"{c['base_ns']:.0f} ns, variant "
                           f"{c['variant_ns']:.0f} ns per gradient "
                           f"({ratio_text(c['variant_ns'] / c['base_ns'])})")
        out.append("")
    out += ["## Outcomes per class", "",
            "| class | " + " | ".join(OUTCOMES[:6]) + " | other |",
            "|---|" + "---:|" * 7]
    by_class = defaultdict(Counter)
    for r in records:
        by_class[r["class"] or "?"][r["outcome"]] += 1
    for cls, c in sorted(by_class.items()):
        other = sum(n for k, n in c.items() if k not in OUTCOMES[:6])
        out.append(f"| {cls} | " + " | ".join(str(c[k]) for k in OUTCOMES[:6])
                   + f" | {other} |")
    out += ["", "`fine` means the program changed (or only stage 1-2 ran) "
            "and the gradient did not slow by the threshold; "
            "`flagged_cmdstan_equal` means stanli slowed by "
            f"{FLAG_RATIO}x or more but CmdStan slowed about as much.", "",
            "## Preparation cliffs", "",
            f"Equivalent variants whose preparation takes {PREP_CLIFF_RATIO:g}x "
            f"the base's and {PREP_CLIFF_S * 1e3:.0f} ms more.", ""]
    preps = [r for r in records if r.get("prep_cliff")]
    out += [f"- {r['seed']}/{r['variant']}: preparation {prep_text(r)}, "
            f"log_prob ops {ops_text(r)}" for r in preps] or ["None."]
    out += ["", "## Every variant", "",
            "log_prob ops are the final count, the program that runs, then in "
            "parentheses the count before the passes, which explains "
            "preparation time.", "",
            "| variant | class | dose | outcome | log_prob ops | write_array "
            "ops | preparation | stanli | CmdStan | cliff | note |",
            "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in records:
        (bo, bw), (vo, vw) = ops_of(r, "base"), ops_of(r, "variant")
        stanli = (r.get("confirm") or r.get("timing") or {}).get("ratio")
        cmd = (r.get("cmdstan") or {}).get("ratio")
        cliff = normalized_text(r.get("cliff"))
        note = note_of(r)
        out.append(f"| {r['seed']}/{r['variant']} | {r['class']} | {r['dose']} "
                   f"| {r['outcome']} | {ops_text(r)} | {arrow(bw, vw)} "
                   f"| {prep_text(r)} "
                   f"| {ratio_text(stanli)} | {ratio_text(cmd)} | {cliff} "
                   f"| {note[:160].replace('|', '/')} |")
    return "\n".join(out) + "\n"


FINDINGS = ("ops_grow", "prep_superlinear", "per_element_jump",
            "program_changes_shape", "failure")


def scaling_report(models, meta, known):
    out = ["# Scaling report", "",
           f"{len(models)} sources, {meta['seconds']:.0f} s.",
           *load_lines(meta), ""]
    scalable = [x for x in models if x.get("ladders")]
    out += [f"{len(scalable)} of {len(models)} sources have at least one "
            "size variable that scale.py can scale.", ""]
    reasons = Counter()
    for x in models:
        for var, why in (x.get("refused") or {}).items():
            reasons[re.sub(r"`[^`]*`|'[^']*'", "_", why)[:120]] += 1
        if x.get("error"):
            reasons[x["error"][:120]] += 1
        elif not x.get("ladders") and not x.get("refused"):
            reasons["the data block has no int size variable"] += 1
    if reasons:
        out += ["Refused size variables, by reason:", ""]
        out += [f"- {n} x {why}" for why, n in reasons.most_common()]
        out.append("")
    findings = [(x, f) for x in models for f in x.get("findings", [])]
    out += ["## Findings", ""]
    if not findings:
        out += ["None.", ""]
    else:
        out += ["| kind | source | variable | sizes | detail | CmdStan | known |",
                "|---|---|---|---|---|---|---|"]
        order = {k: i for i, k in enumerate(FINDINGS)}
        for x, f in sorted(findings, key=lambda t: (order[t[1]["kind"]],
                                                     t[0]["name"])):
            rec = {"name": x["name"], **f}
            out.append(f"| {f['kind']} | {x['name']} | {f['var']} "
                       f"| {f.get('sizes', '')} | {f['detail'][:200]} "
                       f"| {f.get('cmdstan', '-')} "
                       f"| {known_reason(f['kind'], rec, known) or 'new'} |")
        out.append("")
    out += ["## Ladders", "",
            "| source | variable | kind | sizes | log_prob ops (first -> last) "
            "| executor (last) | ns/grad (first -> last) | op jumps | stop |",
            "|---|---|---|---|---|---|---|---|---|"]
    for x in models:
        for var, lad in (x.get("ladders") or {}).items():
            pts = [p for p in lad["points"] if p.get("ops") is not None]
            if not pts:
                out.append(f"| {x['name']} | {var} | {lad['kind']} | - | - | - "
                           f"| - | - | {lad.get('stop', '')} |")
                continue
            ns = [p for p in pts if p.get("ns")]
            nstext = (f"{ns[0]['ns']:.0f} -> {ns[-1]['ns']:.0f}" if ns else "-")
            out.append(
                f"| {x['name']} | {var} | {lad['kind']} "
                f"| {pts[0]['size']}..{pts[-1]['size']} ({len(pts)}) "
                f"| {pts[0]['ops']} -> {pts[-1]['ops']} "
                f"| {mib(pts[-1].get('executor_bytes'))} | {nstext} "
                f"| {jumps(lad, pts)} | {lad.get('stop', '')} |")
    return "\n".join(out) + "\n"


def jumps(lad, pts):
    """Each located op-count jump as "ops a at n -> ops b at n+1"."""
    ops = {p["size"]: p["ops"] for p in pts}
    return ", ".join(f"{ops.get(lo)} at {lo} -> {ops.get(hi)} at {hi}"
                     for lo, hi in lad.get("thresholds", [])) or "-"
