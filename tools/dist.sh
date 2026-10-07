#!/bin/sh
# The release binaries of one program, for release.sh's `make dist`; it signs
# the darwin one and publishes them all:
#   DIR/NAME-darwin-universal  arm64 and x86_64, nothing past libSystem
#   DIR/NAME-linux-amd64.gz    static, musl
#   DIR/NAME-linux-arm64.gz
# macOS is CC's; Linux is zig's cross compiler (ZIG), which carries musl for
# both architectures, so one Mac builds the whole release.
set -eu
usage='usage: dist.sh DIR NAME CFLAGS... SOURCES...'
dir=${1:?$usage}
name=${2:?$usage}
shift 2
cc=${CC:-cc}
zig=${ZIG:-zig}
work=$dir/.work/$name
mkdir -p "$work"
for arch in arm64 x86_64; do
    $cc -arch "$arch" -mmacosx-version-min=11.0 -o "$work/$arch" "$@"
done
lipo -create -output "$dir/$name-darwin-universal" "$work/arm64" "$work/x86_64"
for pair in x86_64:amd64 aarch64:arm64; do
    out=$dir/$name-linux-${pair#*:}
    $zig cc -target "${pair%%:*}-linux-musl" -static -s -o "$out" "$@"
    gzip -9f "$out"
done
