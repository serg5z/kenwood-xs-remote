#!/bin/sh
set -eu
output=$1
report="$output/validation.txt"
exec > "$report" 2>&1
dpkg-query -W -f='${Package} ${Version} ${Architecture}\n' > "$output/packages.txt"
getconf GNU_LIBC_VERSION
gcc --version
pkg-config --modversion libgpiod
dpkg-query -W libcec-dev
readelf -h /lib/ld-linux-armhf.so.3
readelf -A /lib/ld-linux-armhf.so.3
for name in cec-xs-bridge cec-xs-client xs8_send xs8-service libcec-runtime; do
    binary="$output/$name"
    if [ "$name" = libcec-runtime ]; then
        # cecloader.h uses dlopen: libcec can be absent from DT_NEEDED even
        # though the running CEC programs require it and its dependencies.
        binary="$(gcc -print-file-name=libcec.so)"
    fi
    echo "=== $name ==="
    file "$binary"
    readelf -h "$binary"
    readelf -A "$binary"
    readelf -l "$binary"
    readelf -d "$binary"
    readelf -h "$binary" | grep -q 'Class:.*ELF32'
    readelf -h "$binary" | grep -q 'Machine:.*ARM'
    readelf -h "$binary" | grep -q 'Flags:.*hard-float ABI'
    readelf -A "$binary" | grep -Eq 'Tag_CPU_arch: v6(KZ|K)?$'
    readelf -A "$binary" | grep -q 'Tag_ABI_VFP_args: VFP registers'
    if [ "$name" != libcec-runtime ]; then
        readelf -l "$binary" | grep -q '/lib/ld-linux-armhf.so.3'
    fi
    # The loader lists dependencies without executing the application's main.
    libraries=$(/lib/ld-linux-armhf.so.3 --list "$binary")
    printf '%s\n' "$libraries"
    printf '%s\n' "$libraries" | awk '/=> \// {print $3}' | while read -r library; do
        echo "--- dependency: $library ---"
        readelf -A "$library"
        # Some shared objects omit attributes; record that limitation. Reject
        # explicit ARMv7/v8 dependencies rather than trusting the armhf label.
        if readelf -A "$library" | grep -Eq 'Tag_CPU_arch: v(6T2|7|8|9)|Tag_FP_arch: (VFPv[3-9]|FPv)|Tag_Advanced_SIMD_arch:'; then
            echo "Incompatible dependency: $library" >&2
            exit 1
        fi
    done
    echo 'Required symbol versions:'
    readelf --version-info "$binary"
done
(cd "$output" && sha256sum cec-xs-bridge cec-xs-client xs8_send xs8-service) \
    > "$output/SHA256SUMS"
echo 'ELF validation passed; this is not a hardware test.'
