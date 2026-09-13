#!/usr/bin/env bash
# Shared provenance helpers for the native, browser, and Windows stanc
# artifacts.
# This file is sourced by build scripts; it is not intended to be run.

_stanli_embed_repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

stanc_embed_read_setup() {
  local key=${1:?setup key}
  sed -n "s/^${key}=\\([^ ]*\\).*/\\1/p" \
    "$_stanli_embed_repo_root/tools/dev_setup.sh"
}

_stanc_embed_sha256_stream() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 | awk '{print $1}'
  else
    python3 -c 'import hashlib, sys; print(hashlib.sha256(sys.stdin.buffer.read()).hexdigest())'
  fi
}

_stanc_embed_sha256_file() {
  local path=${1:?file to hash}
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
  else
    python3 -c \
      'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' \
      "$path"
  fi
}

_stanc_embed_switch_exists() {
  local opam_switch=${1:?opam switch}
  command -v opam >/dev/null 2>&1 &&
    opam switch list --color=never --short 2>/dev/null |
      tr -d '\r' |
      grep -Fqx "$opam_switch"
}

_stanc_embed_ocaml_version() {
  local opam_switch=${1:?opam switch}
  local version
  version=$(opam exec --switch="$opam_switch" -- ocamlc -version) || return $?
  printf '%s\n' "$version" | tr -d '\r'
}

_stanc_embed_ocaml_target() {
  local opam_switch=${1:?opam switch}
  opam exec --switch="$opam_switch" -- ocamlc -config-var target | tr -d '\r'
}

_stanc_embed_package_version() {
  local opam_switch=${1:?opam switch}
  local package=${2:?opam package}
  opam list --color=never --switch="$opam_switch" --installed --short \
    --columns=version "$package" 2>/dev/null | tr -d '\r'
}

_stanc_embed_stamp_value() {
  local stamp=${1:?provenance stamp}
  local key=${2:?provenance key}
  sed -n "s/^${key}=//p" "$stamp"
}

# Hash both the sorted relative names and contents of every producer input.
# A newly added encoder module or a change to the build recipe therefore
# invalidates an old complete object without another manually maintained list.
stanc_embed_inputs_sha256() {
  (
    cd "$_stanli_embed_repo_root"
    while IFS= read -r input; do
      printf '%s\n%s\n' "$input" "$(_stanc_embed_sha256_file "$input")"
    done < <(
      find compiler/native compiler/ocaml tools/stanc_embed \
        -maxdepth 1 -type f -print |
        LC_ALL=C sort
    )
  ) | _stanc_embed_sha256_stream
}

stanc_embed_expected_stamp() {
  local src_sha=${1:-$(stanc_embed_read_setup STANC3_SRC_SHA)}
  local opam_switch=${2:-$(stanc_embed_read_setup OPAM_SWITCH)}
  printf '%s\n' \
    'format=stanli-stanc-embed-v3' \
    "stanc3_src_sha=$src_sha" \
    "producer_inputs_sha256=$(stanc_embed_inputs_sha256)" \
    "ocaml_version=$(_stanc_embed_ocaml_version "$opam_switch")" \
    "ocaml_target=$(_stanc_embed_ocaml_target "$opam_switch")" \
    "dune_version=$(_stanc_embed_package_version "$opam_switch" dune)"
}

stanc_embed_artifact_matches() {
  local object=${1:?embedded object}
  local src_sha=${2:-$(stanc_embed_read_setup STANC3_SRC_SHA)}
  local opam_switch=${3:-$(stanc_embed_read_setup OPAM_SWITCH)}
  local stamp="${object}.stamp"
  [[ -f "$object" && -f "$stamp" ]] || return 1
  if _stanc_embed_switch_exists "$opam_switch"; then
    [[ "$(cat "$stamp")" == \
       "$(stanc_embed_expected_stamp "$src_sha" "$opam_switch")" ]]
    return
  fi

  # A CI cache hit deliberately skips the entire opam installation. In that
  # case validate every source-derived field and the configured OCaml version,
  # while requiring the producing target and Dune version to be recorded.
  [[ "$(_stanc_embed_stamp_value "$stamp" format)" == \
       'stanli-stanc-embed-v3' ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" stanc3_src_sha)" == "$src_sha" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" producer_inputs_sha256)" == \
       "$(stanc_embed_inputs_sha256)" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" ocaml_version)" == \
       "$(stanc_embed_read_setup OCAML_VERSION)" ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" ocaml_target)" ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" dune_version)" ]]
}

