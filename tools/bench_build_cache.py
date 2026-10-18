"""Content-addressed directory cache for corpus benchmark build products."""
import hashlib
import json

VERSION = 1


def cache_key(context, stan_sha256):
    payload = dict(version=VERSION, context=context, stan_sha256=stan_sha256)
    return hashlib.sha256(json.dumps(payload, sort_keys=True, allow_nan=False).encode()).hexdigest()
