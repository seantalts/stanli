#!/usr/bin/env bash
# Regenerate the MIR fixtures from their .stan sources with stanli's compiler,
# through a built stanli_check (STANLI_CHECK, default build/stanli_check).
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/gen_fixtures.py --stanli-check "${STANLI_CHECK:-build/stanli_check}"
