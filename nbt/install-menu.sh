#!/bin/sh
set -eu
PATH=/bin:/usr/bin:/sbin:/usr/sbin
export PATH LC_ALL=C

fail() { echo "$*" >&2; exit 1; }
[ "$#" -eq 1 ] || fail "Use install-menu or uninstall-menu."
case "$1" in install-menu|uninstall-menu) ;; *) fail "Invalid menu operation." ;; esac
[ "$(uname -s)" = QNX ] || fail "Run on the head unit."
case "$(uname -n)" in hu-omap|hu-omap.*) ;; *) fail "Run on hu-omap." ;; esac
credentials=$(pidin -p "$$" -F '%a %W') || fail "Cannot verify effective UID."
uid=
while read -r pid effective_uid extra; do
    [ "$pid" = "$$" ] || continue
    [ -z "$extra" ] || fail "Unexpected pidin output."
    uid=$effective_uid
done <<EOF_UID
$credentials
EOF_UID
[ "$uid" = 0 ] || fail "Run as root."

case "$0" in */*) source=${0%/*} ;; *) source=. ;; esac
source=$(cd "$source" && pwd)
root=$(cd /opt/hmi/data/ro/Plugins && pwd -P) || fail "Cannot resolve the active HMI plugin directory."
case "$root" in
    /opt/hmi/ID5/data/ro/Plugins|/fs/sda0/opt/hmi/ID5/data/ro/Plugins|\
    /opt/hmi/ID6/data/ro/Plugins|/fs/sda0/opt/hmi/ID6/data/ro/Plugins) ;;
    *) fail "Unexpected HMI plugin directory $root." ;;
esac
dest=$root/NbtCarLife
incoming=$root/.NbtCarLife-incoming-$$
previous=$root/.NbtCarLife-previous-$$
marker=.nbt-carlife-menu
manifest='NbtCarLife managed menu v1'
files='libNbtCarLife.so NbtCarLife.manifest NbtCarLife.xml NbtCarLife_ALL_Texts.zip NbtCarLife_ALL_Images.zip'

[ ! -L "$dest" ] || fail "Unexpected symlink $dest."
[ ! -e "$incoming" ] && [ ! -e "$previous" ] || fail "Menu staging path is in use."
if [ -e "$dest" ]; then
    [ -f "$dest/$marker" ] && [ ! -L "$dest/$marker" ] &&
        [ "$(cat "$dest/$marker")" = "$manifest" ] || fail "Refusing to modify an unmanaged plugin."
    for entry in "$dest"/* "$dest"/.[!.]* "$dest"/..?*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        case "${entry##*/}" in
            libNbtCarLife.so|NbtCarLife.manifest|NbtCarLife.xml|NbtCarLife_ALL_Texts.zip|NbtCarLife_ALL_Images.zip|.nbt-carlife-menu)
                [ -f "$entry" ] && [ ! -L "$entry" ] || fail "Unexpected file type: $entry." ;;
            *) fail "Unexpected extra file in the managed plugin: $entry." ;;
        esac
    done
elif [ "$1" = uninstall-menu ]; then
    echo "The CarLife menu is already absent."
    exit 0
fi

if [ "$1" = install-menu ]; then
    [ -x /var/opt/nbt-carlife/nbt-carlife ] || fail "Install the CarLife executable first."
    for name in $files; do
        [ -s "$source/$name" ] && [ ! -L "$source/$name" ] || fail "Missing menu asset $name."
    done
fi

remounted=no
committed=no
made=no
moved=no
cleanup() {
    result=$?
    trap - 0
    if [ "$committed" = no ] && [ "$moved" = yes ] && [ ! -e "$dest" ]; then
        mv "$previous" "$dest" || result=1
    fi
    if [ "$made" = yes ] && [ -d "$incoming" ]; then rm -r "$incoming" || result=1; fi
    if [ "$committed" = yes ] && [ "$moved" = yes ]; then rm -r "$previous" || result=1; fi
    if [ "$remounted" = yes ]; then mount -ur /fs/sda0 || result=1; fi
    exit "$result"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM
umask 077

if output=$(mkdir "$incoming" 2>&1); then
    made=yes
else
    case "$output" in *[Rr]ead-only*) ;; *) fail "$output" ;; esac
    mount -uw /fs/sda0
    remounted=yes
    mkdir "$incoming"
    made=yes
fi

if [ "$1" = install-menu ]; then
    for name in $files; do
        cat "$source/$name" > "$incoming/$name"
        chmod 644 "$incoming/$name"
    done
    chmod 755 "$incoming/libNbtCarLife.so"
    echo "$manifest" > "$incoming/$marker"
    chmod 755 "$incoming"
fi
if [ -d "$dest" ]; then
    mv "$dest" "$previous"
    moved=yes
fi
if [ "$1" = install-menu ]; then mv "$incoming" "$dest"; fi
committed=yes
echo "CarLife menu operation complete. The HMI picks up changes after its next restart."
