"""TOML compatibility via the standard library or its maintained backport."""

from __future__ import annotations

try:
    import tomllib
except ImportError:  # Python 3.10 and earlier.
    try:
        import tomli as tomllib  # type: ignore
    except ImportError as exc:
        # Reached by anyone whose system python3 predates 3.11 and who ran
        # the driver with it, which is the documented invocation everywhere
        # but here. Naming the fix costs one line and saves the detour of
        # discovering that requirements.txt already carries the backport.
        raise RuntimeError(
            "Python before 3.11 requires the 'tomli' package; run the "
            "harness under the interpreter that has it, which is the one "
            "tools/dev_setup.sh --conformance installs into "
            ".venv-conformance/bin/python") from exc


loads = tomllib.loads
