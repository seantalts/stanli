#!/usr/bin/env python3
"""Independent CmdStan checks for selected covariance-structure screen cases.

Retains diagnostics and numerical summaries without asserting that finite
outputs establish correctness or that ill-conditioned cases meet ten ULP.
"""
import argparse
import hashlib
import json
import math
import pathlib
import subprocess
import sys

from covariance_structure_screen import (HERE as SCREEN, REPO, VARIANTS,
                                         model_source, run_bounded)
from custom_covariance import ulps

sys.path.insert(0, str(REPO / "tools"))
from cmdstan_ref import compile_cmd


def compare(actual, expected, names):
    if len(actual) != len(expected):
        return dict(actual_length=len(actual), expected_length=len(expected))
    rows = []
    for name, x, y in zip(names, actual, expected):
        same_class = ((math.isnan(x) and math.isnan(y)) or x == y or
                      (math.isfinite(x) and math.isfinite(y)))
        rows.append(dict(name=name, actual=x, expected=y,
                         matching_classification=same_class,
                         ulp=ulps(x, y) if math.isfinite(x) and math.isfinite(y) else None,
                         absolute=abs(x-y) if math.isfinite(x) and math.isfinite(y) else None,
                         scaled=abs(x-y)/(1+abs(y)) if math.isfinite(x) and math.isfinite(y) else None))
    finite = [r for r in rows if r["ulp"] is not None]
    return dict(count=len(rows), nonmatching_classifications=sum(not r["matching_classification"] for r in rows),
                nonbitwise=sum(r["actual"] != r["expected"] for r in rows),
                over_10_ulp=sum(r["ulp"] > 10 for r in finite),
                max_ulp=max((r["ulp"] for r in finite), default=0),
                max_absolute=max((r["absolute"] for r in finite), default=0),
                max_scaled=max((r["scaled"] for r in finite), default=0),
                worst=sorted(finite, key=lambda r:r["ulp"], reverse=True)[:5])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runtime", required=True, type=pathlib.Path)
    ap.add_argument("--cmdstan", required=True, type=pathlib.Path)
    ap.add_argument("--stanc", required=True, type=pathlib.Path)
    ap.add_argument("--work", required=True, type=pathlib.Path)
    ap.add_argument("--output", required=True, type=pathlib.Path)
    ap.add_argument("--variants", nargs="+", choices=VARIANTS,
                    default=["permuted_writes", "gq_array", "transformed_data"])
    ap.add_argument("--sizes", nargs="+", type=int, default=[0, 1, 35, 36, 64])
    ap.add_argument("--jitters", nargs="+", type=float, default=[.1, 1e-8, 1e-14])
    a = ap.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    sha = lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    revision = lambda p:subprocess.check_output(["git", "-C", str(p), "rev-parse", "HEAD"],text=True).strip()
    report = dict(source_sha=revision(REPO), harness_sha256=sha(pathlib.Path(__file__)),
                  screen_sha256=sha(SCREEN), runtime_sha256=sha(a.runtime),
                  stanc_sha256=sha(a.stanc), cmdstan_sha=revision(a.cmdstan),
                  stan_sha=revision(a.cmdstan/"stan"),
                  math_sha=revision(a.cmdstan/"stan/lib/stan_math"), builds=[], cases=[])
    for variant in a.variants:
        source = a.work/(variant+".stan")
        header = source.with_suffix(".hpp")
        exe = source.with_suffix(".ref")
        source.write_text(model_source(variant))
        commands = [[str(a.stanc),str(source),f"--o={header}"],
                    compile_cmd(a.cmdstan,header,REPO/"tools/ref_driver.cpp",exe,opt="-O1",sundials=False)]
        build = dict(variant=variant, model_sha256=sha(source), commands=commands, diagnostics=[])
        print("Building",variant,flush=True)
        for i,cmd in enumerate(commands):
            r = subprocess.run(cmd,capture_output=True,text=True,timeout=180)
            build["diagnostics"].append(dict(returncode=r.returncode,stdout=r.stdout,stderr=r.stderr))
            (a.work/f"{variant}-build-{i}.log").write_text(r.stdout+r.stderr)
            print(r.stdout+r.stderr,end="",flush=True)
            r.check_returncode()
        report["builds"].append(build)
        for n in a.sizes:
            for jitter in a.jitters:
                data = dict(N=n,x=[i/max(1,n-1) for i in range(n)],y=[math.sin(i) for i in range(n)],
                            jitter=jitter,selector=1,permutation=list(range(n,0,-1)))
                label=f"{variant}-n{n}-j{jitter}"
                path = a.work/(label+".json")
                path.write_text(json.dumps(data))
                cmd=[sys.executable,str(SCREEN),"--worker","--runtime",str(a.runtime),
                     "--variant",variant,"--data",str(path),"--full"]
                native=run_bounded(cmd,30)
                case=dict(variant=variant,N=n,jitter=jitter,data=data,
                          native_returncode=native.returncode,native_stderr=native.stderr,points=[])
                (a.work/(label+"-native.json")).write_text(native.stdout)
                if native.returncode:
                    case["native_stdout"]=native.stdout
                else:
                    got=json.loads(native.stdout)
                    case["native_stage"]=got["stage"]
                    case["native_runtime"]=got["runtime"]
                    for p in got.get("points",[]):
                        ref=run_bounded([str(exe),str(path),str(p["point"])],30)
                        (a.work/(label+f"-point{p['point']}-ref.log")).write_text(ref.stdout+ref.stderr)
                        row=dict(point=p["point"],grad_status=p["grad_status"],grad_error=p["grad_error"],
                                 wa_status=p.get("wa_status"),reference_exit=ref.returncode,reference_stderr=ref.stderr)
                        ok=next((s for s in ref.stdout.splitlines() if s.startswith("OK ")),None)
                        if ok and p["grad_status"]==0:
                            expected=[float(v) for v in ok.split()[1:]]
                            row["density_gradient"]=compare([p["lp"]]+p["gradient"],expected,
                                ["lp"]+[f"grad[{i}]" for i in range(len(p["gradient"]))])
                        else:
                            row["reference_stdout"]=ref.stdout
                        wa=next((s for s in ref.stdout.splitlines() if s.startswith("WAVALS ")),None)
                        names=next((s for s in ref.stdout.splitlines() if s.startswith("WANAMES ")),None)
                        if wa and not wa.startswith("WAVALS FAIL") and p.get("wa_status")==0:
                            row["output_names_match"]=got.get("wa_names")==names[8:].split(",")
                            row["outputs"]=compare(p["wa_values"],[float(v) for v in wa.split()[1:]],names[8:].split(","))
                        row["matching_gradient_classification"]=(bool(ok)==(p["grad_status"]==0))
                        case["points"].append(row)
                report["cases"].append(case)
                print(label,[(p.get("density_gradient",{}).get("max_ulp"),p.get("outputs",{}).get("max_ulp")) for p in case["points"]],flush=True)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(report,indent=2)+"\n")


if __name__=="__main__":main()
