#!/bin/bash
set -e
R=/Users/xitrium/claud/stanrt/.worktrees/fast-density
cd $R
cmake -B build-fd -DCMAKE_BUILD_TYPE=Release -DSTANLI_STAN_DENSITY_ORACLE=OFF \
  -DSTANLI_STANC_EMBED_OBJ=$R/deps/stanc3/stanc_embed.o \
  -DSTANLI_OCAML_STDLIB=/Users/xitrium/.opam/stanc3-55/lib/ocaml "$@"
