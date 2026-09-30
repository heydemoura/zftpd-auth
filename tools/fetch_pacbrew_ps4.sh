#!/usr/bin/env bash
# Fetch the PacBrew PS4 port libraries that back the URL downloader.
#
# PacBrew ships ps4-openorbis-libcurl (static, mbedTLS, links -lSceNet) and its
# dependencies as pacman packages.  This script unpacks the pinned set into a
# plain prefix — no pacman and no OpenOrbis toolchain are needed, because the
# payload keeps being built with PS4_PAYLOAD_SDK: only the headers and the
# static libraries are used.  (The curl-config wrapper inside the package emits
# OpenOrbis-only link flags such as its own linker script and -nostdlib, which
# must not leak into a payload link.)
#
# The Makefile detects the prefix automatically (PS4_PACBREW_ROOT), so after
# running this script `make TARGET=ps4 ENABLE_ZHTTPD=1` builds with the URL
# downloader enabled and the CA bundle embedded.
#
# Usage: tools/fetch_pacbrew_ps4.sh [PREFIX]
#   PREFIX  extraction root (default: external/pacbrew/ps4/openorbis)
#
# Environment:
#   PACBREW_PS4_REPO     package server (default: the public PacBrew repo)
#   PS4_CA_BUNDLE_FILE   reuse an existing Mozilla CA bundle instead of
#                        downloading one from curl.se
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
prefix="${1:-$script_dir/../external/pacbrew/ps4/openorbis}"
prefix="${prefix%/}"
repo="${PACBREW_PS4_REPO:-https://pacman.mydedibox.fr/pacbrew/packages}"
ca_file="${PS4_CA_BUNDLE_FILE:-}"
cache="$(dirname "$prefix")/.downloads"

# name sha256 — pinned like the PS5 PacBrew bundle in the release workflow.
packages=(
  "ps4-openorbis-zlib-1.3.1-2-any.pkg.tar.xz 2b082d25f34d923e2fd35e0b5f29b19e142e263da4380665bb0e80f0a6378700"
  "ps4-openorbis-mbedtls-2.16.6-3-any.pkg.tar.xz 7fe7ad512c2ccf7b53865433753b9ddcaf6385eed36b8160d93134fa64f0dc58"
  "ps4-openorbis-libcurl-7.80.0-3-any.pkg.tar.xz 249c987a8e41e1b7d320cd3129aa1752416dc19e830714d5a38be6c977f5db98"
)

sha_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

mkdir -p "$cache" "$prefix"
prefix="$(cd "$prefix" && pwd -P)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zftpd-pacbrew-ps4.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT

for entry in "${packages[@]}"; do
  file="${entry%% *}"
  want="${entry##* }"
  archive="$cache/$file"
  if [[ ! -f "$archive" ]] || [[ "$(sha_of "$archive")" != "$want" ]]; then
    echo "downloading $file"
    curl -fL --retry 3 -o "$archive.tmp" "$repo/$file"
    mv "$archive.tmp" "$archive"
  fi
  if [[ "$(sha_of "$archive")" != "$want" ]]; then
    echo "error: checksum mismatch for $archive" >&2
    exit 1
  fi
  tar -xJf "$archive" -C "$work"
done

# The packages carry the upstream /opt/pacbrew/ps4/openorbis layout; flatten
# it into the prefix so the include/ and lib/ trees can be used directly.
if [[ -d "$work/opt/pacbrew/ps4/openorbis" ]]; then
  cp -R "$work/opt/pacbrew/ps4/openorbis/." "$prefix/"
fi
test -f "$prefix/usr/lib/libcurl.a"
test -f "$prefix/usr/lib/libmbedtls.a"
test -f "$prefix/usr/lib/libz.a"

# HTTPS verification must not degrade silently: keep a Mozilla bundle next to
# the libraries, where the Makefile looks for it (embedded into the payload).
bundle="$prefix/usr/etc/ca-bundle.crt"
if [[ -n "$ca_file" ]]; then
  mkdir -p "$(dirname "$bundle")"
  cp "$ca_file" "$bundle"
fi
if [[ ! -s "$bundle" ]]; then
  mkdir -p "$(dirname "$bundle")"
  echo "downloading Mozilla CA bundle"
  curl -fL --retry 3 -o "$bundle.tmp" https://curl.se/ca/cacert.pem
  mv "$bundle.tmp" "$bundle"
fi
grep -q -- "-----BEGIN CERTIFICATE-----" "$bundle"

echo
echo "PacBrew PS4 portlibs installed in $prefix"
echo "  PS4_PACBREW_ROOT=$prefix"
echo "  PS4_CA_BUNDLE=$bundle"
echo "make TARGET=ps4 ENABLE_ZHTTPD=1 now builds with the URL downloader enabled"
