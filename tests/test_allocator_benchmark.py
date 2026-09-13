#!/usr/bin/env python3
"""Warmup contract and full-snapshot regression test; no speed threshold.

Uses only the standard library so native CI does not require NumPy.
The measurement driver separately checks a NumPy host loaded before the DSO.
"""
import argparse
import ctypes
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def child(args):
    before = ctypes.create_string_buffer(b"host-owned" * 1024)
    library = ctypes.CDLL(str(args.library.resolve()))
    owns = library.stanli_allocator_bench_owns
    owns.argtypes, owns.restype = [ctypes.c_void_p], ctypes.c_bool
    fresh = ctypes.create_string_buffer(before.raw)
    assert not owns(before) and not owns(fresh)
    entry = library.stanli_allocator_bench_run
    entry.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
    entry.restype = ctypes.c_int
    argv = [b"warmup-test", str(args.mir).encode(), str(args.data).encode(),
            args.mode.encode(), str(args.workers).encode(), b"64", b"2",
            b"1", b"test", str(args.snapshot).encode()]
    native = (ctypes.c_char_p * len(argv))(*argv)
    result = entry(len(argv), native)
    assert before.raw == fresh.raw[:-1]
    assert not owns(before) and not owns(fresh)
    return result


def check(args):
    shipping = ctypes.CDLL(str(args.shipping.resolve()))
    for name in ("stanli_allocator_bench_run", "stanli_allocator_bench_owns"):
        assert not hasattr(shipping, name), "Benchmark entry escaped into shipping library"
    env = {k: v for k, v in os.environ.items() if not k.startswith(
        ("STANLI_", "DYLD_", "MIMALLOC_", "Malloc", "TCMALLOC_"))}
    with tempfile.TemporaryDirectory(prefix="stanli-warmup-test-") as temporary:
        for workers in (1, 4):
            reference = None
            for label, warmup in (("default", None), ("explicit", "500"), ("short", "0"),
                                  ("negative", "-1"), ("excessive", "5001"), ("invalid", "oops")):
                snapshot = pathlib.Path(temporary) / f"{workers}-{label}.snapshot"
                command = [sys.executable, __file__, "--child",
                    "--library", str(args.library), "--mode", args.mode,
                    "--mir", str(args.mir), "--data", str(args.data),
                    "--workers", str(workers), "--snapshot", str(snapshot)]
                extra = {} if warmup is None else {"STANLI_ALLOCATOR_BENCH_WARMUP_MS": warmup}
                result = subprocess.run(command, env={**env, **extra},
                                        text=True, capture_output=True, timeout=30)
                if label in ("negative", "excessive", "invalid"):
                    assert result.returncode != 0 and "FAIL:" in result.stderr, result
                    assert not snapshot.exists(), "Refusal must precede native execution"
                    continue
                assert result.returncode == 0, result.stdout + result.stderr
                rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
                warm = [r for r in rows if r["kind"] == "warmup"]
                requested = 500 if warmup is None else int(warmup)
                assert len(warm) == workers
                assert all(r["requested_ms"] == requested and r["wall_ns"] >= requested * 1e6
                           and r["gradients"] >= 32 for r in warm), warm
                assert len([r for r in rows if r["kind"] == "first_gradient"]) == workers
                assert len([r for r in rows if r["kind"] == "timing"]) == 2
                graph = [{k: v for k, v in r.items() if k not in ("kind", "cycle", "ns")}
                         for r in rows if r["kind"] == "prepare"]
                assert len(graph) == 1 and graph[0]["params"] > 0
                data = snapshot.read_bytes()
                assert len(data.splitlines()) == workers * 8
                current = (data, graph)
                if reference is None:
                    reference = current
                assert current == reference, "Warmup mode changed full gradients or graph"
                print(f"{workers} workers / {label}: warmup, full gradients and graph pass")
    print("Shipping exports and six invalid-warmup refusals pass; no performance claim.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--library", type=pathlib.Path, required=True)
    parser.add_argument("--shipping", type=pathlib.Path)
    parser.add_argument("--mode", choices=("system", "private"), required=True)
    parser.add_argument("--mir", type=pathlib.Path, required=True)
    parser.add_argument("--data", type=pathlib.Path, required=True)
    parser.add_argument("--child", action="store_true")
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--snapshot", type=pathlib.Path)
    arguments = parser.parse_args()
    if arguments.child:
        sys.exit(child(arguments))
    check(arguments)
