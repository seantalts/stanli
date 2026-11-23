# Corpus status

The table of every corpus model is now in
[TESTING.md](../TESTING.md#every-model). For each model it shows where the
model comes from, how accurate CmdStan and stanli are on it, and how far
stanli may be from CmdStan.

`tools/gen_docs.py` writes that table, and CI fails when it is out of date.
