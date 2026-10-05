#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "Usage: $0 <solidarcad-build-deps-*.tar.zst> [install-root]" >&2
}

if [[ $# -lt 1 ]] || [[ $# -gt 2 ]]; then
  usage
  exit 2
fi

archive=$(realpath "$1")
install_root=${2:-"$HOME/.local/share/solidarcad-build-deps"}

if [[ ! -f "$archive" ]]; then
  echo "Package was not found: $archive" >&2
  exit 1
fi

if ! command -v zstd >/dev/null 2>&1; then
  echo "zstd is required to install the build-dependency package." >&2
  exit 1
fi

checksum_file="$archive.sha256"
if [[ ! -f "$checksum_file" ]]; then
  echo "Checksum file was not found: $checksum_file" >&2
  exit 1
fi

(
  cd -- "$(dirname -- "$archive")"
  sha256sum -c "$(basename -- "$checksum_file")"
)

checksum=$(sha256sum "$archive" | awk '{print $1}')
install_dir="$install_root/ubuntu24.04-x64-${checksum:0:12}"

if [[ -e "$install_dir" ]]; then
  if [[ -f "$install_dir/env.sh" ]]; then
    echo "Build dependencies are already installed: $install_dir"
    echo "Run: source \"$install_dir/env.sh\""
    exit 0
  fi
  echo "Install target already exists but is incomplete: $install_dir" >&2
  exit 1
fi

mkdir -p "$install_root"
install_tmp=$(mktemp -d "$install_root/.install-XXXXXXXX")
trap 'rm -rf -- "$install_tmp"' EXIT
tar --zstd -xf "$archive" -C "$install_tmp" --strip-components=1

if [[ ! -f "$install_tmp/env.sh" ]] || \
   [[ ! -f "$install_tmp/qt/lib/cmake/Qt6/Qt6Config.cmake" ]] || \
   [[ ! -x "$install_tmp/vcpkg/vcpkg" ]]; then
  echo "The extracted package is incomplete." >&2
  exit 1
fi

mv "$install_tmp" "$install_dir"
trap - EXIT

current_link="$install_root/current"
if [[ -L "$current_link" ]] || [[ ! -e "$current_link" ]]; then
  ln -sfn "$(basename -- "$install_dir")" "$current_link"
  env_path="$current_link/env.sh"
else
  env_path="$install_dir/env.sh"
  echo "Warning: $current_link exists and is not a symlink; it was not replaced." >&2
fi

echo "Installed: $install_dir"
echo "Run: source \"$env_path\""
