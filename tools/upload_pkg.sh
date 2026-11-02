#!/usr/bin/env bash
# Upload a build to the PS3 as <name>(<rev>).pkg and delete that app's older revisions, so the
# Package Manager list shows which revision is current.
# Usage: upload_pkg.sh <app dir>, e.g. upload_pkg.sh videotest
# The revision counter is <app dir>/.pkg_rev.
set -euo pipefail

PS3=${PS3_HOST:?set PS3_HOST to your PS3's address, with webMAN MOD's FTP running}
REMOTE_DIR=dev_hdd0/packages

app_dir=${1:?usage: upload_pkg.sh <app dir>}
pkgs=("$app_dir"/*.pkg)
local_pkg=
for p in "${pkgs[@]}"; do
	[[ $p == *.gnpdrm.pkg ]] || local_pkg=$p
done
[[ -n $local_pkg ]] || { echo "no .pkg in $app_dir" >&2; exit 1; }
name=$(basename "$local_pkg" .pkg)

rev_file=$app_dir/.pkg_rev
rev=$(( $(cat "$rev_file" 2>/dev/null || echo 0) + 1 ))
new_name="$name($rev).pkg"

curl -gsS -m 120 -T "$local_pkg" "ftp://$PS3/$REMOTE_DIR/$new_name"

want=$(sha256sum "$local_pkg" | cut -d' ' -f1)
got=$(curl -gs -m 120 "ftp://$PS3/$REMOTE_DIR/$new_name" | sha256sum | cut -d' ' -f1)
if [[ $want != "$got" ]]; then
	echo "checksum mismatch on $new_name; older revisions kept" >&2
	exit 1
fi
echo "$rev" > "$rev_file"

# Exactly <name>.pkg or <name>(<number>).pkg, so other apps' packages are never touched.
curl -gs -m 30 --list-only "ftp://$PS3/$REMOTE_DIR/" | tr -d '\r' | while IFS= read -r file; do
	if [[ $file != "$new_name" && ( $file == "$name.pkg" || $file =~ ^"$name"\([0-9]+\)\.pkg$ ) ]]; then
		curl -gsS -m 30 -o /dev/null -Q "DELE /$REMOTE_DIR/$file" "ftp://$PS3/"
		echo "deleted $file"
	fi
done

echo "uploaded $new_name"
