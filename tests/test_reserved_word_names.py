#!/usr/bin/env python3
"""Identifiers stanc3 mangles as C++ reserved words keep their Stan names.

stanc3 spells `class` as `_stan_class` in the MIR. The data keys and the
write_array column names stay `class` and `friend`, as in CmdStan.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
check = str(pathlib.Path(sys.argv[1]).resolve())
FIX = ROOT / "tests" / "fixtures"

proc = subprocess.run(
    [check, str(FIX / "reserved_words.stan"), str(FIX / "reserved_words.json"),
     "--point", "0", "--wa-values"],
    capture_output=True, text=True, timeout=120)
out = proc.stdout
assert proc.returncode == 0 and "\nOK " in "\n" + out, (proc.returncode, out)

lines = {line.split(" ", 1)[0]: line.split(" ", 1)[1] for line in out.splitlines()
         if " " in line}
names = lines["WANAMES"].split(",")
assert names == ["explicit", "signed", "friend"], names

values = [float(v) for v in lines["WAVALS"].split()]
explicit, signed, friend = values
assert signed == 2 * explicit, values
assert friend == signed + 6.0, values
print("reserved word names OK")
