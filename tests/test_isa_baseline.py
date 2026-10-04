"""The Linux x86-64 runtime must not use instructions beyond the x86-64 baseline."""

import importlib.util
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_isa_baseline", ROOT / "tools/check_isa_baseline.py"
)
check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check)

LLVM_ROWS = [
    "  401000: \tvfmadd231sd\t%xmm1, %xmm2, %xmm0",
    "  401005: \tvaddpd\t%ymm1, %ymm2, %ymm3",
    "  40100a: \tvpaddd\t%zmm1, %zmm2, %zmm3",
    "  40100f: \tpopcntq\t%rdi, %rax",
    "  401014: \tlzcntq\t%rdi, %rax",
    "  401019: \tandnq\t%rdi, %rsi, %rax",
    "  40101e: \tshlxq\t%rdi, %rsi, %rax",
    "  401023: \tpshufb\t%xmm1, %xmm0",
    "  401028: \tpminsd\t%xmm1, %xmm0",
    "  40102d: \tpcmpgtq\t%xmm1, %xmm0",
    "  401032: \tmovbeq\t(%rdi), %rax",
    "  401037: \tlock\tcmpxchg16b\t(%rdi)",
    "  40103c: \taesenc\t%xmm1, %xmm0",
    "  401041: \tmovddup\t%xmm1, %xmm0",
    "  401046: \troundsd\t$9, %xmm1, %xmm0",
]
GNU_ROWS = [
    "  401000:\tc4 e2 f1 b9 c2    \tvfmadd231sd %xmm2,%xmm1,%xmm0",
    "  401005:\tf3 48 0f b8 c7    \tpopcnt %rdi,%rax",
    "  40100a:\tc4 e2 f0 f2 c6    \tandn  %rsi,%rcx,%rax",
    "  40100f:\tf0 48 0f c7 0f    \tlock cmpxchg16b (%rdi)",
]
ALLOWED_ROWS = [
    "  401000: \ttzcntq\t%rdi, %rax",
    "  401004: \tendbr64",
    "  401008: \trdsspq\t%rax",
    "  40100c: \tincsspq\t%rax",
    "  401010: \trdrand\t%eax",
    "  401014: \trdseed\t%eax",
    "  401018: \tcpuid",
    "  40101c: \txgetbv",
    "  401020: \tmovsd\t(%rdi), %xmm0",
    "  401024: \tmulsd\t%xmm1, %xmm0",
    "  401028: \tcvtsi2sd\t%rdi, %xmm0",
    "  40102c: \tcmpxchgq\t%rsi, (%rdi)",
    "  401030: \tlock\tcmpxchgq\t%rsi, (%rdi)",
    "  401034: \tsqrtsd\t%xmm1, %xmm0",
    "  401038: \tpxor\t%xmm0, %xmm0",
    "  40103c: \tshlq\t%rax",
]
CONTROL_SOURCE = """
double dot(const double *a, const double *b, int n) {
  double s = 0;
  for (int i = 0; i < n; i++) s += a[i] * b[i];
  return s;
}
unsigned bits(unsigned long x) {
  return __builtin_popcountl(x) + __builtin_clzl(x | 1);
}
"""


def classes(rows):
    return set(check.scan_lines(rows))


def find_tool(*names):
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    for path in ("/opt/homebrew/opt/llvm/bin/" + n for n in names):
        if pathlib.Path(path).exists():
            return path
    return None


class ScanLinesTest(unittest.TestCase):
    def test_llvm_rows_are_each_flagged(self):
        for row in LLVM_ROWS:
            with self.subTest(row=row):
                self.assertTrue(classes([row]), row)

    def test_gnu_rows_with_raw_bytes_are_flagged(self):
        for row in GNU_ROWS:
            with self.subTest(row=row):
                self.assertTrue(classes([row]), row)

    def test_baseline_and_guarded_rows_pass(self):
        for row in ALLOWED_ROWS:
            with self.subTest(row=row):
                self.assertFalse(classes([row]), row)

    def test_extension_names(self):
        by_row = {r: classes([r]) for r in LLVM_ROWS}
        self.assertEqual(by_row[LLVM_ROWS[0]], {"fma"})
        self.assertEqual(by_row[LLVM_ROWS[1]], {"avx"})
        self.assertEqual(by_row[LLVM_ROWS[2]], {"avx512"})
        self.assertEqual(by_row[LLVM_ROWS[3]], {"popcnt"})
        self.assertEqual(by_row[LLVM_ROWS[4]], {"lzcnt"})
        self.assertEqual(by_row[LLVM_ROWS[5]], {"bmi1"})
        self.assertEqual(by_row[LLVM_ROWS[6]], {"bmi2"})

    def test_hits_carry_address_and_text(self):
        hits = check.scan_lines(LLVM_ROWS[3:4])["popcnt"]
        self.assertEqual(hits[0][0], 0x40100F)
        self.assertIn("popcntq", hits[0][1])


class ControlBinaryTest(unittest.TestCase):
    def build(self, flags):
        compiler = find_tool("clang")
        objdump = find_tool("llvm-objdump", "objdump")
        if not compiler or not objdump:
            self.skipTest("clang and objdump are required")
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp, "t.c")
            obj = pathlib.Path(tmp, "t.o")
            src.write_text(CONTROL_SOURCE)
            subprocess.run(
                [compiler, "--target=x86_64-linux-gnu", "-O3", "-c", *flags,
                 str(src), "-o", str(obj)],
                check=True,
            )
            return check.scan_file(str(obj), objdump)

    def test_baseline_object_passes(self):
        self.assertFalse(self.build(["-march=x86-64"]))

    def test_v3_object_is_flagged(self):
        found = self.build(["-march=x86-64-v3"])
        self.assertIn("fma", found)
        self.assertIn("popcnt", found)

    def test_cli_exit_status(self):
        compiler = find_tool("clang")
        objdump = find_tool("llvm-objdump", "objdump")
        if not compiler or not objdump:
            self.skipTest("clang and objdump are required")
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp, "t.c")
            src.write_text(CONTROL_SOURCE)
            status = {}
            for name, flag in (("ok", "-march=x86-64"), ("bad", "-march=x86-64-v3")):
                obj = pathlib.Path(tmp, name + ".o")
                subprocess.run(
                    [compiler, "--target=x86_64-linux-gnu", "-O3", "-c", flag,
                     str(src), "-o", str(obj)],
                    check=True,
                )
                status[name] = subprocess.run(
                    ["python3", str(ROOT / "tools/check_isa_baseline.py"),
                     "--objdump", objdump, str(obj)],
                    capture_output=True, text=True,
                )
            self.assertEqual(status["ok"].returncode, 0, status["ok"].stdout)
            self.assertEqual(status["bad"].returncode, 1)
            self.assertIn("fma", status["bad"].stdout)


if __name__ == "__main__":
    unittest.main()
