#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/stanc_embed/provenance.sh
configured_version=$(stanc_embed_read_setup OCAML_VERSION)
switch=$(stanc_embed_read_setup OPAM_SWITCH)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/stanli-cache-probe.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT
export STANLI_TEST_SWITCH="$switch"
export STANLI_TEST_REMOVAL="$test_dir/removal"
cat > "$test_dir/opam" <<'MOCK'
#!/usr/bin/env bash
case "$1:$2" in
  switch:list) printf '%s\r\n' "$STANLI_TEST_SWITCH" ;;
  switch:remove) printf '%s\n' "$*" > "$STANLI_TEST_REMOVAL" ;;
  exec:*)
    case "$STANLI_TEST_PROBE" in
      failed) echo 'mock missing runtime DLL' >&2; exit 53 ;;
      empty) exit 0 ;;
      *) printf '%s\r\n' "$STANLI_TEST_PROBE" ;;
    esac ;;
  *) exit 99 ;;
esac
MOCK
chmod +x "$test_dir/opam"
export PATH="$test_dir:$PATH"
export STANLI_TEST_PROBE="$configured_version"
bash tools/stanc_embed/validate_cached_switch.sh
test ! -e "$STANLI_TEST_REMOVAL"
for STANLI_TEST_PROBE in failed empty; do
  export STANLI_TEST_PROBE
  if bash tools/stanc_embed/validate_cached_switch.sh > "$test_dir/output" 2>&1; then
    echo "Invalid probe $STANLI_TEST_PROBE was accepted" >&2
    exit 1
  fi
  test ! -e "$STANLI_TEST_REMOVAL"
  grep -Fq 'preserving' "$test_dir/output"
  if [[ "$STANLI_TEST_PROBE" == failed ]]; then
    grep -Fq 'mock missing runtime DLL' "$test_dir/output"
    grep -Fq 'exit 53' "$test_dir/output"
  fi
done
export STANLI_TEST_PROBE=0.0.0
bash tools/stanc_embed/validate_cached_switch.sh
grep -Fxq "switch remove $switch --yes" "$STANLI_TEST_REMOVAL"
echo 'Cached OCaml validation tests passed'