stancjs_expected_stamp() {
  local src_repo=${1:?stanc3 source repository}
  local src_sha=${2:?stanc3 source revision}
  local opam_switch=${3:?opam switch}
  local jsoo_version
  jsoo_version=$(_stanc_embed_package_version "$opam_switch" js_of_ocaml)
  printf '%s\n' \
    'format=stanli-stancjs-v3' \
    "stanc3_src_repo=$src_repo" \
    "stanc3_src_sha=$src_sha" \
    "opam_switch=$opam_switch" \
    "ocaml_version=$(_stanc_embed_ocaml_version "$opam_switch")" \
    "ocaml_target=$(_stanc_embed_ocaml_target "$opam_switch")" \
    "dune_version=$(_stanc_embed_package_version "$opam_switch" dune)" \
    "js_of_ocaml_version=$jsoo_version" \
    'dune_profile=release' \
    'dune_subst=1'
}

stancjs_artifact_matches() {
  local artifact=${1:?stancjs artifact}
  local src_repo=${2:?stanc3 source repository}
  local src_sha=${3:?stanc3 source revision}
  local opam_switch=${4:?opam switch}
  local stamp="${artifact}.stamp"
  [[ -f "$artifact" && -f "$stamp" ]] || return 1
  if _stanc_embed_switch_exists "$opam_switch"; then
    [[ "$(cat "$stamp")" == \
       "$(stancjs_expected_stamp "$src_repo" "$src_sha" "$opam_switch")" ]]
    return
  fi

  [[ "$(_stanc_embed_stamp_value "$stamp" format)" == \
       'stanli-stancjs-v3' ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" stanc3_src_repo)" == \
       "$src_repo" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" stanc3_src_sha)" == \
       "$src_sha" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" opam_switch)" == \
       "$opam_switch" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" ocaml_version)" == \
       "$(stanc_embed_read_setup OCAML_VERSION)" ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" ocaml_target)" ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" dune_version)" ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" js_of_ocaml_version)" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" dune_profile)" == release ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" dune_subst)" == 1 ]]
}

# What the portable JavaScript compiler is built from: the overlay sources
# and the scripts build_web.sh runs. The native embed's build scripts live
# in the same directory but never feed this artifact, so they are listed
# out, as [stanli_windows_cli_inputs] does for the executable: editing the
# native build must not invalidate a JS artifact it never touched. A new
# script build_web.sh starts running belongs in this list.
stanli_stancjs_inputs() {
  (
    cd "$_stanli_embed_repo_root"
    {
      find compiler/js compiler/ocaml -maxdepth 1 -type f -print
      printf '%s\n' \
        tools/stanc_embed/fix_static_newline.sh \
        tools/stanc_embed/install_overlay.sh \
        tools/stanc_embed/provenance.sh
    } | LC_ALL=C sort
  )
}

stanli_stancjs_inputs_sha256() {
  (
    cd "$_stanli_embed_repo_root"
    while IFS= read -r input; do
      printf '%s\n%s\n' "$input" "$(_stanc_embed_sha256_file "$input")"
    done < <(stanli_stancjs_inputs)
  ) | _stanc_embed_sha256_stream
}

