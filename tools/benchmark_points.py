"""Committed evaluation points for the gradient benchmark.

Models that declare `int prior_only;` in their data block are benchmarked at
draws from their own prior; every other model keeps the fixed points built into
the timing drivers.
"""

import gzip
import hashlib
import io
import json
import math
import pathlib
import re

SCHEMA = 1
DEFAULT_PATH = pathlib.Path(__file__).resolve().parent.parent / "docs/internal/artifacts/benchmark-points.json.gz"
_HEX = re.compile(r"[0-9a-f]{64}")
_PRIOR_ONLY = re.compile(r"\bint\s*(<[^>]*>)?\s+prior_only\s*;")


def sha256(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"(//|#)[^\n]*", " ", text)


def _data_block(text):
    for match in re.finditer(r"(?<![\w])data\s*\{", text):
        before = text[:match.start()].rstrip()
        if before.endswith("transformed"):
            continue
        depth, at = 1, match.end()
        while at < len(text) and depth:
            depth += {"{": 1, "}": -1}.get(text[at], 0)
            at += 1
        return text[match.end():at - 1]
    return ""


def prior_only_model(text):
    return bool(_PRIOR_ONLY.search(_data_block(_strip_comments(text))))


def _fail(message):
    raise ValueError(f"benchmark points: {message}")


def _check_hashes(where, entry):
    for key in ("stan_sha256", "data_sha256"):
        if not isinstance(entry.get(key), str) or not _HEX.fullmatch(entry[key]):
            _fail(f"{where}: {key} is not a sha256 digest")


def _validate(doc):
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        _fail(f"unsupported schema (expected {SCHEMA})")
    settings, models, fallbacks = doc.get("settings"), doc.get("models"), doc.get("fallbacks")
    if not isinstance(settings, dict) or not isinstance(models, dict) or not isinstance(fallbacks, dict):
        _fail("settings, models and fallbacks are required")
    count = len(settings.get("selected_draws", ()))
    if count < 1:
        _fail("settings.selected_draws is empty")
    for name, entry in models.items():
        if entry.get("source") != "prior":
            _fail(f"{name}: source must be 'prior'")
        _check_hashes(name, entry)
        n, points = entry.get("n_params"), entry.get("points")
        if not isinstance(n, int) or isinstance(n, bool) or n < 1:
            _fail(f"{name}: n_params must be a positive integer")
        if not isinstance(points, list) or len(points) != count:
            _fail(f"{name}: expected {count} points")
        for k, point in enumerate(points):
            if (not isinstance(point, list) or len(point) != n
                    or not all(isinstance(v, (int, float)) and not isinstance(v, bool)
                               and math.isfinite(v) for v in point)):
                _fail(f"{name}: point {k} is not {n} finite numbers")
    for name, entry in fallbacks.items():
        if name in models:
            _fail(f"{name}: listed as both prior and fallback")
        if not isinstance(entry.get("reason"), str) or not entry["reason"]:
            _fail(f"{name}: fallback needs a reason")
        _check_hashes(name, entry)
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


def points_for(doc, name, stan_sha256, data_sha256, source_text=None):
    """Vectors for a prior-point model, None for a fixed-point model.

    A model with a recorded entry whose stan or data hash differs raises; so
    does a prior-only model with no entry.
    """
    entry = doc["models"].get(name) or doc["fallbacks"].get(name)
    if entry is None:
        if source_text is not None and prior_only_model(source_text):
            _fail(f"{name}: declares prior_only but has no entry; regenerate with tools/gen_benchmark_points.py")
        return None
    if entry["stan_sha256"] != stan_sha256:
        _fail(f"{name}: stan source changed since the points were generated")
    if entry["data_sha256"] != data_sha256:
        _fail(f"{name}: data changed since the points were generated")
    return entry["points"] if name in doc["models"] else None
