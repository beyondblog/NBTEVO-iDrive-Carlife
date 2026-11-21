#!/bin/sh
# Run on the NBT Evo head unit, with nbt-carlife beside this script.
set -eu

fail() {
    echo "$*" >&2
    exit 1
}

[ "$#" -eq 0 ] || fail "Usage: sh install.sh"
PATH=/bin:/usr/bin:/sbin:/usr/sbin
export PATH
[ "$(uname -s)" = QNX ] || fail "Run this installer on the QNX head unit."
case "$0" in */*) source_dir=${0%/*} ;; *) source_dir=. ;; esac
source_dir=$(cd "$source_dir" && pwd)
source_file=$source_dir/nbt-carlife
[ ! -L "$source_file" ] && [ -f "$source_file" ] && [ -s "$source_file" ] || fail "Missing nbt-carlife beside install.sh."

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
stage=$lock/nbt-carlife

[ ! -L "$dest" ] || fail "Unexpected symlink at $dest."
if mkdir "$dest" 2>/dev/null; then
    if ! echo 'nbt-carlife managed install v1' > "$marker"; then
        rmdir "$dest" 2>/dev/null || :
        fail "Could not create the installation marker."
    fi
elif [ ! -d "$dest" ]; then
    fail "Cannot create $dest. Run on the head unit with write access to /var/opt."
fi

[ "$(find "$dest" -prune -type d -user root \
    \( -perm 0700 -o -perm 0755 \) -print)" = "$dest" ] ||
    fail "Expected a root-owned directory with mode 0700 (or legacy 0755): $dest."
mkdir "$lock" 2>/dev/null || fail "Another maintenance command is active in $dest."
trap 'rm -r "$lock"' 0
trap 'exit 1' HUP INT TERM
[ ! -L "$marker" ] && [ -f "$marker" ] &&
    [ "$(cat "$marker")" = 'nbt-carlife managed install v1' ] ||
    fail "Refusing to replace files in an unmanaged $dest."
[ ! -L "$dest/nbt-carlife" ] && [ ! -d "$dest/nbt-carlife" ] ||
    fail "Unexpected file type at $dest/nbt-carlife."

chmod 700 "$dest"
chmod 600 "$marker"
cp "$source_file" "$stage"
chmod 700 "$stage"
mv -f "$stage" "$dest/nbt-carlife"
echo "Installed $dest/nbt-carlife"
echo "Query Wi-Fi Direct: $dest/nbt-carlife --wifi-direct-info"
echo "Search for phone:   $dest/nbt-carlife --auto"
echo "Start by address:   $dest/nbt-carlife --wifi-render PHONE_IPV4"
