"""Committed evaluation points and gradient counts for the corpus benchmark.

Each model has one entry, recorded from a Stanli NUTS reference run on the
benchmark data. A reference_run entry holds ten points taken from that run and
the number of gradient evaluations the run took. A fixed entry says why the run
could not supply points: the benchmark then uses the fixed points built into the
timing drivers and the default gradient count.
"""

import gzip
import hashlib
import io
import json
import math
import pathlib
import re

SCHEMA = 2
DEFAULT_GRADIENTS = 5000
DEFAULT_PATH = pathlib.Path(__file__).resolve().parent.parent / "docs/internal/artifacts/benchmark-points.json.gz"
SOURCES = ("reference_run", "fixed")
_HEX = re.compile(r"[0-9a-f]{64}")
_PHASES = ("warmup", "sampling")


def sha256(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def select_iterations(leapfrogs, points=10):
    """Systematic selection at cumulative-leapfrog quantiles (k + 0.5) / points.

    Returns, for each k, the first iteration whose cumulative leapfrog count
    reaches that fraction of the total. The comparison is in integers.
    """
    total = sum(leapfrogs)
    if not leapfrogs or total <= 0:
        raise ValueError("benchmark points: the reference run did no leapfrog steps")
    chosen, cumulative, at = [], 0, 0
    for k in range(points):
        while True:
            reached = cumulative + leapfrogs[at]
            if 2 * points * reached >= (2 * k + 1) * total:
                break
            cumulative = reached
            at += 1
        chosen.append(at)
    return chosen


def _fail(message):
    raise ValueError(f"benchmark points: {message}")


def _count(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _check_phases(where, entry, key):
    block = entry.get(key)
    if not isinstance(block, dict) or not all(_count(block.get(phase)) for phase in _PHASES):
        _fail(f"{where}: {key} needs warmup and sampling counts")
    return block


def _check_statistics(where, entry):
    gradients = _check_phases(where, entry, "gradients")
    if not _count(gradients.get("total")) or gradients["total"] != gradients["warmup"] + gradients["sampling"]:
        _fail(f"{where}: gradients.total is not warmup + sampling")
    _check_phases(where, entry, "divergences")
    _check_phases(where, entry, "max_treedepth")


def _validate(doc):
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        _fail(f"unsupported schema (expected {SCHEMA})")
    settings, models = doc.get("settings"), doc.get("models")
    if not isinstance(settings, dict) or not isinstance(models, dict):
        _fail("settings and models are required")
    count, total = settings.get("points"), settings.get("warmup", 0) + settings.get("draws", 0)
    if not _count(count) or count < 1:
        _fail("settings.points must be a positive integer")
    for name, entry in models.items():
        if entry.get("source") not in SOURCES:
            _fail(f"{name}: source must be one of {SOURCES}")
        for key in ("stan_sha256", "data_sha256"):
            if not isinstance(entry.get(key), str) or not _HEX.fullmatch(entry[key]):
                _fail(f"{name}: {key} is not a sha256 digest")
        if entry["source"] == "fixed":
            if not isinstance(entry.get("reason"), str) or not entry["reason"]:
                _fail(f"{name}: a fixed entry needs a reason")
            if "points" in entry or "iterations" in entry:
                _fail(f"{name}: a fixed entry has no points")
            if "gradients" in entry:
                _check_statistics(name, entry)
            continue
        n, points, iterations = entry.get("n_params"), entry.get("points"), entry.get("iterations")
        if not _count(n) or n < 1:
            _fail(f"{name}: n_params must be a positive integer")
        if not isinstance(points, list) or len(points) != count:
            _fail(f"{name}: expected {count} points")
        for k, point in enumerate(points):
            if (not isinstance(point, list) or len(point) != n
                    or not all(isinstance(v, (int, float)) and not isinstance(v, bool)
                               and math.isfinite(v) for v in point)):
                _fail(f"{name}: point {k} is not {n} finite numbers")
        if (not isinstance(iterations, list) or len(iterations) != count
                or not all(_count(i) and i < total for i in iterations)
                or iterations != sorted(iterations)):
            _fail(f"{name}: iterations must be {count} increasing indices below {total}")
        _check_statistics(name, entry)
    return doc


def dumps(doc):
    return json.dumps(doc, sort_keys=True, separators=(",", ":"), allow_nan=False).encode() + b"\n"


def write(path, doc):
    _validate(doc)
    buffer = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buffer, mtime=0, compresslevel=9) as stream:
        stream.write(dumps(doc))
    pathlib.Path(path).write_bytes(buffer.getvalue())


def load(path):
    with gzip.open(path, "rb") as stream:
        return _validate(json.loads(stream.read()))


def entry_for(doc, name, stan_sha256, data_sha256):
    """The model's entry, after checking that its stan and data hashes still match."""
    entry = doc["models"].get(name)
    if entry is None:
        _fail(f"{name}: no entry; regenerate with tools/gen_benchmark_points.py")
    if entry["stan_sha256"] != stan_sha256:
        _fail(f"{name}: stan source changed since the points were generated")
    if entry["data_sha256"] != data_sha256:
        _fail(f"{name}: data changed since the points were generated")
    return entry


def points_of(entry):
    return entry["points"] if entry["source"] == "reference_run" else None


def gradient_count(entry):
    return entry["gradients"]["total"] if entry["source"] == "reference_run" else DEFAULT_GRADIENTS
