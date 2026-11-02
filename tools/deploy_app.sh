#!/usr/bin/env bash
# Put a new build straight into the installed app over FTP, with no Package Manager step.
# The app must have been installed from a .pkg once. Keeps the previous EBOOT.BIN on this PC, and
# clears the old log, so a stale result can't be mistaken for a new one.
# Usage: deploy_app.sh <app dir> <title id>, e.g. deploy_app.sh app ANIMEPS30
set -euo pipefail

PS3=${PS3_HOST:?set PS3_HOST to your PS3's address, with webMAN MOD's FTP running}
app_dir=${1:?usage: deploy_app.sh <app dir> <title id>}
title=${2:?usage: deploy_app.sh <app dir> <title id>}
local_eboot=$app_dir/build/pkg/USRDIR/EBOOT.BIN
remote=dev_hdd0/game/$title/USRDIR/EBOOT.BIN

curl -sS -m 60 -o "$app_dir/build/EBOOT.previous.BIN" "ftp://$PS3/$remote"
curl -sS -m 60 -T "$local_eboot" "ftp://$PS3/$remote"

want=$(sha256sum "$local_eboot" | cut -d' ' -f1)
got=$(curl -s -m 60 "ftp://$PS3/$remote" | sha256sum | cut -d' ' -f1)
if [[ $want != "$got" ]]; then
	echo "checksum mismatch: the installed EBOOT.BIN may be damaged" >&2
	exit 1
fi
curl -sS -m 15 -o /dev/null -Q "DELE /dev_hdd0/game/$title/USRDIR/data/app_log.txt" "ftp://$PS3/" || true
echo "deployed $title, checksum ${want:0:12}"
