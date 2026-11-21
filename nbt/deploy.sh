#!/bin/sh
# Mac / Linux launcher. Use an existing SSH destination or SSH config alias.
set -eu

fail() {
    printf '%s\n' "$*" >&2
    exit 1
}

if [ "$#" -ne 2 ]; then
    fail "Usage: $0 upload|install|uninstall|install-menu|uninstall-menu SSH_DESTINATION"
fi
action=$1
host=$2
case "$action" in
    upload|install|uninstall) ;;
    install-menu|uninstall-menu) ;;
    *) fail "Expected upload, install, uninstall, install-menu or uninstall-menu." ;;
esac
case "$host" in ''|-*|*[!a-zA-Z0-9_.:@%-]*) fail "Invalid SSH destination." ;; esac

here=$(cd "$(dirname "$0")" && pwd)
binary=$here/../dist/qnx-armv7le/nbt-carlife
bundle=$here/../dist/qnx-armv7le
script=$here/$action.sh
[ "$action" != uninstall-menu ] || script=$here/install-menu.sh
if [ "$action" != upload ]; then
    [ -f "$script" ] && [ ! -L "$script" ] || fail "Missing $script."
fi
case "$action" in
upload|install)
    [ -f "$binary" ] && [ ! -L "$binary" ] && [ -s "$binary" ] || fail "Missing $binary."
    (
        cd "$(dirname "$binary")"
        if command -v shasum >/dev/null 2>&1; then
            shasum -a 256 -c SHA256SUMS
        else
            sha256sum -c SHA256SUMS
        fi
    )
    ;;
install-menu)
    (
        cd "$bundle"
        if command -v shasum >/dev/null 2>&1; then
            shasum -a 256 -c SHA256SUMS.plugin
        else
            sha256sum -c SHA256SUMS.plugin
        fi
    )
    ;;
esac

umask 077
work=$(mktemp -d /tmp/car-deploy.XXXXXX)
socket=$work/ssh
# QNX /dev/shmem is flat on NBT Evo. Use a private data-partition directory.
stage=/fs/sda1/opt/nbt-carlife-stage-${work##*.}
mounted_stage=false
master_open=false
node_prefix=
ssh_password=${CARLIFE_SSH_PASSWORD:-}
unset CARLIFE_SSH_PASSWORD

master_login() {
    if [ -n "$ssh_password" ]; then
        cat > "$work/askpass" <<'ASKPASS'
#!/bin/sh
printf '%s\n' "$CARLIFE_ASKPASS_PASSWORD"
ASKPASS
        chmod 700 "$work/askpass"
        CARLIFE_ASKPASS_PASSWORD="$ssh_password" SSH_ASKPASS="$work/askpass" \
            SSH_ASKPASS_REQUIRE=force DISPLAY="${DISPLAY:-none}" ssh "$@" </dev/null
    else
        SSH_ASKPASS_REQUIRE=never ssh "$@"
    fi
}

ssh_command() {
    ssh -T -o "ControlPath=$socket" -o BatchMode=yes -o PubkeyAuthentication=no \
        -o StrictHostKeyChecking=yes -o ClearAllForwardings=yes "$host" "$1"
}

remote() {
    if [ -n "$node_prefix" ]; then
        quoted=$(printf '%s' "$1" | sed "s/'/'\\\\''/g")
        ssh_command "$node_prefix /bin/ksh -c '$quoted'"
    else
        ssh_command "$1"
    fi
}

cleanup() {
    if "$mounted_stage"; then
        remote "test -d '$stage' && test ! -L '$stage' && rm -r '$stage'" </dev/null ||
            printf 'Could not remove the temporary directory %s\n' "$stage" >&2
    fi
    if "$master_open"; then
        ssh -O exit -o "ControlPath=$socket" "$host" >/dev/null 2>&1 || :
    fi
    rm -f "$work/received" "$work/askpass"
    rmdir "$work" 2>/dev/null || :
}
trap cleanup 0
trap 'exit 1' HUP INT TERM

printf 'Connecting to %s\n' "$host"
# Host key algorithms identify the car. User authentication is password-only.
host_keys=+ssh-rsa
if ssh -Q key | grep -qx ssh-dss; then host_keys=+ssh-rsa,ssh-dss; fi
master_login -fMN -o "ControlPath=$socket" -o ConnectTimeout=8 \
    -o "HostKeyAlgorithms=$host_keys" -o StrictHostKeyChecking=ask \
    -o BatchMode=no -o PubkeyAuthentication=no -o PasswordAuthentication=yes \
    -o KbdInteractiveAuthentication=yes -o PreferredAuthentications=password,keyboard-interactive \
    -o NumberOfPasswordPrompts=1 -o ClearAllForwardings=yes "$host"
master_open=true
unset ssh_password

if ! node=$(ssh_command 'test "$(uname -s)" = QNX && uname -n' </dev/null); then
    fail "SSH destination must be the QNX head unit."
fi
case "$node" in
    hu-omap|hu-omap.*) ;;
    hu-jacinto|hu-jacinto.*) node_prefix='on -f hu-omap' ;;
    *) fail "Unexpected QNX node: $node. Expected hu-omap or hu-jacinto." ;;
esac
if ! remote 'case "$(uname -n)" in hu-omap|hu-omap.*) ;; *) exit 1 ;; esac
    credentials=$(pidin -p "$$" -F "%a %W") || exit 1
    uid=
    while read -r pid effective_uid extra; do
        [ "$pid" = "$$" ] || continue
        [ -z "$extra" ] || exit 1
        uid=$effective_uid
    done <<EOF_PIDIN
$credentials
EOF_PIDIN
    [ "$uid" = 0 ]' </dev/null; then
    fail "Cannot run commands as root on the target hu-omap node."
fi
printf 'Deploying on hu-omap (SSH node: %s)\n' "$node"
remote "umask 077; mkdir '$stage'" </dev/null
mounted_stage=true

upload() {
    file=$1
    name=${file##*/}
    printf 'Transferring and checking %s\n' "$name"
    remote "umask 077; cat > '$stage/$name'" < "$file"
    remote "cat '$stage/$name'" </dev/null > "$work/received"
    cmp -s "$file" "$work/received" || fail "Transfer verification failed for $name."
}

if [ "$action" != upload ]; then upload "$script"; fi
case "$action" in
    upload|install) upload "$binary" ;;
    install-menu)
        for name in libNbtCarLife.so NbtCarLife.manifest NbtCarLife.xml NbtCarLife_ALL_Texts.zip NbtCarLife_ALL_Images.zip; do
            upload "$bundle/plugin/$name"
        done
        ;;
esac
if [ "$action" = upload ]; then
    mounted_stage=false
    printf 'Verified upload on hu-omap: %s/nbt-carlife\n' "$stage"
    exit 0
fi
printf 'Running %s on the head unit\n' "$action"
case "$action" in
    install-menu|uninstall-menu) remote "sh '$stage/install-menu.sh' '$action'" </dev/null ;;
    *) remote "sh '$stage/$action.sh'" </dev/null ;;
esac
