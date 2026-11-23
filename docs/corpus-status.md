# Corpus status

The table of every corpus model is in
[TESTING.md](../TESTING.md#every-model). For each model it gives the source,
the number of values compared with CmdStan, and the limit on the log density,
the gradient and the `write_array` outputs.

`tools/gen_docs.py` writes that table from the recorded CmdStan references and
[`tests/ulp_ledger.json`](../tests/ulp_ledger.json), and CI fails when the
table and those files disagree.
