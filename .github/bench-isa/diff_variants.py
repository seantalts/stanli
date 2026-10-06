#!/usr/bin/env python3
import argparse
import concurrent.futures
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import verify_refs  # noqa: E402


def evaluate(binary, stan, data, point, timeout):
    try:
        p = subprocess.run([str(binary), str(stan), str(data), "--point",
                            str(point)], capture_output=True, text=True,
                           timeout=timeout, cwd=REPO)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    got = verify_refs.parse_status(p.stdout)
    if not got:
        return None, "crash"
    if got[0] != "OK":
        return None, got[0]
    return [float(x) for x in got[1:]], "OK"


def one_model(model, ref, pdb, variants, tmp, timeout):
    stan, data = verify_refs.model_files(model, ref, pdb, tmp)
    if not stan.exists() or not data.exists():
        return model, {}
    result = {}
    for point in verify_refs.POINTS:
        base, bstat = evaluate(variants[0][1], stan, data, point, timeout)
        for name, binary in variants[1:]:
            cur = result.setdefault(name, [0, 0.0, 0, "OK"])
            val, stat = evaluate(binary, stan, data, point, timeout)
            if base is None or val is None:
                if bstat != stat:
                    cur[3] = f"{bstat}/{stat}"
                continue
            if len(base) != len(val):
                cur[3] = "shape"
                continue
            for a, b in zip(base, val):
                rel, ulp = verify_refs.pair_dev(a, b)
                cur[0] = max(cur[0], ulp)
                cur[1] = max(cur[1], rel)
                cur[2] += ulp != 0
    return model, result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", action="append", required=True)
    ap.add_argument("--pdb", type=pathlib.Path, required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timeout", type=float, default=300)
    args = ap.parse_args()
    variants = []
    for spec in args.variant:
        name, _, path = spec.partition("=")
        variants.append((name, pathlib.Path(path).resolve() / "stanli_check"))
    pdb = args.pdb / "posterior_database"
    refs, _ = verify_refs.load_refs()
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="diff_isa_"))
    names = [n for n, _ in variants[1:]]
    with args.out.open("w") as out, \
            concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        out.write("model\t" + "\t".join(
            f"{n}_max_ulp\t{n}_max_rel\t{n}_values_moved\t{n}_status"
            for n in names) + "\n")
        futs = [pool.submit(one_model, m, refs[m], pdb, variants, tmp,
                            args.timeout) for m in sorted(refs)]
        for fut in concurrent.futures.as_completed(futs):
            model, res = fut.result()
            cells = []
            for n in names:
                u, r, moved, st = res.get(n, ["", "", "", "missing"])
                cells += [u, f"{r:.3e}" if r != "" else "", moved, st]
            out.write(model + "\t" + "\t".join(map(str, cells)) + "\n")
            out.flush()


if __name__ == "__main__":
    main()
