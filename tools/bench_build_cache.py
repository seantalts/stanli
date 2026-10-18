"""Content-addressed directory cache for corpus benchmark build products."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

VERSION = 1
META = "meta.json"


def cache_key(context, stan_sha256):
    payload = dict(version=VERSION, context=context, stan_sha256=stan_sha256)
    return hashlib.sha256(json.dumps(payload, sort_keys=True, allow_nan=False).encode()).hexdigest()


class BuildCache:
    """Entries appear complete or not at all: they are built aside and renamed into place."""
    def __init__(self, root):
        self.root = Path(root)

    def entry(self, key):
        return self.root / key

    def lookup(self, key):
        directory = self.entry(key)
        try:
            meta = json.loads((directory / META).read_text())
            if all((directory / name).is_file() for name in meta["files"]):
                return meta
        except (OSError, ValueError, KeyError, TypeError):
            pass
        return None

    def scratch(self, key):
        self.root.mkdir(parents=True, exist_ok=True)
        return Path(tempfile.mkdtemp(prefix=f".tmp-{key[:16]}-", dir=self.root))

    def commit(self, scratch, key, meta):
        """Rename the finished directory into place; False means another writer won."""
        (scratch / META).write_text(json.dumps(meta, indent=2, sort_keys=True, allow_nan=False) + "\n")
        try:
            os.rename(scratch, self.entry(key))
        except OSError:
            if not self.entry(key).exists():
                raise
            self.discard(scratch)
            return False
        return True

    def discard(self, scratch):
        shutil.rmtree(scratch, ignore_errors=True)
