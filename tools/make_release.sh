#!/usr/bin/env bash
# Publish a built package as a GitHub release, with the latest.json the app's update check reads.
# Usage: make_release.sh <version code> <version name> "<notes>"
# The code and name must match APP_VERSION_CODE and APP_VERSION_NAME in app/source/config.h.
set -euo pipefail

REPO=razziedazzel/nami-ps3
code=${1:?version code}
name=${2:?version name}
notes=${3:-}
root=$(cd "$(dirname "$0")/.." && pwd)
config=$root/app/source/config.h
pkg=$root/app/anime_app.pkg

grep -q "APP_VERSION_CODE $code\$" "$config" || { echo "config.h has a different version code" >&2; exit 1; }
grep -q "APP_VERSION_NAME \"$name\"\$" "$config" || { echo "config.h has a different version name" >&2; exit 1; }
[[ $name =~ ^[0-9A-Za-z.-]+$ ]] || { echo "the version name must be plain characters" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
asset=$work/Nami-PS3-$name.pkg
cp "$pkg" "$asset"
sha=$(sha256sum "$asset" | cut -d' ' -f1)
python3 - "$code" "$name" "$sha" "$notes" "$REPO" > "$work/latest.json" <<'PY'
import json, sys
code, name, sha, notes, repo = sys.argv[1:]
print(json.dumps({
    "versionCode": int(code),
    "versionName": name,
    "pkgUrl": f"https://github.com/{repo}/releases/download/v{name}/Nami-PS3-{name}.pkg",
    "sha256": sha,
    "notes": notes,
}, indent=2))
PY
gh release create "v$name" "$asset" "$work/latest.json" --repo "$REPO" --title "Nami $name" --notes "$notes"
