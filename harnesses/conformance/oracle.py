"""Pinned BridgeStan reference compilation and content-addressed caching."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import threading
import time
from typing import Dict, Mapping, Optional, Sequence, Tuple


class OracleError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class ToolchainVersions:
    cmdstan_commit: str
    stan_commit: str
    stan_math_commit: str
    stanc_build_id: str
    stanc_sha256: str
    bridgestan_commit: str
    bridgestan_version: str
    compiler: str
    compile_flags: Tuple[str, ...]

    def to_dict(self) -> Dict[str, object]:
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class ReferenceBuild:
    executable: Optional[pathlib.Path]
    error: Optional[str]
    source: pathlib.Path
    source_sha256: str
    build_seconds: float = 0.0
    cache_hit: bool = False


def _run(command: Sequence[object], timeout: float = 30.0) \
        -> subprocess.CompletedProcess:
    argv = [str(item) for item in command]
    try:
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout)
    except OSError as exc:
        raise OracleError(f"could not execute {argv[0]}: {exc}") from exc
    except subprocess.TimeoutExpired as exc:
        raise OracleError(f"timed out running {' '.join(argv)}") from exc


def _git_head(path: pathlib.Path) -> str:
    result = _run(("git", "-C", path, "rev-parse", "HEAD"))
    if result.returncode or not result.stdout.strip():
        raise OracleError(f"{path} is not a readable pinned git checkout")
    return result.stdout.strip()


def _sha256(path: pathlib.Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as exc:
        raise OracleError(f"could not hash {path}: {exc}") from exc


def _stanc_identity(stanc: pathlib.Path) -> Tuple[str, str]:
    result = _run((stanc, "--version"))
    version = (result.stdout or result.stderr).strip()
    if result.returncode or not version:
        raise OracleError(f"could not identify stanc at {stanc}")
    return version, _sha256(stanc)


def _bridgestan_version(path: pathlib.Path) -> str:
    header = (path / "src" / "version.hpp").read_text(encoding="utf-8")
    values = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^#define BRIDGESTAN_{name} (\d+)$", header, re.M)
        if match is None:
            raise OracleError(f"could not read BridgeStan {name.lower()} version")
        values.append(match.group(1))
    return ".".join(values)


def _make_assignments(cmdstan: pathlib.Path, stanc: pathlib.Path,
                      compiler: str) -> Tuple[str, ...]:
    return (
        f"STAN={cmdstan.resolve() / 'stan'}/",
        f"STANC={stanc.resolve()}",
        f"CXX={compiler}",
        "O=1",
        "CXXFLAGS+=-ffp-contract=off",
    )


def _make_value(bridgestan: pathlib.Path, assignments: Sequence[str],
                variable: str) -> str:
    result = _run(("make", "-s", "-C", bridgestan, *assignments,
                   f"print-{variable}"))
    if result.returncode:
        raise OracleError(
            f"could not resolve BridgeStan {variable}: "
            f"{(result.stderr or result.stdout).strip()[-1000:]}")
    return result.stdout.strip()


def validate_toolchain(cmdstan: pathlib.Path, stanc: pathlib.Path,
                       deps: pathlib.Path, compiler: str = "clang++") \
        -> ToolchainVersions:
    """Prove BridgeStan will use the exact Stan sources pinned by stanli."""
    cmdstan, deps = pathlib.Path(cmdstan).resolve(), pathlib.Path(deps).resolve()
    bridgestan = deps / "bridgestan"
    expected_stan, expected_math = (_git_head(deps / "stan"),
                                    _git_head(deps / "math"))
    actual_stan = _git_head(cmdstan / "stan")
    actual_math = _git_head(cmdstan / "stan" / "lib" / "stan_math")
    problems = []
    if actual_stan != expected_stan:
        problems.append(f"Stan {actual_stan} != stanli pin {expected_stan}")
    if actual_math != expected_math:
        problems.append(f"Stan Math {actual_math} != stanli pin {expected_math}")

    stanc_build_id, stanc_sha256 = _stanc_identity(stanc)
    cmdstan_stanc = cmdstan / "bin" / ("stanc.exe" if sys.platform == "win32"
                                       else "stanc")
    if not cmdstan_stanc.exists():
        problems.append(f"CmdStan stanc is missing at {cmdstan_stanc}")
    else:
        reference_id, reference_sha = _stanc_identity(cmdstan_stanc)
        if reference_id != stanc_build_id or reference_sha != stanc_sha256:
            problems.append("CmdStan and stanli stanc binaries differ")
    if problems:
        raise OracleError("reference version mismatch: " + "; ".join(problems))

    compiler_result = _run((compiler, "--version"))
    if compiler_result.returncode:
        raise OracleError(f"compiler identity failed for {compiler}")
    compiler_id = (compiler_result.stdout
                   or compiler_result.stderr).splitlines()[0]
    assignments = _make_assignments(cmdstan, stanc, compiler)
    flags = tuple(_make_value(bridgestan, assignments, name)
                  for name in ("CXXFLAGS", "CPPFLAGS", "LDLIBS"))
    return ToolchainVersions(
        _git_head(cmdstan), actual_stan, actual_math, stanc_build_id,
        stanc_sha256, _git_head(bridgestan), _bridgestan_version(bridgestan),
        compiler_id, flags)


_PREPARE_LOCK = threading.Lock()
_PREPARED = set()


def prepare_reference_runtime(cmdstan: pathlib.Path, stanc: pathlib.Path,
                              deps: pathlib.Path,
                              versions: ToolchainVersions,
                              compiler: str = "clang++") -> None:
    """Build BridgeStan's shared adapter once before parallel model builds."""
    bridgestan = pathlib.Path(deps).resolve() / "bridgestan"
    assignments = _make_assignments(cmdstan, stanc, compiler)
    identity = hashlib.sha256(json.dumps(
        {"versions": versions.to_dict(), "assignments": assignments},
        sort_keys=True).encode("utf-8")).hexdigest()
    with _PREPARE_LOCK:
        if identity in _PREPARED:
            return
        metadata = bridgestan / ".stanli-conformance-runtime.json"
        adapter = bridgestan / "src" / "bridgestan.o"
        recorded = metadata.read_text(encoding="utf-8").strip() \
            if metadata.exists() else ""
        if recorded != identity:
            adapter.unlink(missing_ok=True)
        result = _run(("make", "-C", bridgestan, *assignments, adapter),
                      timeout=900.0)
        if result.returncode:
            raise OracleError("BridgeStan runtime build failed: "
                              + (result.stderr or result.stdout)[-4000:])
        metadata.write_text(identity + "\n", encoding="utf-8")
        _PREPARED.add(identity)


