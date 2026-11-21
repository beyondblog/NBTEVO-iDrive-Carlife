#!/bin/sh
# Run on the host that owns the Docker daemon.
set -eu

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 SUPRA_MILESTONE_7 QNX_TARGET OUTPUT_DIRECTORY" >&2
    exit 2
fi

project=$(cd "$(dirname "$0")/.." && pwd)
supra=$(cd "$1" && pwd)
sysroot=$(cd "$2" && pwd)
mkdir -p "$3"
output=$(cd "$3" && pwd)
image=${CARLIFE_BUILD_IMAGE:-nbt-carlife-clang-qnx}

if ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build -t "$image" "$project/nbt/docker"
fi
docker run --rm --network none --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$project,dst=/src,readonly" \
    --mount "type=bind,src=$supra,dst=/supra,readonly" \
    --mount "type=bind,src=$sysroot,dst=/qnx,readonly" \
    --mount "type=bind,src=$output,dst=/out" \
    "$image" sh -ec '
        scratch=$(mktemp -d)
        trap '\''rm -rf "$scratch"'\'' EXIT
        make -f nbt/clang-qnx.mk SUPRA_SRC=/supra QNX_TARGET=/qnx OUT="$scratch"
        file "$scratch/nbt-carlife"
        readelf -h -l -d -A "$scratch/nbt-carlife" |
            grep -E "ELF32|little endian|EXEC|EABI|interpreter|NEEDED|CPU_arch"
        install -m 755 "$scratch/nbt-carlife" /out/nbt-carlife
        install -d /out/plugin
        install -m 755 "$scratch/libNbtCarLife.so" /out/plugin/libNbtCarLife.so
        install -m 644 nbt/rhmi/NbtCarLife.xml nbt/rhmi/NbtCarLife.manifest /out/plugin
    '
python3 "$project/nbt/rhmi/resources.py" "$output/plugin"
install -m 644 "$supra/third_party/speexdsp/COPYING" "$output/LICENSE-SPEEXDSP.txt"
install -m 644 "$project/licenses/LICENSE-SUPRA.md" "$output/LICENSE-SUPRA.md"
cd "$output"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum nbt-carlife > SHA256SUMS
    sha256sum plugin/* > SHA256SUMS.plugin
else
    shasum -a 256 nbt-carlife > SHA256SUMS
    shasum -a 256 plugin/* > SHA256SUMS.plugin
fi
