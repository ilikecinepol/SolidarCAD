#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "Usage: $0 <qt-root> <vcpkg-root> <vcpkg-binary-cache> <output.tar.zst>" >&2
}

if [[ $# -ne 4 ]]; then
  usage
  exit 2
fi

qt_root=$(realpath "$1")
vcpkg_root=$(realpath "$2")
vcpkg_cache=$(realpath "$3")
output=$(realpath -m "$4")

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)
package_name=solidarcad-build-deps-ubuntu24.04-x64

if [[ ! -f "$qt_root/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
  echo "Qt6Config.cmake was not found under: $qt_root" >&2
  exit 1
fi

if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
  echo "Bootstrapped vcpkg executable was not found under: $vcpkg_root" >&2
  exit 1
fi

if [[ ! -d "$vcpkg_cache" ]] || ! find "$vcpkg_cache" -type f -print -quit | grep -q .; then
  echo "The vcpkg binary cache is empty: $vcpkg_cache" >&2
  exit 1
fi

if ! command -v zstd >/dev/null 2>&1; then
  echo "zstd is required to create the build-dependency package." >&2
  exit 1
fi

mkdir -p "$(dirname -- "$output")"
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
package_root="$stage/$package_name"
mkdir -p "$package_root/qt" "$package_root/vcpkg" \
  "$package_root/vcpkg-binary-cache" "$package_root/licenses"

cp -a "$qt_root/." "$package_root/qt/"

# Keep the pinned vcpkg registry, its shallow Git metadata and executable so
# builtin-baseline resolution still works. Omit only transient build outputs;
# package restores come from the packaged binary cache.
tar -C "$vcpkg_root" \
  --exclude=buildtrees \
  --exclude=downloads \
  --exclude=installed \
  --exclude=packages \
  -cf - . | tar -C "$package_root/vcpkg" -xf -

cp -a "$vcpkg_cache/." "$package_root/vcpkg-binary-cache/"
cp -a "$repo_root/LICENSES/." "$package_root/licenses/"
cp "$repo_root/DEPENDENCIES.md" "$repo_root/NOTICE" \
  "$repo_root/THIRD_PARTY_NOTICES.md" "$package_root/"

qt_version=$("$qt_root/bin/qmake" -query QT_VERSION)
baseline=$(python3 -c \
  'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["builtin-baseline"])' \
  "$repo_root/vcpkg.json")
source_commit=$(git -C "$repo_root" rev-parse HEAD)

cat > "$package_root/manifest.json" <<EOF
{
  "format": 1,
  "platform": "ubuntu-24.04-x86_64",
  "qt_version": "$qt_version",
  "occt_version": "8.0.1",
  "vcpkg_baseline": "$baseline",
  "vcpkg_triplet": "ci-x64-linux",
  "source_commit": "$source_commit"
}
EOF

cat > "$package_root/env.sh" <<'EOF'
#!/usr/bin/env bash

_solidar_build_deps_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export SOLIDAR_BUILD_DEPS_ROOT="$_solidar_build_deps_root"
export VCPKG_ROOT="$_solidar_build_deps_root/vcpkg"
export VCPKG_DEFAULT_TRIPLET=ci-x64-linux
export CMAKE_PREFIX_PATH="$_solidar_build_deps_root/qt${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

export SOLIDAR_VCPKG_WRITE_CACHE="${SOLIDAR_VCPKG_WRITE_CACHE:-$HOME/.cache/solidarcad-vcpkg-binaries}"
mkdir -p "$SOLIDAR_VCPKG_WRITE_CACHE"
export VCPKG_BINARY_SOURCES="clear;files,$_solidar_build_deps_root/vcpkg-binary-cache,read;files,$SOLIDAR_VCPKG_WRITE_CACHE,readwrite"

unset _solidar_build_deps_root
EOF
chmod +x "$package_root/env.sh"

cat > "$package_root/README.txt" <<'EOF'
SolidarCAD build dependencies for Ubuntu 24.04 x86_64

1. Extract this archive with scripts/install_linux_build_deps.sh.
2. Source the env.sh path printed by that script.
3. From a SolidarCAD source checkout run:
     cmake --preset ci
     cmake --build --preset ci
     QT_QPA_PLATFORM=offscreen ctest --preset ci

The package contains Qt and a read-only vcpkg binary cache. New or changed
packages are written to ~/.cache/solidarcad-vcpkg-binaries.
EOF

tar --zstd -cf "$output" -C "$stage" "$package_name"
(
  cd -- "$(dirname -- "$output")"
  sha256sum "$(basename -- "$output")" > "$(basename -- "$output").sha256"
)

echo "Created: $output"
echo "Checksum: $output.sha256"
