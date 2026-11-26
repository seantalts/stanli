"""Compare the output of two stanli_check runs of one model.

stanli_check prints a status line per point (the log density and gradient,
or the error), the write_array row and the model's printed output.
run_outcome() reads one run into a (state, payload) pair, and compare_runs()
lists how two such pairs differ. tools/perturb compares a rewritten model
with its base this way.
"""
from __future__ import annotations

import json
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_stanc3_good as good  # noqa: E402

POINTS = (0, 1, 2)
REL_TOL = 1e-10
ABS_TOL = 1e-12

STATUS_HEADS = ("OK", "EVAL_FAIL", "COMPILE_FAIL", "DATA_FAIL", "WANAMES",
                "WAVALS", "POINT")


def close(a, b, rel=REL_TOL):
    """Do two printed doubles agree? Both NaN, or the same infinity, do."""
    a, b = float(a), float(b)
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    if math.isinf(a) or math.isinf(b):
        return a == b
    return abs(a - b) <= max(ABS_TOL, rel * max(abs(a), abs(b)))


def first_value_diff(want, got):
    """(index, want, got) of the first disagreeing value, or None."""
    if len(want) != len(got):
        return ("width", len(want), len(got))
    for i, (a, b) in enumerate(zip(want, got)):
        if not close(a, b):
            return (i, a, b)
    return None


def printed_lines(text):
    """{section: [line, ...]} of the lines stanli_check did not write itself:
    the model's print statements, and reject messages that print."""
    sections, current = {"pre": []}, "pre"
    for line in text.splitlines():
        head, _, tail = line.partition(" ")
        if head == "POINT" and tail.strip().isdigit():
            current = int(tail)
            sections[current] = []
        elif head in STATUS_HEADS or line in ("WANAMES", "WAVALS"):
            continue
        else:
            sections[current].append(line)
    return sections


def refused(status):
    """Did log_prob refuse? An all-NaN row counts as a refusal."""
    return not good.accepted(status)


def compare_point(k, a, b, prints_a, prints_b):
    """[(kind, detail)] for one point; each detail names the two runs a
    and b."""
    out = []
    sa, sb = a.get("status"), b.get("status")
    if sa is None or sb is None:
        if sa != sb:
            out.append(("one_side_threw", {"point": k, "a": sa, "b": sb}))
        return out
    ra, rb = refused(sa), refused(sb)
    if ra != rb:
        out.append(("one_side_threw", {"point": k, "a": sa, "b": sb}))
    elif ra:
        ma = sa[1] if sa[0] != "OK" else "all NaN"
        mb = sb[1] if sb[0] != "OK" else "all NaN"
        if ma != mb:
            out.append(("error_message", {
                "point": k, "a": ma, "b": mb,
                "normalized_agree": good.normalize(ma) == good.normalize(mb)}))
    else:
        va, vb = sa[1], sb[1]
        if not close(va[0], vb[0]):
            out.append(("lp", {"point": k, "a": va[0], "b": vb[0]}))
        diff = first_value_diff(va[1:], vb[1:])
        if diff:
            out.append(("grad", {"point": k, "index": diff[0],
                                 "a": diff[1], "b": diff[2]}))
    wa, wb = a.get("wa"), b.get("wa")
    if wa != wb:
        if wa is None or wb is None or wa[0] != wb[0]:
            out.append(("one_side_threw", {"point": k, "write_array": True,
                                           "a": wa, "b": wb}))
        elif wa[0] == "threw":
            out.append(("error_message", {
                "point": k, "write_array": True, "a": wa[1],
                "b": wb[1], "normalized_agree":
                good.normalize(wa[1]) == good.normalize(wb[1])}))
        elif wa[1] != wb[1]:
            out.append(("write_array", {"point": k, "names_a": wa[1][:20],
                                        "names_b": wb[1][:20]}))
        else:
            diff = first_value_diff(wa[2], wb[2])
            if diff:
                name = wa[1][diff[0]] if isinstance(diff[0], int) else "width"
                out.append(("write_array", {"point": k, "column": name,
                                            "a": diff[1], "b": diff[2]}))
    if prints_a != prints_b:
        out.append(("output", {"point": k, **printed_diff(prints_a,
                                                          prints_b)}))
    return out


