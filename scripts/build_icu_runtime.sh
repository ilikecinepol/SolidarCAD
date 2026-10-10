#!/usr/bin/env bash
set -euo pipefail

prefix="${1:?usage: build_icu_runtime.sh <install-prefix>}"
archive_name="icu4c-73_2-src.tgz"
archive_sha256="818a80712ed3caacd9b652305e01afc7fa167e6f2e94996da44b90c2ab604ce1"
archive_url="https://github.com/unicode-org/icu/releases/download/release-73-2/${archive_name}"

if [[ -f "${prefix}/lib/libicuuc.so.73" &&
      -f "${prefix}/lib/libicui18n.so.73" &&
      -f "${prefix}/lib/libicudata.so.73" ]]; then
  exit 0
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

curl --fail --location --retry 3 --output "${work_dir}/${archive_name}" \
  "${archive_url}"
echo "${archive_sha256}  ${work_dir}/${archive_name}" | sha256sum --check --strict
tar --extract --gzip --file "${work_dir}/${archive_name}" \
  --directory "${work_dir}"

pushd "${work_dir}/icu/source" >/dev/null
./runConfigureICU Linux \
  --prefix="${prefix}" \
  --disable-extras \
  --disable-samples \
  --disable-tests
make --jobs="${ICU_BUILD_JOBS:-2}"
make install
popd >/dev/null
