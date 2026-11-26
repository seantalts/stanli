# usage: python3 r5_make_data.py N DISTINCT OUT.json
# N rows, of which the first DISTINCT are distinct and the rest repeat them.
import json, sys
N, d, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
base = [(1 + i % 4, float(i % 2), 0.1 * i - 1.0) for i in range(d)]
rows = [base[i % d] for i in range(N)]
json.dump({"N": N, "J": 4, "g": [r[0] for r in rows], "x": [r[1] for r in rows],
           "y": [r[2] for r in rows]}, open(out, "w"))
