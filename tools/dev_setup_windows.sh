#!/usr/bin/env bash
# Windows prerequisites, sourced by dev_setup.sh from Git Bash or MSYS2 Bash.

windows_refresh_path() {
  local installed_path
  installed_path=$(powershell.exe -NoProfile -NonInteractive -Command \
    '[Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")' | tr -d '\r')
  export PATH="$(cygpath -up "$installed_path"):$PATH"
}

windows_install() {
  winget.exe install --id "$1" --exact --source winget \
    --accept-package-agreements --accept-source-agreements
  windows_refresh_path
}

case "$(powershell.exe -NoProfile -NonInteractive -Command \
  '(Get-CimInstance Win32_Processor | Select-Object -First 1).Architecture' | tr -d '\r')" in
  12)
    if [ "$WANT_EMBED" = 1 ]; then
      echo "Embedding is unsupported on Windows ARM64: OCaml runs under x64 emulation and cannot build native ARM64 objects. Rerun with --no-embed." >&2
      exit 1
    fi
    msys_env=clangarm64; msys_package_prefix=mingw-w64-clang-aarch64
    ;;
  # Keep Clang, the linker, and the C++ exception runtime from one toolchain.
  9) msys_env=clang64; msys_package_prefix=mingw-w64-clang-x86_64 ;;      # x64
  *) echo "Unsupported Windows architecture" >&2; exit 1 ;;
esac

if have pacman; then
  msys_root=$(cd "$(dirname "$(command -v pacman)")/../.." && pwd)
else
  msys_root=$(cygpath -u 'C:/msys64')
fi
msys_root=${msys_root%/}
[[ -x "$msys_root/usr/bin/pacman.exe" ]] || windows_install MSYS2.MSYS2

# Git for Windows and MSYS2 ship different builds of the msys runtime. A
# process that one runtime starts directly from the other receives almost no
# environment (COMSPEC, TEMP, TMP, LOCALAPPDATA and the user's PATH are all
# missing), which breaks opam ("Exec format error") and dune. Everything below
# runs MSYS2 tools, so from Git Bash restart this script under MSYS2's bash.
# PowerShell is a native process, so it passes the whole environment along.
if [[ ! -x /usr/bin/pacman.exe && -x "$msys_root/usr/bin/bash.exe" ]]; then
  if [[ -n ${STANLI_SETUP_REEXEC:-} ]]; then
    echo "could not start dev_setup.sh under MSYS2's bash ($msys_root)" >&2
    exit 1
  fi
  step "restarting under MSYS2 bash ($(cygpath -m "$msys_root"))"
  STANLI_SETUP_REEXEC=1 MSYSTEM=${msys_env^^} \
  STANLI_SETUP_BASH=$(cygpath -w "$msys_root/usr/bin/bash.exe") \
  STANLI_SETUP_SCRIPT=$(cygpath -w "$REPO/tools/dev_setup.sh") \
  STANLI_SETUP_ARGS="$*" \
    powershell.exe -NoProfile -NonInteractive -Command \
      '& $env:STANLI_SETUP_BASH $env:STANLI_SETUP_SCRIPT @($env:STANLI_SETUP_ARGS -split " " | Where-Object { $_ }); exit $LASTEXITCODE'
  exit 0
fi

# winget puts opam on the user PATH, which an older shell may not have yet.
have opam || windows_refresh_path
have opam || windows_install OCaml.opam
export PATH="$msys_root/$msys_env/bin:$msys_root/usr/bin:$PATH"
for tool in git make; do
  have "$tool" || pacman -S --needed --noconfirm "$tool"
done
for tool in clang lld cmake python; do
  # An older UCRT64 or system installation must not satisfy this check.
  [ -x "$msys_root/$msys_env/bin/$tool.exe" ] ||
    pacman -S --needed --noconfirm "$msys_package_prefix-$tool"
done
