#!/usr/bin/env bash
# Build the PS3 app's site code for this PC and test it against the live sites (needs curl and mbedtls).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../app/source
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -O1 -g -Wall -I"$src" -o "$out/sitetest" "$here/sitetest.c" "$here/host_http.c" \
	"$src/sites.c" "$src/catalog.c" "$src/resolve.c" -lmbedcrypto
SITETEST_DIR="$out/" "$out/sitetest"
