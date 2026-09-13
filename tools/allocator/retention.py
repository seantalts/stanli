#!/usr/bin/env python3
"""Fixed-lifetime/size stress in one NumPy host; no allocator tuning/collection.

Run once per variant and compare all snapshot digests. Each of 12 rounds loads
small, large, then small normal models. Four workers each evaluate 8192 changing
gradients per model, in addition to warmup and checks. The native evaluator logs
RSS after every timing block, join, and destruction (not inside the hot loop).
"""
import argparse
import ctypes
import hashlib
import json
import pathlib
import sys
import numpy as np


def main(args):
    out = args.output.resolve()
    out.mkdir(parents=True)
    old = np.arange(8192, dtype=np.float64)
    lib = ctypes.CDLL(str(args.library.resolve()))
    lib.stanli_allocator_bench_owns.argtypes = [ctypes.c_void_p]
    lib.stanli_allocator_bench_owns.restype = ctypes.c_bool
    lib.stanli_allocator_bench_run.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
    lib.stanli_allocator_bench_run.restype = ctypes.c_int
    assert not lib.stanli_allocator_bench_owns(old.ctypes.data)
    cases = {c["name"]: c for c in json.loads(args.inputs.read_text())["cases"]}
    results = []
    for cycle in range(12):
        for name in ["normal_8", "normal_262144", "normal_1024"]:
            inp = pathlib.Path(cases[name]["input_dir"])
            path = out / f"{cycle:02d}-{name}.snapshot"
            print(json.dumps(dict(kind="lifetime", cycle=cycle, name=name)), flush=True)
            argv = [b"retention", str(inp / "model.mir").encode(), str(inp / "data.json").encode(),
                    args.mode.encode(), b"4", b"1024", b"8", b"1", b"retention", str(path).encode()]
            native = (ctypes.c_char_p * len(argv))(*argv)
            assert lib.stanli_allocator_bench_run(len(argv), native) == 0
            results.append(dict(cycle=cycle, name=name,
                                sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    fresh = np.arange(8192, dtype=np.float64)
    assert np.array_equal(old, fresh)
    assert not lib.stanli_allocator_bench_owns(fresh.ctypes.data)
    for name in ["normal_8", "normal_262144", "normal_1024"]:
        assert len({r["sha256"] for r in results if r["name"] == name}) == 1
    (out / "result.json").write_text(json.dumps(dict(mode=args.mode, python=sys.executable,
        native_gradients=12 * 3 * 4 * 1024 * 8, snapshots=results,
        library_sha256=hashlib.sha256(args.library.read_bytes()).hexdigest()), indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--library", type=pathlib.Path, required=True)
    parser.add_argument("--mode", choices=["system", "private"], required=True)
    parser.add_argument("--inputs", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    main(parser.parse_args())
