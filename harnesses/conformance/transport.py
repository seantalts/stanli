"""Availability and identity checks for the shared BridgeStan client."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
from typing import Dict, Mapping, Optional


class TransportError(RuntimeError):
    pass


def worker_environment(pythonpath: Optional[pathlib.Path]) -> Mapping[str, str]:
    environment = dict(os.environ)
    if pythonpath is not None:
        previous = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = (str(pythonpath) + os.pathsep + previous
                                     if previous else str(pythonpath))
    return environment


_IDENTITY_PROGRAM = r"""
import bridgestan, json, platform, stanli
missing = [name for name in ("bridgestan_model", "stan_to_mir")
           if not hasattr(stanli, name)]
if missing:
    raise RuntimeError("stanli does not expose " + ", ".join(missing))
build_id = getattr(stanli, "build_id", None)
print(json.dumps({
    "python": platform.python_version(),
    "stanli_version": getattr(stanli, "__version__", None),
    "stanli_build_id": build_id() if callable(build_id) else None,
    "bridgestan_version": getattr(bridgestan, "__version__", None),
}, sort_keys=True))
"""


def transport_identity(python_executable: pathlib.Path,
                       pythonpath: Optional[pathlib.Path],
                       expected_bridgestan: Optional[str] = None,
                       timeout: float = 30.0) -> Dict[str, object]:
    try:
        completed = subprocess.run(
            [str(python_executable), "-c", _IDENTITY_PROGRAM],
            capture_output=True, text=True,
            env=worker_environment(pythonpath), timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise TransportError(f"could not start BridgeStan transport: {exc}") \
            from exc
    if completed.returncode:
        detail = (completed.stderr or completed.stdout).strip()
        raise TransportError(f"BridgeStan transport is unavailable: "
                             f"{detail[-1000:]}")
    try:
        value = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise TransportError("transport identity returned invalid JSON") \
            from exc
    if not isinstance(value, dict):
        raise TransportError("transport identity is not an object")
    actual = value.get("bridgestan_version")
    if expected_bridgestan is not None and actual != expected_bridgestan:
        raise TransportError(
            f"BridgeStan Python {actual!r} != reference "
            f"{expected_bridgestan!r}")
    return dict(value)
