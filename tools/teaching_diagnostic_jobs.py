#!/usr/bin/env python3
"""Compatibility entry point for archived diagnostic commands."""
import json
import pathlib
import sys
from corpus_diagnostic_jobs import jobs_for

if __name__ == '__main__':
    directory, output = map(pathlib.Path, sys.argv[1:])
    output.write_text(json.dumps(jobs_for(directory), indent=2) + '\n')
