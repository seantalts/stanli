"""Check which Actions cache entries tools/prune_caches.py would delete."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
H1, H2, H3 = "1" * 64, "2" * 64, "3" * 64
MAIN = "refs/heads/main"
NOW = "2026-11-18T12:00:00Z"


def entry(id, key, created, ref=MAIN, accessed=None, version="v1"):
    return {"id": id, "key": key, "ref": ref, "version": version,
            "createdAt": created, "lastAccessedAt": accessed or created,
            "sizeInBytes": id * 1000000}


ENTRIES = [
    # Superseded family member: only the newest survives.
    entry(1, f"ccache-asan-{H3}-{H1}", "2026-11-17T20:00:00Z"),
    entry(2, f"ccache-asan-{H3}-{H2}", "2026-11-18T09:00:00Z"),
    # An older entry in its 30-minute grace period is kept.
    entry(3, f"ccache-mac-{H1}", "2026-11-18T11:40:00Z"),
    entry(4, f"ccache-mac-{H2}", "2026-11-18T11:50:00Z"),
    # Keys without a hex suffix are never deleted.
    entry(5, "emsdk-Linux-X64-4.0.8-v1", "2026-11-10T00:00:00Z"),
    entry(6, "emsdk-Linux-X64-6.0.6-v1", "2026-11-17T00:00:00Z"),
    entry(7, "pdb-lite-abc-2", "2026-11-10T00:00:00Z"),
    entry(8, "pdb-lite-abc-2", "2026-11-17T00:00:00Z"),
    # A different version is a different restorer, not a newer copy.
    entry(9, f"deps-Linux-{H1}", "2026-11-17T00:00:00Z", version="a"),
    entry(10, f"deps-Linux-{H1}", "2026-11-18T00:00:00Z", version="b"),
    # Read after its newer sibling existed: another job still uses it.
    entry(11, f"R-4.6.2-1-{H1}", "2026-11-17T00:00:00Z",
          accessed="2026-11-18T06:00:00Z"),
    entry(12, f"R-4.6.2-1-{H2}", "2026-11-18T05:00:00Z"),
    # A partial seed joins its family in both orders.
    entry(13, f"clang-v6-{H3}-{H1}", "2026-11-17T00:00:00Z"),
    entry(14, f"clang-v6-{H3}-{H2}-partial-19876543210",
          "2026-11-18T01:00:00Z"),
    entry(15, f"clang-v6-{H1}-{H1}-partial-19876543210",
          "2026-11-17T00:00:00Z"),
    entry(16, f"clang-v6-{H1}-{H2}", "2026-11-18T01:00:00Z"),
    # A decimal run id alone is not a hex suffix.
    entry(17, "win-setup-ocaml-44205670876", "2026-11-10T00:00:00Z"),
    entry(18, "win-setup-ocaml-44213147532", "2026-11-17T00:00:00Z"),
    # Pull-request entries go once the pull request is closed or merged.
    entry(19, f"ccache-asan-{H3}-{H1}", "2026-11-18T11:59:00Z",
          ref="refs/pull/568/merge"),
    entry(20, f"ccache-asan-{H3}-{H1}", "2026-11-18T00:00:00Z",
          ref="refs/pull/576/merge"),
    entry(21, "emsdk-Linux-X64-6.0.6-v1", "2026-11-18T00:00:00Z",
          ref="refs/pull/568/merge"),
    # Other branches and tags are left alone.
    entry(22, f"ccache-asan-{H3}-{H1}", "2026-11-01T00:00:00Z",
          ref="refs/heads/feature"),
]
PR_STATES = {"568": "closed", "576": "open"}


class PrunePlanTest(unittest.TestCase):
    def plan(self, *extra):
        with tempfile.TemporaryDirectory() as tmp:
            entries = Path(tmp) / "entries.json"
            states = Path(tmp) / "states.json"
            entries.write_text(json.dumps(ENTRIES))
            states.write_text(json.dumps(PR_STATES))
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/prune_caches.py"),
                 "--dry-run", "--entries", str(entries),
                 "--pr-states", str(states), "--now", NOW, *extra],
                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_deletes_exactly_the_superseded_and_closed_entries(self):
        deleted = {int(line.split()[1]) for line in self.plan().splitlines()
                   if line.startswith("delete ")}
        self.assertEqual(deleted, {1, 13, 15, 19, 21})

    def test_reports_the_total_freed(self):
        self.assertIn("would free 69.0 MB", self.plan())


if __name__ == "__main__":
    unittest.main()
