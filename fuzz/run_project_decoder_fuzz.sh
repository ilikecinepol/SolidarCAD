#!/usr/bin/env bash
set -eu

executable="${1:-build/fuzz/fuzz/project_decoder_fuzz}"
seconds="${2:-60}"
temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/solidar-project-fuzz.XXXXXX")"
corpus_dir="${temporary_root}/corpus"
artifact_dir="${temporary_root}/artifacts"
mkdir -p "${corpus_dir}" "${artifact_dir}"
cp fuzz/corpus/project_decoder/minimal-v1.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/minimal-v5.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/minimal-v6-assembly.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/minimal-v8-nested-assembly.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/minimal-v9-empty-assembly.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/full-v9-v2-mate.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/full-v9-angular-mates.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/corrupt-v9-angular-mate.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/full-v9-angle-mate.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/corrupt-v9-angle-mate.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/unknown-v9-angle-type.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/full-v9-concentric-mates.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/corrupt-v9-concentric-mate.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/unknown-v9-axis-reference.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/full-v9-point-distance-mates.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/corrupt-v9-point-distance-mate.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/unknown-v9-point-reference.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/unknown-v9-capability.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/corrupt-v9-mate.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/boundary-v9-options.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/minimal-v12-home-placement.solidar "${corpus_dir}/"
cp fuzz/corpus/project_decoder/corrupt-v12-home-placement.solidar "${corpus_dir}/"

echo "Fuzz corpus and artifacts: ${temporary_root}"
exec "${executable}" "${corpus_dir}" \
  "-artifact_prefix=${artifact_dir}/" \
  "-max_total_time=${seconds}" \
  -timeout=30 \
  -max_len=1048576 \
  -rss_limit_mb=4096
