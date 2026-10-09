#!/usr/bin/env python3
"""Summarise a survey directory: coverage, validity and error classes.

  tools/hp/survey_report.py DIR [--gate 1e-12]
"""
import argparse
import collections
import json
import pathlib
import re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--gate", type=float, default=1e-12)
    a = ap.parse_args()
    ok, bad, errs = [], [], collections.defaultdict(list)
    times = []
    for p in sorted(pathlib.Path(a.dir).glob("*.json")):
        r = json.loads(p.read_text())
        if "error" in r:
            e = re.sub(r"\d+", "N", r["error"])[:70]
            errs[e].append(r["model"])
            continue
        devs = [o.get("dev_cmdstan", -1) for o in r["points"].values()]
        t = r["total_s"]
        times.append((t, r["model"], r["n"]))
        if all(d <= a.gate for d in devs):
            ok.append(r["model"])
        else:
            bad.append((r["model"], max(devs)))
    print("evaluated", len(ok) + len(bad), "valid", len(ok), "mismatch", len(bad),
          "errors", sum(len(x) for x in errs.values()))
    for m, d in sorted(bad, key=lambda x: -x[1]):
        print("  MISMATCH %-34s %.2e" % (m, d))
    for e, ms in sorted(errs.items(), key=lambda x: -len(x[1])):
        print("  ERR %3d %-60s %s" % (len(ms), e, " ".join(ms[:4])))
    times.sort(reverse=True)
    print("slowest", [(round(t, 1), m, n) for t, m, n in times[:8]])


if __name__ == "__main__":
    main()
