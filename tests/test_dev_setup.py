"""Run setup in a disposable checkout with toolchain/build commands stubbed.

Verify flag composition, cached-mode transitions and staged artifacts without
installing packages or modifying a developer's real compiler/dependencies.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]


class DevSetupTest(unittest.TestCase):
    def setUp(self):
        # Windows CreateProcess searches System32 before PATH for a bare
        # executable name. Its bash.exe is the WSL launcher, not MSYS2 Bash.
        # Resolve the shell from the development environment's PATH first.
        bash = shutil.which("bash")
        self.assertIsNotNone(bash, "MSYS2/Git Bash or a Unix Bash must be on PATH")
        self.bash = str(Path(bash).resolve())
        self.temp = tempfile.TemporaryDirectory(prefix="stanli setup ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / "fake-bin"
        self.bin.mkdir()
        for name in ("dev_setup.sh", "dev_setup_windows.sh", "build_jobs.sh"):
            dest = self.root / "tools" / name
            dest.parent.mkdir(exist_ok=True)
            shutil.copyfile(SOURCE / "tools" / name, dest)
        self.script("tools/stanc_embed/provenance.sh", """
stanc_embed_artifact_matches() { test -f "$1"; }
""")
        self.script("deps/fetch.sh", "true\n")
        self.script("harnesses/conformance/build_stanc.sh", """
mkdir -p deps/stanc3
printf '#!/usr/bin/env bash\necho test-stanc\n' > deps/stanc3/stanc-pinned
chmod +x deps/stanc3/stanc-pinned
sed -n 's/^STANC3_SRC_SHA=//p' tools/dev_setup.sh > deps/stanc3/stanc-pinned.src
""")
        self.script("tools/stanc_embed/build.sh", """
touch deps/stanc3/stanc_embed.o
printf '#!/usr/bin/env bash\nexit 0\n' > deps/stanc3/stanli-vectorize-probe
chmod +x deps/stanc3/stanli-vectorize-probe
""")
        self.script("tools/stanc_embed/install_overlay.sh", "true\n")
        self.script("harnesses/conformance/fetch_cmdstan.sh", "true\n")
        self.script(".venv-conformance/bin/python", "echo Python-3-test\n")
        (self.root / "deps/stanc3-src").mkdir()
        (self.root / "deps/posteriordb/.git").mkdir(parents=True)
        (self.root / "deps/cmdstan/.git").mkdir(parents=True)
        tbb = self.root / "deps/cmdstan/stan/lib/stan_math/lib/tbb/libtbb.so.2"
        tbb.parent.mkdir(parents=True)
        tbb.touch()
        self.script("tools/corpus.py", "", shell=False)
        self.script("fake-bin/git", """
if [[ "$*" == *'rev-parse HEAD' ]]; then
  sed -n 's/^STANC3_SRC_SHA=//p' tools/dev_setup.sh
fi
""")
        for tool in ("clang", "clang++"):
            self.script("fake-bin/" + tool, "echo clang-test\n")
        self.script("fake-bin/uname", "echo Linux\n")
        self.script("fake-bin/opam", """
case "$1" in
  switch) echo stanc3-55 ;;
  var) echo /test-opam/lib ;;
  exec)
    mkdir -p _build/default/src/stanli_stancjs
    printf '#!/usr/bin/env bash\necho portable\n' > \
      _build/default/src/stanli_stancjs/stanli_compiler_cli.exe ;;
esac
""")
        self.script("fake-bin/cmake", """
printf '<%s>' "$@" >> "$SETUP_TEST_LOG"
printf '\n' >> "$SETUP_TEST_LOG"
mkdir -p build-rel
echo library > build-rel/libstanli.so
""")
        self.script("fake-bin/ctest", "echo ctest >> \"$SETUP_TEST_LOG\"\n")
        self.log = self.root / "cmake.log"
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        STANLI_JOBS="1", SETUP_TEST_LOG="cmake.log")
        for key in ("CC", "CXX", "CONFORMANCE_VENV", "PYTHON"):
            self.env.pop(key, None)

    def script(self, name, body, shell=True):
        dest = self.root / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((("#!/usr/bin/env bash\nset -eu\n" if shell else "")
                          + body).encode("utf-8"))
        dest.chmod(0o755)

    def setup(self, *args, code=0):
        self.log.unlink(missing_ok=True)
        result = subprocess.run([self.bash, "tools/dev_setup.sh", *args],
                                cwd=self.root, env=self.env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result

    def assert_mode(self, embedded):
        configs = [line for line in self.log.read_text().splitlines()
                   if line.startswith("<-B>")]
        self.assertEqual(len(configs), 2)
        for line in configs:
            if embedded:
                self.assertIn("/deps/stanc3/stanc_embed.o>", line)
                self.assertIn("<-DSTANLI_OCAML_STDLIB=/test-opam/lib/ocaml>", line)
            else:
                self.assertIn("<-DSTANLI_STANC_EMBED_OBJ=>", line)
                self.assertIn("<-DSTANLI_OCAML_STDLIB=>", line)

    def test_default_and_cached_mode_transitions(self):
        self.setup()
        self.assert_mode(True)
        self.setup("--no-embed")
        self.assert_mode(False)
        self.assertTrue((self.root / "deps/stanc3/stanli-compile").is_file())
        self.setup("--embed")
        self.assert_mode(True)

    def test_all_respects_no_embed_in_both_orders_and_stages_compiler(self):
        for args in (("--no-embed", "--all"), ("--all", "--no-embed")):
            with self.subTest(args=args):
                self.setup(*args)
                self.assert_mode(False)
                compiler = self.root / "python/stanli/_bin/stanli-compile"
                self.assertEqual(compiler.read_bytes(),
                                 (self.root / "deps/stanc3/stanli-compile").read_bytes())

    def test_no_build_still_produces_selected_compiler(self):
        self.setup("--no-embed", "--no-build")
        self.assertFalse(self.log.exists())
        self.assertTrue((self.root / "deps/stanc3/stanli-compile").is_file())

    def test_separate_compiler_build_and_test_phases(self):
        self.setup("--no-build")
        self.assertFalse(self.log.exists())
        self.setup("--no-test")
        self.assert_mode(True)
        self.assertNotIn("ctest", self.log.read_text())
        self.setup()
        self.assertIn("ctest", self.log.read_text())

    def test_arm64_requires_explicit_no_embed(self):
        self.script("fake-bin/uname", "echo MINGW64_NT\n")
        self.script("fake-bin/powershell.exe", "echo 12\n")
        result = self.setup(code=1)
        self.assertIn("Rerun with --no-embed", result.stderr)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
