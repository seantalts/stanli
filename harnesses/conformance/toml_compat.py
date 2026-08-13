"""TOML compatibility via the standard library or its maintained backport."""

from __future__ import annotations

try:
    import tomllib
except ImportError:  # Python 3.10 and earlier.
    try:
        import tomli as tomllib  # type: ignore
    except ImportError as exc:
        raise RuntimeError(
            "Python before 3.11 requires the 'tomli' package") from exc


loads = tomllib.loads