def printed_diff(a, b):
    """Where two lists of printed lines first differ."""
    i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
             min(len(a), len(b)))
    return {"line": i, "lines_a": len(a), "lines_b": len(b),
            "a": a[i:i + 3], "b": b[i:i + 3]}


def run_outcome(run, records):
    """Summarize one stanli_check run: (state, payload).

    state is "timeout", "crash", "refused" (stanli cannot lower or read the
    MIR), "compile_fail" (data or model rejected), or "points"."""
    if run is None:
        return "timeout", None
    code, out, err = run
    parsed = good.parse_output(out)
    pre = parsed["pre"]
    if pre and pre[0] == "COMPILE_FAIL":
        rec = records.compile
        if rec is None or rec.get("ok", True):
            return "crash", f"no compile failure record: {pre[1][:200]}"
        if good.unsupported(rec):
            return "refused", pre[1]
        return "compile_fail", pre[1]
    if code not in (0, 1) or not parsed["points"]:
        tail = (err or out).strip().splitlines()[-1:] or [f"exit {code}"]
        return "crash", f"exit {code}: {tail[0][:200]}"
    points = {k: good.with_status(parsed["points"].get(k), records, k)
              for k in POINTS}
    return "points", {"points": points, "prints": printed_lines(out)}


def constant_lp_shift(pa, pb):
    """Does lp differ by the same amount at every point, with nothing else
    differing in log_prob? Both sides must have values at every point."""
    shifts = []
    for k in POINTS:
        sa, sb = pa["points"][k]["status"], pb["points"][k]["status"]
        if sa is None or sb is None or refused(sa) or refused(sb):
            return False
        if first_value_diff(sa[1][1:], sb[1][1:]):
            return False
        la, lb = float(sa[1][0]), float(sb[1][0])
        if not (math.isfinite(la) and math.isfinite(lb)):
            return False
        shifts.append(la - lb)
    return all(close(s, shifts[0]) for s in shifts)


def compare_runs(a, b, propto=False):
    """(differences, not_comparable_reason) for two (state, payload) pairs
    from run_outcome. Each difference is (kind, detail), and the detail
    keys "a" and "b" hold what each run gave.

    `propto` says the program has a `~` statement or a `target +=` with a
    `_lupdf` or `_lupmf` call. These drop the terms that are constant for
    the argument types they see, and the optimizer can turn an argument
    into data, so for such a program an lp that differs by the same amount
    at every point, with the same gradient, agrees."""
    (sa, pa), (sb, pb) = a, b
    if sa == sb and sa in ("timeout", "crash", "refused"):
        return [], f"both_{sa}"
    if sa == "refused":
        return [], "a_refused"
    for state in ("crash", "timeout"):
        if state in (sa, sb):
            return [(state, {"a": pa if sa == state else None,
                             "b": pb if sb == state else None})], None
    if sb == "refused":
        return [("b_refused", {"stanli_refused": pb})], None
    if sa == "compile_fail" or sb == "compile_fail":
        if sa != sb:
            return [("one_side_threw", {
                "a": pa if sa == "compile_fail" else "ran",
                "b": pb if sb == "compile_fail" else "ran"})], None
        if pa != pb:
            return [("error_message", {
                "a": pa, "b": pb,
                "normalized_agree": good.normalize(pa) == good.normalize(pb)
            })], None
        return [], None
    diffs = []
    for k in POINTS:
        diffs += compare_point(k, pa["points"][k], pb["points"][k],
                               pa["prints"].get(k, []),
                               pb["prints"].get(k, []))
    if propto and constant_lp_shift(pa, pb):
        diffs = [diff for diff in diffs if diff[0] != "lp"]
    if pa["prints"]["pre"] != pb["prints"]["pre"]:
        diffs.append(("output", {"point": "pre", **printed_diff(
            pa["prints"]["pre"], pb["prints"]["pre"])}))
    return diffs, None


def lp_and_rows_differ(a, b):
    """The first difference of lp, write_array rows or which points throw
    between two runs, as one line, or None. Gradients, printed output and
    error wording are not compared."""
    diffs, reason = compare_runs(a, b)
    if reason:
        return reason
    for kind, d in diffs:
        if kind in ("lp", "write_array", "one_side_threw", "crash",
                    "timeout"):
            return f"{kind}: {json.dumps(d)[:300]}"
    return None