def cached_reference_build(
        source_bytes: bytes, source_name: str, cache_root: pathlib.Path,
        namespace: str, cache_inputs: Mapping[str, object],
        cmdstan: pathlib.Path, stanc: pathlib.Path, deps: pathlib.Path,
        versions: ToolchainVersions, compiler: str = "clang++") \
        -> ReferenceBuild:
    """Compile one model through official BridgeStan, or return its cache."""
    source_sha = hashlib.sha256(source_bytes).hexdigest()
    payload = {"schema": 1, "source_sha256": source_sha,
               "versions": versions.to_dict(), **dict(cache_inputs)}
    key = hashlib.sha256(json.dumps(
        payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
                         ).hexdigest()
    # Absolute, unconditionally: the library path below is handed to
    # `make -C deps/bridgestan`, and -C makes a relative target resolve
    # inside the bridgestan tree, where no generated source exists. make
    # then reports "No rule to make target" for every shard -- which is
    # exactly what the nightly did from its first run, since the workflow
    # passes `--resume conformance-cache`.
    workdir = pathlib.Path(cache_root).resolve() / namespace / key
    source = workdir / source_name
    library = source.with_name(source.stem + "_model.so")
    metadata = workdir / "build-metadata.json"
    if metadata.exists() and source.exists() and library.exists():
        try:
            recorded = json.loads(metadata.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            recorded = {}
        if recorded.get("cache_key") == key:
            return ReferenceBuild(library, None, source, source_sha, 0.0, True)

    started = time.monotonic()
    try:
        workdir.mkdir(parents=True, exist_ok=True)
        source.write_bytes(source_bytes)
        header = source.with_suffix(".hpp")
        # --O1, because that is the only MIR stanli ever consumes. stanc3
        # rewrites `c + w*x` into fma at that level, so without it the two
        # sides compile different arithmetic and differ by one rounding per
        # accumulation. That is invisible in a well-scaled result and large
        # in ULP once a probe's weighted sum cancels toward zero, where it
        # was being reported as a semantic mismatch. Passed here rather
        # than through STANCFLAGS because this call bypasses make.
        translated = _run((stanc, "--O1", source, f"--o={header}"),
                          timeout=180.0)
        if translated.returncode:
            detail = (translated.stderr or translated.stdout).strip()[-4000:]
            return ReferenceBuild(None, "stanc_fail: " + detail, source,
                                  source_sha, time.monotonic() - started)
        bridgestan = pathlib.Path(deps).resolve() / "bridgestan"
        assignments = _make_assignments(cmdstan, stanc, compiler)
        built = _run(("make", "-C", bridgestan, *assignments, library),
                     timeout=1800.0)
        if built.returncode:
            detail = (built.stderr or built.stdout).strip()[-4000:]
            return ReferenceBuild(None, "ref_build_fail: " + detail, source,
                                  source_sha, time.monotonic() - started)
        metadata.write_text(json.dumps(
            {"cache_key": key, "inputs": payload}, indent=2,
            sort_keys=True) + "\n", encoding="utf-8")
        return ReferenceBuild(library, None, source, source_sha,
                              time.monotonic() - started)
    except Exception as exc:
        return ReferenceBuild(None, f"reference_build_exception: {exc}",
                              source, source_sha,
                              time.monotonic() - started)
