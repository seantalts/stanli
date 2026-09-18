#!/usr/bin/env bash
# js_of_ocaml 6.4.1 breaks the line after `static` in a class body, and
# Safari 17 reads `static` alone on a line as a field named `static`
# (ocsigen/js_of_ocaml#2421). Delete the perl line once the pinned
# js_of_ocaml has that fix; keep the check.
set -euo pipefail
: "${1:?javascript file}"
for f in "$@"; do
  perl -0pi -e 's/\bstatic[ \t]*\n/static /g' "$f"
  if grep -nE '(^|[^A-Za-z0-9_$])static[[:blank:]]*$' "$f"; then
    echo "$f: line break after static" >&2
    exit 1
  fi
done
