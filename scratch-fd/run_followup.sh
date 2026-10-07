#!/bin/bash
cd /Users/xitrium/claud/stanrt/.worktrees/fast-density
B=$PWD/scratch-fd/bin
E() { echo "STANLI_FAST_REDUCE=$1,STANLI_FAST_NOCHECK=$2,STANLI_FAST_CHECK=$3"; }
R=scratch-fd/results
uptime > $R/uptime_confirm.txt
printf "ch12_m12_4\nlow_dim_gauss_mix_collapse\nch15_m15_4\n" > $R/confirm_models.txt
python3 harnesses/ab_bench_corpus.py deps/posteriordb $R/confirm \
  --arm A=$B/A_bench_grad,$(E 0 0 0) --arm B=$B/B_bench_grad,$(E 0 0 0) \
  --arm C=$B/B_bench_grad,$(E 1 0 0) --arm F=$B/F_bench_grad,$(E 0 0 0) \
  --arm Z=$B/Z_bench_grad,$(E 0 0 0) \
  --models $R/confirm_models.txt --rounds 24 --load-limit 5 > $R/confirm.log 2>&1
uptime > $R/uptime_v1acc.txt
python3 harnesses/ab_bench_corpus.py deps/posteriordb $R/v1acc \
  --arm B=$B/B_bench_grad,$(E 0 0 0) --arm C4=$B/B_bench_grad,$(E 1 0 0) \
  --arm C8=$B/B_bench_grad,$(E 2 0 0) --arm Z=$B/Z_bench_grad,$(E 0 0 0) \
  --models $R/models_len64.txt --rounds 8 --load-limit 5 > $R/v1acc.log 2>&1
uptime > $R/uptime_v3b_starters.txt
python3 harnesses/ab_bench_corpus.py deps/posteriordb $R/v3b_starters \
  --arm B2=$B/B2_bench_grad,$(E 0 0 0) --arm H=$B/B2_bench_grad,$(E 0 0 1) \
  --arm D2=$B/B2_bench_grad,$(E 0 1 0) --arm Y=$B/Y_bench_grad,$(E 0 0 0) \
  --mir-dir /Users/xitrium/claud/stanli-sulong/models --rounds 24 --load-limit 5 > $R/v3b_starters.log 2>&1
uptime > $R/uptime_v3b.txt
python3 harnesses/ab_bench_corpus.py deps/posteriordb $R/v3b \
  --arm B2=$B/B2_bench_grad,$(E 0 0 0) --arm H=$B/B2_bench_grad,$(E 0 0 1) \
  --arm D2=$B/B2_bench_grad,$(E 0 1 0) --arm Y=$B/Y_bench_grad,$(E 0 0 0) \
  --models $R/models_len16.txt --rounds 8 --load-limit 5 > $R/v3b.log 2>&1
