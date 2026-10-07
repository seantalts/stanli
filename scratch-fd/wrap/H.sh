#!/bin/bash
exec /usr/bin/env STANLI_FAST_REDUCE=0 STANLI_FAST_NOCHECK=0 STANLI_FAST_CHECK=1 /Users/xitrium/claud/stanrt/.worktrees/fast-density/scratch-fd/bin/B2_stanli_check "$@"