stanli_stancjs_expected_stamp() {
  local src_repo=${1:?stanc3 source repository}
  local src_sha=${2:?stanc3 source revision}
  local opam_switch=${3:?opam switch}
  local jsoo_version
  jsoo_version=$(_stanc_embed_package_version "$opam_switch" js_of_ocaml)
  printf '%s\n' \
    'format=stanli-portable-stancjs-v3' \
    "stanc3_src_repo=$src_repo" \
    "stanc3_src_sha=$src_sha" \
    "opam_switch=$opam_switch" \
    "ocaml_version=$(_stanc_embed_ocaml_version "$opam_switch")" \
    "ocaml_target=$(_stanc_embed_ocaml_target "$opam_switch")" \
    "dune_version=$(_stanc_embed_package_version "$opam_switch" dune)" \
    "js_of_ocaml_version=$jsoo_version" \
    "producer_inputs_sha256=$(stanli_stancjs_inputs_sha256)" \
    'dune_profile=release' \
    'dune_subst=1'
}

stanli_stancjs_artifact_matches() {
  local artifact=${1:?portable stancjs artifact}
  local src_repo=${2:?stanc3 source repository}
  local src_sha=${3:?stanc3 source revision}
  local opam_switch=${4:?opam switch}
  local stamp="${artifact}.stamp"
  [[ -f "$artifact" && -f "$stamp" ]] || return 1
  if _stanc_embed_switch_exists "$opam_switch"; then
    [[ "$(cat "$stamp")" == \
       "$(stanli_stancjs_expected_stamp \
          "$src_repo" "$src_sha" "$opam_switch")" ]]
    return
  fi

  [[ "$(_stanc_embed_stamp_value "$stamp" format)" == \
       'stanli-portable-stancjs-v3' ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" stanc3_src_repo)" == \
       "$src_repo" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" stanc3_src_sha)" == \
       "$src_sha" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" opam_switch)" == \
       "$opam_switch" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" ocaml_version)" == \
       "$(stanc_embed_read_setup OCAML_VERSION)" ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" ocaml_target)" ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" dune_version)" ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" js_of_ocaml_version)" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" producer_inputs_sha256)" == \
       "$(stanli_stancjs_inputs_sha256)" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" dune_profile)" == release ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" dune_subst)" == 1 ]]
}

# The Windows executable uses the same portable-MIR package as the native and
# JavaScript producers, but only the small command-line entry point from the JS
# directory. Keep this narrower than [stanli_stancjs_inputs_sha256]: changing
# the browser wrapper must not change the provenance of an executable that
# does not link it.
stanli_windows_cli_inputs() {
  (
    cd "$_stanli_embed_repo_root"
    {
      find compiler/ocaml -maxdepth 1 -type f -print
      printf '%s\n' \
        .gitattributes \
        compiler/js/dune \
        compiler/js/stanli_compiler_cli.ml \
        tools/stanc_embed/install_overlay.sh \
        tools/stanc_embed/provenance.sh
    } | LC_ALL=C sort
  )
}

stanli_windows_cli_inputs_sha256() {
  (
    cd "$_stanli_embed_repo_root"
    while IFS= read -r input; do
      printf '%s\n%s\n' "$input" "$(_stanc_embed_sha256_file "$input")"
    done < <(stanli_windows_cli_inputs)
  ) | _stanc_embed_sha256_stream
}

