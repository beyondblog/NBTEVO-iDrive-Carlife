#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/nbt-carlife-host.XXXXXX")
trap 'rm -rf "$work"' 0
trap 'exit 1' HUP INT TERM
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$here" -o "$work/test" \
    "$here/tests/host.cpp" "$here/carlife_session.cpp" "$here/discovery.cpp" \
    "$here/display_commands.cpp" "$here/firewall.cpp" "$here/wifi_key.cpp"
"$work/test"
