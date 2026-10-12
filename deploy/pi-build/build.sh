#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
repo=$(pwd)
docker_bin=${DOCKER:-docker}
if ! command -v "$docker_bin" >/dev/null 2>&1; then
    docker_bin=/Applications/Docker.app/Contents/Resources/bin/docker
fi
if ! command -v "$docker_bin" >/dev/null 2>&1; then
    echo 'Docker CLI not found; set DOCKER to its path.' >&2
    exit 1
fi
# Docker Desktop's credential helper lives beside its CLI on macOS.
PATH="$(dirname "$(command -v "$docker_bin")"):$PATH"
export PATH
image=${PI_BUILD_IMAGE:-kenwood-xs-pi-build:trixie-armv6}
output=${PI_BUILD_OUTPUT:-.build/pi}
mkdir -p "$output"
output=$(cd "$output" && pwd)
if [ "$output" = "$repo/.build" ] || [ "$output" = "$repo" ]; then
    echo 'PI_BUILD_OUTPUT must be separate from the native build and source directory.' >&2
    exit 1
fi
started=$(date +%s)
"$docker_bin" build --platform linux/arm/v6 -t "$image" deploy/pi-build
"$docker_bin" run --rm --platform linux/arm/v6 --network none \
    --env QEMU_CPU=arm1176 \
    --mount "type=bind,src=$repo,dst=/src,readonly" \
    --mount "type=bind,src=$output,dst=/out" \
    "$image" sh -eu -c '
        make -B BUILD_DIR=/out \
            CFLAGS="-O2 -Wall -Wextra -march=armv6 -mfpu=vfp -mfloat-abi=hard -marm" \
            CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -march=armv6 -mfpu=vfp -mfloat-abi=hard -marm" all
        if ! sh /usr/local/bin/verify-xs8-build /out; then
            cat /out/validation.txt >&2
            exit 1
        fi
    '
elapsed=$(($(date +%s) - started))
printf 'Pi build and ELF validation completed in %s seconds. Output: %s\n' "$elapsed" "$output"
printf '%s\n' "$elapsed" > "$output/build-seconds.txt"
