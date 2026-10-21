"""The pinned Stan toolchain: deps/pins.env and the checks that enforce it."""
import hashlib
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
PINS_FILE = REPO / "deps" / "pins.env"
PINNED_STANC = REPO / "deps" / "stanc3" / "stanc-pinned"
STAMP_NAME = ".stanli-build-stamp"
SETUP_COMMAND = "harnesses/conformance/fetch_cmdstan.sh"
LABELS = {
    "cmdstan_head": "CmdStan HEAD",
    "stan_head": "Stan (stan submodule) HEAD",
    "math_head": "Stan Math HEAD",
    "cmdstan_tree": "CmdStan tracked files",
    "cmdstan_stanc": "CmdStan bin/stanc sha256",
    "stanc": "stanc sha256",
    "pinned_stanc_provenance": "deps/stanc3/stanc-pinned built from stanc3",
}


def parse_pins(text):
    pins = {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key.isidentifier():
            raise ValueError(f"pins file line {number} is not KEY=value: {line!r}")
        pins[key] = value
    return pins


def pins():
    return parse_pins(PINS_FILE.read_text())


def cmdstan_version():
    return pins()["CMDSTAN_TAG"].removeprefix("v")


def _git(path, *args):
    path = pathlib.Path(path)
    return subprocess.check_output(
        ["git", "-c", f"safe.directory={path.resolve()}", "-C", str(path), *args],
        text=True, stderr=subprocess.DEVNULL).rstrip("\n")


def git_head(path):
    return _git(path, "rev-parse", "HEAD").strip()


def sha256_file(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def default_stanc(cmdstan_dir):
    cmdstan_dir = pathlib.Path(cmdstan_dir)
    exe = cmdstan_dir / "bin" / "stanc.exe"
    return exe if exe.exists() else cmdstan_dir / "bin" / "stanc"


def _heads(cmdstan_dir):
    cmdstan_dir = pathlib.Path(cmdstan_dir)
    return dict(cmdstan=git_head(cmdstan_dir),
                stan=git_head(cmdstan_dir / "stan"),
                math=git_head(cmdstan_dir / "stan" / "lib" / "stan_math"))


def require_pinned_toolchain(cmdstan_dir, stanc_path=None, allow_unpinned=False,
                             stanc_declared=False):
    """Refuse to compile against anything but the toolchain in deps/pins.env.

    Returns {"mismatches": [...], "stanc_sha256": ...}. A mismatch raises
    ValueError unless allow_unpinned, in which case the caller records the
    returned entries. CmdStan's own bin/stanc is always checked. A stanc_path
    is checked against the pinned compiler unless stanc_declared says it was
    chosen on purpose; the sha256 of the compiler in use is returned either way.
    """
    cmdstan_dir = pathlib.Path(cmdstan_dir)
    pin = pins()
    try:
        heads = _heads(cmdstan_dir)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ValueError(f"{cmdstan_dir} is not a CmdStan git checkout") from exc

    mismatches = []

    def check(name, expected, found):
        if expected != found:
            mismatches.append(dict(check=name, expected=expected, found=found))

    check("cmdstan_head", pin["CMDSTAN_SHA"], heads["cmdstan"])
    check("stan_head", pin["STAN_SHA"], heads["stan"])
    check("math_head", pin["MATH_SHA"], heads["math"])
    dirty = [line for line in _git(cmdstan_dir, "status", "--porcelain",
                                   "--untracked-files=no",
                                   "--ignore-submodules=untracked").splitlines()
             if line[3:] != "make/local"]
    check("cmdstan_tree", "no tracked changes", "; ".join(dirty) or "no tracked changes")

    pinned = pathlib.Path(PINNED_STANC)
    provenance = pathlib.Path(str(pinned) + ".src")
    recorded = provenance.read_text().strip() if provenance.exists() else None
    check("pinned_stanc_provenance", pin["STANC3_SRC_SHA"], recorded)
    pinned_sha = sha256_file(pinned) if pinned.exists() else None
    installed = default_stanc(cmdstan_dir)
    installed_sha = sha256_file(installed) if installed.exists() else None
    check("cmdstan_stanc", pinned_sha, installed_sha)
    stanc_sha = installed_sha
    if stanc_path is not None:
        stanc_sha = sha256_file(stanc_path)
        if not stanc_declared:
            check("stanc", pinned_sha, stanc_sha)

    if mismatches and not allow_unpinned:
        lines = [f"  {LABELS[m['check']]}: expected {m['expected']}, found {m['found']}"
                 for m in mismatches]
        raise ValueError(
            f"{cmdstan_dir} does not match deps/pins.env:\n" + "\n".join(lines)
            + f"\nRun {SETUP_COMMAND} (tools/dev_setup.sh --corpus) in the checkout"
              " that owns deps/cmdstan to install the pinned toolchain.")
    return dict(mismatches=mismatches, stanc_sha256=stanc_sha)


def compiler_identity(cmdstan_dir):
    printed = subprocess.check_output(["make", "-s", "-C", str(cmdstan_dir), "print-CXX"],
                                      text=True, stderr=subprocess.DEVNULL)
    cxx = printed.split("=", 1)[1].strip()
    version = subprocess.check_output([cxx, "--version"], text=True).splitlines()[:2]
    return f"{cxx}: " + " / ".join(version)


def build_stamp(cmdstan_dir):
    heads = _heads(cmdstan_dir)
    stanc = default_stanc(cmdstan_dir)
    return "".join(f"{key}={value}\n" for key, value in (
        ("cmdstan", heads["cmdstan"]), ("stan", heads["stan"]), ("math", heads["math"]),
        ("stanc_sha256", sha256_file(stanc)),
        ("compiler", compiler_identity(cmdstan_dir))))


def stamp_is_current(cmdstan_dir):
    stamp = pathlib.Path(cmdstan_dir) / STAMP_NAME
    return stamp.exists() and stamp.read_text() == build_stamp(cmdstan_dir)


def main(argv):
    command, cmdstan_dir = argv[1], pathlib.Path(argv[2])
    if command == "require":
        try:
            require_pinned_toolchain(cmdstan_dir)
        except ValueError as exc:
            print(exc, file=sys.stderr)
            return 1
        return 0
    if command == "stamp-current":
        return 0 if stamp_is_current(cmdstan_dir) else 1
    if command == "write-stamp":
        (cmdstan_dir / STAMP_NAME).write_text(build_stamp(cmdstan_dir))
        return 0
    print("usage: toolchain.py require|stamp-current|write-stamp CMDSTAN_DIR", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
