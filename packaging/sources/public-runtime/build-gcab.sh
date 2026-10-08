#!/bin/bash
# Newly authored recipe: GPL-3.0-only; upstream sources retain their own terms.
set -euo pipefail
root=$(realpath "${1:?Pass the project root}")
out="$root/.work/github-publication/public-gcab"
work=$(mktemp -d /tmp/pusu-public-gcab-XXXXXX)
mkdir -p "$out" "$work/rebuild-kit/packaging/sources/gcab-1.6" "$work/rebuild-kit/packaging/sources/public-runtime"
exec > >(tee "$out/build.log") 2>&1
set -x
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR CONFIG_SITE
export LC_ALL=C SOURCE_DATE_EPOCH=1791417600
printf '%s\n' "$work" > "$out/work-directory.txt"
printf '%s  %s\n' 2f0c9615577c4126909e251f9de0626c3ee7a152376c15b5544df10fc87e560b "$root/packaging/sources/gcab-1.6/gcab-1.6.tar.xz" | sha256sum -c -
gcc --version
meson --version
ninja --version
pkg-config --modversion gio-2.0 zlib
cp "$root/packaging/sources/gcab-1.6/gcab-1.6.tar.xz" "$work/rebuild-kit/packaging/sources/gcab-1.6/"
cp "$root/packaging/sources/public-runtime/build-gcab.sh" "$work/rebuild-kit/packaging/sources/public-runtime/"
tar -xf "$root/packaging/sources/gcab-1.6/gcab-1.6.tar.xz" -C "$work"
# Only unshipped developer bindings/docs/test binaries are omitted. NLS and all
# library cabinet compression/decompression support retain upstream defaults.
CC=gcc CFLAGS='-O2 -fPIC -MD' meson setup "$work/build" "$work/gcab-1.6" --prefix=/usr --libdir=lib --buildtype=release --default-library=shared -Ddocs=false -Dintrospection=false -Dvapi=false -Dtests=false -Dnls=true
ninja -C "$work/build" -v -j"${JOBS:-4}"
DESTDIR="$work/prefix" ninja -C "$work/build" install
cp -a "$work/prefix" "$out/"
readelf -dW "$out/prefix/usr/lib/libgcab-1.0.so.0.3.0"
# The build evidence includes unmodified full source, generated enum/config
# sources, all objects/commands, installed files and standalone rebuild inputs.
tar -cJf "$out/build-evidence.tar.xz" -C "$work" gcab-1.6 build prefix rebuild-kit
sha256sum "$out/prefix/usr/lib/libgcab-1.0.so.0.3.0" "$out/build-evidence.tar.xz"
printf 'Built GCAB; retained source/build directory: %s\n' "$work"
