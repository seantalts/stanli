#!/bin/bash
cd /Users/xitrium/claud/stanrt/.worktrees/fast-density
B=$PWD/scratch-fd/bin
ARMS=(--arm A=$B/A_bench_grad,STANLI_FAST_REDUCE=0,STANLI_FAST_NOCHECK=0
  --arm B=$B/B_bench_grad,STANLI_FAST_REDUCE=0,STANLI_FAST_NOCHECK=0
  --arm C=$B/B_bench_grad,STANLI_FAST_REDUCE=1,STANLI_FAST_NOCHECK=0
  --arm D=$B/B_bench_grad,STANLI_FAST_REDUCE=0,STANLI_FAST_NOCHECK=1
  --arm E=$B/B_bench_grad,STANLI_FAST_REDUCE=1,STANLI_FAST_NOCHECK=1
  --arm F=$B/F_bench_grad,STANLI_FAST_REDUCE=0,STANLI_FAST_NOCHECK=0
  --arm G=$B/F_bench_grad,STANLI_FAST_REDUCE=1,STANLI_FAST_NOCHECK=1
  --arm Z=$B/Z_bench_grad,STANLI_FAST_REDUCE=0,STANLI_FAST_NOCHECK=0)
uptime > scratch-fd/results/uptime_starters.txt
python3 harnesses/ab_bench_corpus.py deps/posteriordb scratch-fd/results/starters "${ARMS[@]}" \
  --mir-dir /Users/xitrium/claud/stanli-sulong/models --rounds 24 --load-limit 5 > scratch-fd/results/starters.log 2>&1
uptime > scratch-fd/results/uptime_corpus.txt
python3 harnesses/ab_bench_corpus.py deps/posteriordb scratch-fd/results/corpus "${ARMS[@]}" \
  --models scratch-fd/results/models_affected.txt --rounds 8 --load-limit 5 > scratch-fd/results/corpus.log 2>&1
