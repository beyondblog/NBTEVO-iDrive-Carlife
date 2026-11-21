#!/bin/sh
# Stop the foreground projection with Ctrl+C before running this script.
set -eu

fail() {
    echo "$*" >&2
    exit 1
}

[ "$#" -eq 0 ] || fail "Usage: sh uninstall.sh"
PATH=/bin:/usr/bin:/sbin:/usr/sbin
export PATH
[ "$(uname -s)" = QNX ] || fail "Run this uninstaller on the QNX head unit."

case "$(uname -n)" in hu-omap|hu-omap.*) ;; *) fail "Run on the hu-omap node." ;; esac
credentials=$(pidin -p "$$" -F '%a %W') || fail "Cannot verify root access with pidin."
uid=
while read -r pid effective_uid extra; do
    [ "$pid" = "$$" ] || continue
    [ -z "$extra" ] || fail "Unexpected output from pidin."
    uid=$effective_uid
done <<EOF_PIDIN
$credentials
EOF_PIDIN
[ "$uid" = 0 ] || fail "Run as root on hu-omap."
[ -d /var/opt ] || fail "Missing /var/opt on the head unit."
umask 077

dest=/var/opt/nbt-carlife
marker=$dest/.nbt-carlife-install
lock=$dest/.maintenance
[ ! -L "$dest" ] || fail "Unexpected symlink at $dest."
if [ ! -e "$dest" ]; then
    echo 'CarLife is not installed.'
    exit 0
fi
[ -d "$dest" ] || fail "Unexpected file type at $dest."
[ "$(find "$dest" -prune -type d -user root \
    \( -perm 0700 -o -perm 0755 \) -print)" = "$dest" ] ||
    fail "Expected a root-owned directory with mode 0700 (or legacy 0755): $dest."
mkdir "$lock" 2>/dev/null || fail "Another maintenance command is active in $dest."
trap 'rm -r "$lock"' 0
trap 'exit 1' HUP INT TERM
[ ! -L "$marker" ] && [ -f "$marker" ] &&
    [ "$(cat "$marker")" = 'nbt-carlife managed install v1' ] ||
    fail "Refusing to remove files from an unmanaged $dest."

rm -f "$dest/nbt-carlife"
rm -r "$lock"
trap - 0
echo "CarLife executable removed. Kept private settings, marker and PF backups in $dest."
