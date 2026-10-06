#!/usr/bin/env python3
import json
import shlex
import sys

build_dir, name, flags = sys.argv[1:4]
entries = json.load(open(f"{build_dir}/compile_commands.json"))
wanted = ("runtime/kernels/densities_common.cpp", "tools/bench_grad.cpp",
          "tools/stanli_check.cpp")
ok = True
for suffix in wanted:
    hits = [e for e in entries if e["file"].endswith(suffix)]
    if not hits:
        print(f"{name}: no compile command for {suffix}")
        ok = False
        continue
    cmd = hits[0].get("command") or " ".join(hits[0]["arguments"])
    toks = shlex.split(cmd)
    picked = [t for t in toks if t.startswith(("-march", "-mno-", "-mfma",
                                               "-ffp-contract", "-O", "-m"))]
    print(f"{name}: {suffix}: {' '.join(picked)}")
    if "-ffp-contract=off" not in toks:
        print(f"{name}: {suffix} lacks -ffp-contract=off")
        ok = False
    for want in flags.split():
        if want not in toks:
            print(f"{name}: {suffix} lacks {want}")
            ok = False
    if not flags and any(t.startswith("-march") for t in toks):
        print(f"{name}: baseline has -march")
        ok = False
sys.exit(0 if ok else 1)
