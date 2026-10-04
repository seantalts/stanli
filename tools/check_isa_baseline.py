#!/usr/bin/env python3
"""Fail when a binary contains instructions beyond the x86-64 baseline.

CPUID-guarded rdrand/rdseed, tzcnt (which decodes as rep bsf on older CPUs)
and the CET shadow-stack no-ops are allowed.
"""

import argparse
import re
import shutil
import subprocess
import sys

PREFIXES = {
    "lock", "rep", "repz", "repnz", "repe", "repne", "notrack", "bnd", "data16",
    "addr32", "rex64", "cs", "ds", "es", "ss", "fs", "gs", "xacquire", "xrelease",
}
VEX_EXCLUDED = {
    "verr", "verw", "vmcall", "vmclear", "vmfunc", "vmlaunch", "vmload", "vmmcall",
    "vmptrld", "vmptrst", "vmread", "vmresume", "vmrun", "vmsave", "vmwrite",
    "vmxoff", "vmxon",
}
FMA_PREFIXES = ("vfmadd", "vfmsub", "vfnmadd", "vfnmsub", "vfmaddsub", "vfmsubadd")
EXTENSIONS = {
    "bmi1": {"andn", "bextr", "blsi", "blsmsk", "blsr"},
    "bmi2": {"bzhi", "mulx", "pdep", "pext", "rorx", "sarx", "shlx", "shrx"},
    "lzcnt": {"lzcnt"},
    "popcnt": {"popcnt"},
    "movbe": {"movbe"},
    "adx": {"adcx", "adox"},
    "cx16": {"cmpxchg16b"},
    "sse3": {
        "addsubpd", "addsubps", "haddpd", "haddps", "hsubpd", "hsubps", "movddup",
        "movshdup", "movsldup", "lddqu", "fisttp",
    },
    "ssse3": {
        "pshufb", "phaddw", "phaddd", "phaddsw", "phsubw", "phsubd", "phsubsw",
        "pmaddubsw", "pmulhrsw", "psignb", "psignw", "psignd", "pabsb", "pabsw",
        "pabsd", "palignr",
    },
    "sse4.1": {
        "blendpd", "blendps", "blendvpd", "blendvps", "pblendvb", "pblendw", "dpps",
        "dppd", "extractps", "insertps", "movntdqa", "mpsadbw", "packusdw",
        "pcmpeqq", "pextrb", "pextrd", "pextrq", "pinsrb", "pinsrd", "pinsrq",
        "pmaxsb", "pmaxsd", "pmaxud", "pmaxuw", "pminsb", "pminsd", "pminud",
        "pminuw", "pmovsxbw", "pmovsxbd", "pmovsxbq", "pmovsxwd", "pmovsxwq",
        "pmovsxdq", "pmovzxbw", "pmovzxbd", "pmovzxbq", "pmovzxwd", "pmovzxwq",
        "pmovzxdq", "pmuldq", "pmulld", "ptest", "roundpd", "roundps", "roundsd",
        "roundss", "phminposuw",
    },
    "sse4.2": {"pcmpgtq", "pcmpestri", "pcmpestrm", "pcmpistri", "pcmpistrm", "crc32"},
    "aes": {"aesenc", "aesenclast", "aesdec", "aesdeclast", "aesimc", "aeskeygenassist"},
    "pclmul": {"pclmulqdq"},
    "sha": {
        "sha1rnds4", "sha1nexte", "sha1msg1", "sha1msg2", "sha256rnds2",
        "sha256msg1", "sha256msg2",
    },
}
LOOKUP = {m: ext for ext, names in EXTENSIONS.items() for m in names}
ROW = re.compile(r"^\s*([0-9a-f]+):\s*(.*)$")
RAW_BYTES = re.compile(r"^(?:[0-9a-f]{2}\s*)+$")
SYMBOL = re.compile(r"^[0-9a-f]+ <(.*)>:$")


def mnemonic_and_operands(text):
    fields = text.split("\t")
    if len(fields) > 1 and RAW_BYTES.match(fields[0]):
        fields = fields[1:]
    tokens = " ".join(fields).split()
    while tokens and tokens[0] in PREFIXES:
        tokens = tokens[1:]
    if not tokens:
        return "", ""
    return tokens[0].lower(), " ".join(tokens[1:])


def classify(mnemonic, operands):
    if mnemonic.startswith("v") and mnemonic not in VEX_EXCLUDED:
        if (
            "zmm" in operands
            or "%k" in operands
            or "{" in operands
            or re.search(r"[xy]mm(1[6-9]|2\d|3[01])\b", operands)
        ):
            return "avx512"
        if mnemonic.startswith(FMA_PREFIXES):
            return "fma"
        return "avx"
    if mnemonic in LOOKUP:
        return LOOKUP[mnemonic]
    if mnemonic[-1:] in "bwlq" and mnemonic[:-1] in LOOKUP:
        return LOOKUP[mnemonic[:-1]]
    return None


def scan_lines(lines):
    hits = {}
    symbol = None
    for line in lines:
        line = line.rstrip("\n")
        sym = SYMBOL.match(line)
        if sym:
            symbol = sym.group(1)
            continue
        row = ROW.match(line)
        if not row:
            continue
        mnemonic, operands = mnemonic_and_operands(row.group(2))
        if not mnemonic:
            continue
        extension = classify(mnemonic, operands)
        if extension:
            hits.setdefault(extension, []).append(
                (int(row.group(1), 16), line.strip(), symbol)
            )
    return hits


def find_objdump():
    return (
        shutil.which("llvm-objdump")
        or shutil.which("objdump")
        or sys.exit("no objdump found")
    )


def scan_file(path, objdump):
    with subprocess.Popen(
        [objdump, "-d", "--no-show-raw-insn", path],
        stdout=subprocess.PIPE,
        text=True,
        errors="replace",
        bufsize=1 << 20,
    ) as process:
        hits = scan_lines(process.stdout)
    if process.returncode != 0:
        raise RuntimeError(f"{objdump} failed on {path}")
    return hits


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--objdump")
    parser.add_argument("--examples", type=int, default=5)
    parser.add_argument("binaries", nargs="+")
    args = parser.parse_args(argv)
    objdump = args.objdump or find_objdump()
    failed = False
    for path in args.binaries:
        hits = scan_file(path, objdump)
        if not hits:
            print(f"{path}: x86-64 baseline")
            continue
        failed = True
        total = sum(len(rows) for rows in hits.values())
        print(f"{path}: {total} instructions beyond the x86-64 baseline")
        for extension, rows in sorted(hits.items()):
            print(f"  {extension}: {len(rows)}")
            for address, text, symbol in rows[: args.examples]:
                print(f"    {text}" + (f"  in {symbol}" if symbol else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
