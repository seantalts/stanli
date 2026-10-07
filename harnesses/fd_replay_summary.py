#!/usr/bin/env python3
"""Per-arm summary of a fd_replay.py run."""
import json
import math
import sys


def main():
    path = sys.argv[1]
    exact = sys.argv[2] if len(sys.argv) > 2 else "A"
    recs = [json.loads(line) for line in open(path)]
    arms = list(recs[0]["arms"])
    models = sorted({r["model"] for r in recs})
    print(f"{len(models)} models, {len(recs)} points, exact arm {exact}")
    hdr = ("arm", "pts_bytes_diff", "pts_val_diff", "models_val_diff", "pts_wa_diff",
           "max_scaled_err_vs_exact", "status_diff", "brms_pts_diff", "brms_max_ulp_vs_exact",
           "brms_max_ulp_vs_cmdstan", "max_scaled_err_vs_cmdstan", "gate_violations")
    print("\t".join(hdr))
    out = {}
    for arm in arms:
        bytes_diff = val_diff = wa_diff = status_diff = viol = 0
        dm = set()
        mr = 0
        cu = cr = 0.0
        bp = bu = bc = 0
        worst_model = ("", 0)
        violators = []
        for r in recs:
            a, e = r["arms"][arm], r["arms"][exact]
            if not a["same_bytes"]:
                bytes_diff += 1
            if a["status"] != e["status"]:
                status_diff += 1
            d = a["vs_exact"]
            if d is not None:
                mr = max(mr, d[0])
                if r["ulp_limit"] is not None:
                    bu = max(bu, d[1])
                    bp += d[1] > 0
                if d[1] > 0 or d[0] > 0:
                    val_diff += 1
                    dm.add(r["model"])
                    if d[1] > worst_model[1]:
                        worst_model = (r["model"], d[1])
            d = a["vs_exact_wa"]
            if d is not None and (d[0] > 0 or d[1] > 0):
                wa_diff += 1
            d = a["vs_cmdstan"]
            if d is not None:
                rel, ulp = d
                if r["ref_status"] != "MISMATCH" and not r["ill"]:
                    cr = max(cr, rel)
                if r["ulp_limit"] is not None:
                    bc = max(bc, ulp)
                gate = r["gate"] if r["gate"] is not None else 1e-9
                if rel >= gate or (r["ulp_limit"] is not None and ulp > r["ulp_limit"]):
                    viol += 1
                    violators.append((r["model"], r["point"], rel, ulp))
        out[arm] = dict(violators=violators, worst_model=worst_model, models=sorted(dm))
        print("\t".join(str(x) for x in (arm, bytes_diff, val_diff, len(dm), wa_diff,
                                         f"{mr:.2e}", status_diff, bp, bu, bc, f"{cr:.2e}", viol)))
    for arm in arms:
        o = out[arm]
        if o["violators"]:
            print(f"{arm} gate violations: {o['violators'][:12]}")
    json.dump(out, open(path + ".summary.json", "w"), indent=1)


if __name__ == "__main__":
    main()
