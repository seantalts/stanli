# Incomplete ARM warmup attempt

[Run 34755561756](https://github.com/seantalts/stanli/actions/runs/34755561756),
source `9c938837`, stopped in process 70 (zero-based) after native execution
when the optional procfs collector encountered ESRCH reading a disappearing
worker. The last native snapshot matches, but post-native Python host checks
did not finish. This is not a complete correctness or performance scorecard.

The archive contains all 71 recorded processes, outputs, full snapshots,
frozen design schedule/calibration/identities and host files. No partial
performance ratio is promoted. The corrected collector retries ARM once with
the same design in run 34755878766; the completed x86 job is not rerun.

Archive SHA-256:
`2579d668e95e3f97b45486d9e7ee16e4c9d8025d64568eb3c693ee768cd8e5cf`.
