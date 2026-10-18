#!/bin/sh
# Fetch build inputs only; never import upstream UI, artwork or test saves.
set -eu
cd "$(dirname "$0")"
. ./upstream.env
temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
curl --fail --location --proto '=https' --tlsv1.2 --max-time 120 \
  "https://codeload.github.com/kwsch/PKHeX/tar.gz/$PKHEX_REVISION" \
  -o "$temporary/source.tar.gz"
printf '%s  %s\n' "$PKHEX_ARCHIVE_SHA256" "$temporary/source.tar.gz" | sha256sum --check --status
mkdir -p "$temporary/pkhex"
tar --extract --gzip --file "$temporary/source.tar.gz" --directory "$temporary/pkhex" \
  --strip-components=1 \
  "PKHeX-$PKHEX_REVISION/PKHeX.Core" \
  "PKHeX-$PKHEX_REVISION/Directory.Build.props" \
  "PKHeX-$PKHEX_REVISION/LICENSE"
mkdir -p .deps
rm -rf .deps/pkhex
mv "$temporary/pkhex" .deps/pkhex
# Runtime libraries are also included as complete matching source archives.
mkdir -p .deps/runtime-source
while read -r name repository revision digest package_version; do
  case "$name" in ''|'#'*) continue ;; esac
  curl --fail --location --proto '=https' --tlsv1.2 --max-time 120 \
    "https://codeload.github.com/$repository/tar.gz/$revision" \
    -o "$temporary/$name.tar.gz"
  printf '%s  %s\n' "$digest" "$temporary/$name.tar.gz" | sha256sum --check --status
  cp "$temporary/$name.tar.gz" ".deps/runtime-source/$name-$revision.tar.gz"
done < runtime-sources.tsv
