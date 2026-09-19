#!/usr/bin/env python3
"""Compatibility entry point for archived teaching-sweep commands."""
import json
import pathlib
import sys
from report_corpus import export

if __name__ == '__main__':
    output = pathlib.Path(sys.argv[3])
    print(json.dumps(export(*map(pathlib.Path, sys.argv[1:])), indent=2))
    for old, new in [('teaching-results.json', 'corpus-results.json'),
                     ('teaching-timings.csv', 'corpus-timings.csv')]:
        (output / old).write_bytes((output / new).read_bytes())
