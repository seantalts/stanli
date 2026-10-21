"""The pinned Stan toolchain: deps/pins.env and the checks that enforce it."""
import pathlib
import subprocess

REPO = pathlib.Path(__file__).resolve().parent.parent
PINS_FILE = REPO / "deps" / "pins.env"


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


def git_head(path):
    path = pathlib.Path(path)
    return subprocess.check_output(
        ["git", "-c", f"safe.directory={path.resolve()}", "-C", str(path), "rev-parse", "HEAD"],
        text=True, stderr=subprocess.DEVNULL).strip()
