#!/usr/bin/env bash
set -eu

executable="${1:-build/fuzz/fuzz/project_decoder_fuzz}"
seconds="${2:-60}"
temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/solidar-project-fuzz.XXXXXX")"
corpus_dir="${temporary_root}/corpus"
artifact_dir="${temporary_root}/artifacts"
mkdir -p "${corpus_dir}" "${artifact_dir}"
cp fuzz/corpus/project_decoder/minimal-v1.solidar "${corpus_dir}/"

echo "Fuzz corpus and artifacts: ${temporary_root}"
exec "${executable}" "${corpus_dir}" \
  "-artifact_prefix=${artifact_dir}/" \
  "-max_total_time=${seconds}" \
  -max_len=1048576 \
  -rss_limit_mb=4096
