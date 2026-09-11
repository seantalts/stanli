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
  12) msys_env=clangarm64; msys_package_prefix=mingw-w64-clang-aarch64 ;; # ARM64
  9) msys_env=ucrt64; msys_package_prefix=mingw-w64-ucrt-x86_64 ;;        # x64
  *) echo "Unsupported Windows architecture" >&2; exit 1 ;;
esac

if have pacman; then
  msys_root=$(cd "$(dirname "$(command -v pacman)")/../.." && pwd)
else
  msys_root=$(cygpath -u 'C:/msys64')
fi
[[ -x "$msys_root/usr/bin/pacman.exe" ]] || windows_install MSYS2.MSYS2

have opam || windows_install OCaml.opam
export PATH="$msys_root/$msys_env/bin:$msys_root/usr/bin:$PATH"
for tool in git make; do
  have "$tool" || pacman -S --needed --noconfirm "$tool"
done
for tool in clang cmake python; do
  have "$tool" || pacman -S --needed --noconfirm "$msys_package_prefix-$tool"
done
