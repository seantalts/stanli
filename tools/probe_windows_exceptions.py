"""Temporary native Windows diagnosis; never a replacement for CTest."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess


def run(command, destination, timeout=90):
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            text=True, errors="replace", timeout=timeout)
    destination.write_text(result.stdout + result.stderr, encoding="utf-8")
    print(json.dumps({"command": [str(x) for x in command],
                      "returncode": result.returncode,
                      "stdout": result.stdout[:2200],
                      "stderr": result.stderr[:2200]}), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("stage")
    args = parser.parse_args()
    out = Path("exception-probe") / args.stage
    out.mkdir(parents=True, exist_ok=True)
    data = out / "nonfinite.json"
    data.write_text('{"N": Infinity}', encoding="utf-8")
    binary = Path("build/stanli_check.exe").resolve()
    if args.stage == "embedded-dynamic":
        link = Path("build/CMakeFiles/stanli_check.dir/link.txt")
        link.write_text(re.sub(r"(?<!\S)-static(?!\S)", "", link.read_text()))
        subprocess.run(["cmake", "-E", "cmake_link_script",
                        "CMakeFiles/stanli_check.dir/link.txt", "--verbose=1"],
                       cwd="build", check=True)
    elif args.stage == "external-static":
        subprocess.run(["cmake", "-B", "build",
                        "-DCMAKE_EXE_LINKER_FLAGS=-static"], check=True)
        subprocess.run(["cmake", "--build", "build", "--target", "stanli_check"],
                       check=True)
    compiler = Path("probe-compilers/stanli-compile.exe").resolve()
    if not compiler.exists():
        compiler = Path("probe-compilers/stanli-compile").resolve()
    stock = Path("deps/stanc3/stanc.exe").resolve()
    if not stock.exists():
        stock = Path("deps/stanc3/stanc").resolve()
    if args.stage.startswith("external"):
        shutil.copy2(compiler, binary.parent / "stanli-compile.exe")
    cases = [
        ("nonfinite", Path("tests/lit/issue_320/nonfinite_int_data_rejected.stan"),
         data, []),
        ("sort", Path("tests/fixtures/sort_functions.stan"),
         Path("tests/function_coverage/empty.json"), ["--point", "0", "--wa-values"]),
    ]
    for name, source, values, options in cases:
        for backend, flags in (("default", []),
                               ("portable", ["--stanli-compile", compiler]),
                               ("stock", ["--stanc", stock])):
            run([binary, source, values, *options, *flags],
                out / f"{name}-{backend}.log")
    run(["llvm-nm", "--defined-only", binary], out / "symbols.txt")
    run(["llvm-objdump", "-p", binary], out / "pe.txt")
    for source in (Path("build/CMakeFiles/stanli_check.dir/link.txt"),
                   Path("build/stanli-check.map"), Path("build/CMakeCache.txt")):
        if source.exists():
            shutil.copy2(source, out / source.name)
    shutil.copy2(binary, out / binary.name)
    pacman = Path(shutil.which("pacman")).resolve()
    gdb = pacman.parents[2] / "mingw64/bin/gdb.exe"
    if args.stage == "embedded" and gdb.exists():
        for name, source, values, options in cases:
            run([gdb, "--batch", "-ex", "set pagination off",
                 "-ex", "set confirm off", "-ex", "break __cxa_throw",
                 "-ex", "run", "-ex", "bt", "-ex", "continue", "-ex", "bt",
                 "--args", binary, source, values, *options],
                out / f"{name}-gdb.log", timeout=120)


if __name__ == "__main__":
    main()
