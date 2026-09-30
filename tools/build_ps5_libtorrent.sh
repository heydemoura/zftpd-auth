#!/usr/bin/env bash
# Build the native BitTorrent engine against the same SDK as the PS5 payload.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 /path/to/ps5-payload-sdk" >&2
  exit 2
fi

sdk="$(cd "$1" && pwd -P)"
hbroot="$sdk/target/user/homebrew"
test -x "$sdk/bin/prospero-clang++"
test -x "$sdk/bin/prospero-pkg-config"
test -f "$hbroot/include/openssl/ssl.h"
if [[ -z "${LLVM_CONFIG:-}" ]]; then
  LLVM_CONFIG="$(command -v llvm-config-21 || command -v llvm-config-20 ||
    command -v llvm-config-19 || command -v llvm-config-18 ||
    command -v llvm-config || true)"
  if [[ -z "$LLVM_CONFIG" ]] && command -v brew >/dev/null 2>&1; then
    llvm_prefix="$(brew --prefix llvm 2>/dev/null || true)"
    if [[ -x "$llvm_prefix/bin/llvm-config" ]]; then
      LLVM_CONFIG="$llvm_prefix/bin/llvm-config"
    fi
  fi
  if [[ -z "$LLVM_CONFIG" ]]; then
    echo "llvm-config missing" >&2
    exit 1
  fi
  export LLVM_CONFIG
fi

if [[ -n "${BOOST_INCLUDE_DIR:-}" ]]; then
  boost_include="$BOOST_INCLUDE_DIR"
elif [[ -f /usr/include/boost/config.hpp ]]; then
  boost_include=/usr/include
elif [[ -f /opt/homebrew/opt/boost/include/boost/config.hpp ]]; then
  boost_include=/opt/homebrew/opt/boost/include
else
  echo "Boost headers missing: install libboost-dev or set BOOST_INCLUDE_DIR" >&2
  exit 1
fi
test -f "$boost_include/boost/config.hpp"
mkdir -p "$hbroot/include"
if [[ ! -f "$hbroot/include/boost/config.hpp" ]]; then
  cp -R "$boost_include/boost" "$hbroot/include/boost"
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/zftpd-libtorrent.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT
version=2.1.2
archive="$work/libtorrent-rasterbar-$version.tar.gz"
curl -fL --retry 3 -o "$archive" \
  "https://github.com/arvidn/libtorrent/releases/download/v$version/libtorrent-rasterbar-$version.tar.gz"
python3 - "$archive" <<'PY'
import hashlib
import sys

expected = "3362546d9cd71b9e49ee6cac7d3f1f914ce9cdb217c86b63d5b22cbed0334dbc"
with open(sys.argv[1], "rb") as stream:
    digest = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(block)
    actual = digest.hexdigest()
if actual != expected:
    raise SystemExit("libtorrent source checksum mismatch")
PY

mkdir -p "$work/src"
tar -xzf "$archive" -C "$work/src" --strip-components=1
cmake -S "$work/src" -B "$work/build" \
  -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_VERBOSE_MAKEFILE=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DBoost_NO_BOOST_CMAKE=ON \
  -DBoost_INCLUDE_DIR="$hbroot/include" \
  -DBOOST_ROOT="$hbroot" \
  -Dbuild_tests=OFF -Dbuild_examples=OFF -Dbuild_tools=OFF \
  -Dpython-bindings=OFF -Di2p=OFF -Dwebtorrent=OFF
cmake --build "$work/build" -j"${JOBS:-4}"
DESTDIR="$sdk/target" cmake --install "$work/build" --prefix /user/homebrew

# CMake's .pc generator also emits absolute build-host paths for Boost and
# OpenSSL. The PS5 pkg-config wrapper already maps /user/homebrew into the SDK.
python3 - "$hbroot/lib/pkgconfig/libtorrent-rasterbar.pc" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
lines = []
for line in path.read_text().splitlines():
    if line.startswith(("Libs:", "Cflags:")):
        parts = line.split()
        parts = [p for p in parts if not ((p.startswith("-I/") or p.startswith("-L/"))
                 and not p.startswith(("-I/user/homebrew", "-L/user/homebrew")))]
        line = " ".join(parts)
    lines.append(line)
path.write_text("\n".join(lines) + "\n")
PY

"$sdk/bin/prospero-pkg-config" --exists libtorrent-rasterbar
echo "PS5 libtorrent-rasterbar $version installed in $sdk"
