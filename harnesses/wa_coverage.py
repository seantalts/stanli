#!/usr/bin/env python3
"""Compatibility entry point; the implementation lives in verify_refs.py."""
import pathlib
import subprocess
import sys

VERIFY = pathlib.Path(__file__).resolve().parents[1] / "tools" / "verify_refs.py"
raise SystemExit(subprocess.call(
    [sys.executable, str(VERIFY), sys.argv[1], "--wa-report", *sys.argv[2:]]))
