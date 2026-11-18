#!/usr/bin/env python3
"""Delete superseded GitHub Actions cache entries.

Many cache keys end in a content hash, so every change to the hashed files
leaves the previous entry behind on main until LRU eviction removes it. That
eviction does not know which entries are current and has removed the main
Clang ccache that pull requests restore. This script deletes, on main, every
entry of a key family except the newest, and every entry that belongs to a
closed or merged pull request.

A family is the key without its trailing hex hash (and without the seed's
`-partial-<run id>` suffix). Keys without such a hash are never deleted. An
entry is kept when it is less than 30 minutes old, when a job read it after a
newer family member existed (two jobs share the prefix), or when its version
differs from the newest member's (a different set of paths).
"""
import argparse
from datetime import datetime, timedelta, timezone
import json
import os
import re
import subprocess
import sys

MAIN = "refs/heads/main"
GRACE = timedelta(minutes=30)
PARTIAL = re.compile(r"-partial-[0-9]+$")
HASH = re.compile(r"-[0-9a-f]{16,}$")
PULL = re.compile(r"^refs/pull/([0-9]+)/merge$")


def when(text):
    # Seconds are precise enough, and older Pythons reject 5-digit fractions.
    return datetime.strptime(text[:19], "%Y-%m-%dT%H:%M:%S").replace(
        tzinfo=timezone.utc)


def family(key):
    base = PARTIAL.sub("", key)
    stripped = HASH.sub("", base)
    return stripped if stripped != base else None


def plan(entries, pr_state, now):
    """Return (entry, reason) pairs to delete. pr_state maps a PR number to
    its state."""
    doomed = []
    groups = {}
    for item in entries:
        if item["ref"] == MAIN:
            name = family(item["key"])
            if name is not None:
                groups.setdefault((name, item["version"]), []).append(item)
        else:
            match = PULL.match(item["ref"])
            if match and pr_state(match[1]) == "closed":
                doomed.append((item, f"pull request {match[1]} is closed"))
    for members in groups.values():
        members.sort(key=lambda item: when(item["createdAt"]))
        newest = members[-1]
        for item in members[:-1]:
            if now - when(item["createdAt"]) < GRACE:
                continue
            if when(item["lastAccessedAt"]) > when(newest["createdAt"]):
                print(f"keep {item['id']} (read after newer entry) "
                      f"{item['key']}")
                continue
            doomed.append((item, f"superseded by {newest['id']}"))
    return doomed


def gh(*args):
    return subprocess.run(["gh", *args], check=True, capture_output=True,
                          text=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--entries", help="cache list JSON instead of gh")
    parser.add_argument("--pr-states", help="PR number -> state JSON")
    parser.add_argument("--now", help="ISO time instead of the clock")
    args = parser.parse_args()

    if args.entries:
        with open(args.entries) as f:
            entries = json.load(f)
    else:
        entries = json.loads(gh(
            "cache", "list", "-R", args.repo, "-L", "1000", "--json",
            "id,key,ref,version,createdAt,lastAccessedAt,sizeInBytes"))
    states = {}
    if args.pr_states:
        with open(args.pr_states) as f:
            states = json.load(f)

    def pr_state(number):
        if number not in states:
            try:
                states[number] = gh("api", f"repos/{args.repo}/pulls/{number}",
                                    "-q", ".state").strip()
            except subprocess.CalledProcessError as error:
                print(f"keep pull request {number}: {error.stderr.strip()}")
                states[number] = "unknown"
        return states[number]

    now = when(args.now) if args.now else datetime.now(timezone.utc)
    freed = 0
    for item, reason in plan(entries, pr_state, now):
        size = item["sizeInBytes"] / 1e6
        print(f"delete {item['id']} {size:.1f} MB {item['ref']} "
              f"{item['key']} ({reason})")
        if not args.dry_run:
            try:
                gh("cache", "delete", "-R", args.repo, str(item["id"]))
            except subprocess.CalledProcessError as error:
                print(f"  failed: {error.stderr.strip()}")
                continue
        freed += item["sizeInBytes"]
    verb = "would free" if args.dry_run else "freed"
    print(f"{verb} {freed / 1e6:.1f} MB")


if __name__ == "__main__":
    sys.exit(main())
