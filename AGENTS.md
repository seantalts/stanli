# Start from current upstream

Before implementation, builds, tests, or delegation, inspect the worktree and
any integration in progress, then run `git fetch origin` and
`git remote set-head origin --auto`. Synchronize the task branch with the
fetched `origin/HEAD`; never assume local `main` or a newly created worktree is
current. Start new task branches/worktrees explicitly from `origin/HEAD`.

For existing work, fast-forward if possible or merge the remote default branch
while preserving task changes. Verify
`git merge-base --is-ancestor origin/HEAD HEAD` before substantive work, and
report the base SHA or any integration problem. Never discard dirty files,
reset away commits, rewrite shared history, or alter another active worktree.
The lead agent owns synchronization in shared worktrees; delegated agents
verify the agreed base rather than racing to merge it.

An explicitly requested historical revision, PR head, or benchmark baseline
must remain at that revision; fetch and report divergence instead. If fetching
or safe integration is blocked, report it rather than silently using stale
code. Fetch again before PR creation/merge, integrate new upstream changes,
and rerun affected checks.