stanli_windows_cli_expected_stamp() {
  local src_repo=${1:?stanc3 source repository}
  local src_sha=${2:?stanc3 source revision}
  local opam_switch=${3:-$(opam switch show --safe)}
  local configured_version ocaml_version ocaml_meta_version ocaml_target
  local dune_version metadata_ok=1
  configured_version=$(stanc_embed_read_setup OCAML_VERSION)
  # ocaml-windows64 is the compiler package. Its opam recipe passes the
  # conf-gcc-windows64 host value directly to OCaml's --target option; those
  # two package fields are therefore the provenance inputs Dune selected.
  ocaml_version=$(
    _stanc_embed_package_version "$opam_switch" ocaml-windows64)
  ocaml_meta_version=$(
    _stanc_embed_package_version "$opam_switch" ocaml-windows)
  ocaml_target=$(opam var --color=never --switch="$opam_switch" \
    conf-gcc-windows64:host 2>/dev/null | tr -d '\r' || true)
  dune_version=$(_stanc_embed_package_version "$opam_switch" dune)

  printf '%s\n' \
    "Windows compiler metadata: ocaml-windows64=${ocaml_version:-<missing>}" \
    "Windows compiler metadata: ocaml-windows=${ocaml_meta_version:-<missing>}" \
    "Windows compiler metadata: conf-gcc-windows64:host=${ocaml_target:-<missing>}" \
    "Windows compiler metadata: dune=${dune_version:-<missing>}" >&2
  if [[ -z "$ocaml_version" ]]; then
    echo "Windows compiler metadata: ocaml-windows64 is not installed" >&2
    metadata_ok=0
  elif [[ "$ocaml_version" != "$configured_version" ]]; then
    echo "Windows compiler metadata: ocaml-windows64 must be $configured_version" >&2
    metadata_ok=0
  fi
  if [[ -z "$ocaml_meta_version" ]]; then
    echo "Windows compiler metadata: ocaml-windows is not installed" >&2
    metadata_ok=0
  elif [[ "$ocaml_meta_version" != "$ocaml_version" ]]; then
    echo "Windows compiler metadata: OCaml package versions differ" >&2
    metadata_ok=0
  fi
  if [[ -z "$ocaml_target" ]]; then
    echo "Windows compiler metadata: 64-bit target is unavailable" >&2
    metadata_ok=0
  elif [[ "$ocaml_target" != x86_64-w64-mingw32 ]]; then
    echo "Windows compiler metadata: unexpected 64-bit target" >&2
    metadata_ok=0
  fi
  if [[ -z "$dune_version" ]]; then
    echo "Windows compiler metadata: dune is not installed" >&2
    metadata_ok=0
  fi
  if [[ "$metadata_ok" != 1 ]]; then
    return 1
  fi
  printf '%s\n' \
    'format=stanli-portable-windows-cli-v2' \
    "stanc3_src_repo=$src_repo" \
    "stanc3_src_sha=$src_sha" \
    "ocaml_version=$ocaml_version" \
    "ocaml_target=$ocaml_target" \
    "dune_version=$dune_version" \
    "producer_inputs_sha256=$(stanli_windows_cli_inputs_sha256)" \
    'dune_context=windows' \
    'dune_profile=release' \
    'dune_subst=1'
}

stanli_windows_cli_artifact_matches() {
  local artifact=${1:?portable Windows compiler artifact}
  local src_repo=${2:?stanc3 source repository}
  local src_sha=${3:?stanc3 source revision}
  local opam_switch=${4:-}
  local stamp="${artifact}.stamp"
  [[ -f "$artifact" && -f "$stamp" ]] || return 1

  # On the Linux producer, compare the complete record against the installed
  # cross toolchain. The Windows packager intentionally has no opam switch, so
  # it validates every source-derived and configured field instead.
  if [[ -z "$opam_switch" ]] && command -v opam >/dev/null 2>&1; then
    opam_switch=$(opam switch show --color=never --safe 2>/dev/null || true)
  fi
  if [[ -n "$opam_switch" ]] && _stanc_embed_switch_exists "$opam_switch" &&
     [[ -n "$(_stanc_embed_package_version "$opam_switch" ocaml-windows)" ]]; then
    [[ "$(cat "$stamp")" == \
       "$(stanli_windows_cli_expected_stamp \
          "$src_repo" "$src_sha" "$opam_switch")" ]]
    return
  fi

  [[ "$(_stanc_embed_stamp_value "$stamp" format)" == \
       'stanli-portable-windows-cli-v2' ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" stanc3_src_repo)" == \
       "$src_repo" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" stanc3_src_sha)" == \
       "$src_sha" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" ocaml_version)" == \
       "$(stanc_embed_read_setup OCAML_VERSION)" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" ocaml_target)" == \
       'x86_64-w64-mingw32' ]] &&
    [[ -n "$(_stanc_embed_stamp_value "$stamp" dune_version)" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" producer_inputs_sha256)" == \
       "$(stanli_windows_cli_inputs_sha256)" ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" dune_context)" == windows ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" dune_profile)" == release ]] &&
    [[ "$(_stanc_embed_stamp_value "$stamp" dune_subst)" == 1 ]]
}
