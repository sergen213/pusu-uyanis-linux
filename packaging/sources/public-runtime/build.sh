#!/bin/bash
# Newly authored recipe: GPL-3.0-only; upstream sources retain their own terms.
set -euo pipefail
root=$(realpath "${1:?Pass the project root}")
consumer_patch=
if [[ -n "${2:-}" ]]; then consumer_patch=$(realpath "$2"); fi
out="$root/.work/github-publication/public-runtime"
work=$(mktemp -d /tmp/pusu-public-runtime-XXXXXX)
prefix="$work/prefix"
mkdir -p "$out" "$prefix" "$work/src"
exec > >(tee "$out/build.log") 2>&1
set -x
# The temporary prefix has no ':' or spaces: PATH/pkg-config/Make cannot represent
# the project's colon-containing path. Retain this directory for local relinking.
printf '%s\n' "$work" > "$out/work-directory.txt"
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH PKG_CONFIG_PATH CONFIG_SITE
export LC_ALL=C SOURCE_DATE_EPOCH=1791417600
for tool in gcc make meson ninja cmake autoconf automake libtoolize pkg-config patch objcopy strip readelf python3; do
    command -v "$tool"
done
gcc --version
ld --version
meson --version
cmake --version
ninja --version
jobs=${JOBS:-4}
# Verify every selected source before extracting or executing upstream scripts.
python3 - "$root" <<'PY'
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1])
inputs = {
 'packaging/sources/appimage-runtime/type2-runtime-20251108.tar.gz': '4c4f6df4647c9f01f871d7edd3716d8aeeffda9d22ffbebe3fccd95a6ab52c95',
 'packaging/sources/appimage-runtime/musl-1.2.5.tar.gz': 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4',
 'packaging/sources/appimage-runtime/fuse-3.15.0.tar.xz': '70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0',
 'packaging/sources/appimage-runtime/squashfuse-0.5.2.tar.gz': 'db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3',
 'packaging/sources/appimage-runtime/zlib-1.3.1.tar.gz': '9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23',
 'packaging/sources/appimage-runtime/zstd-1.5.6.tar.gz': '30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7',
 'packaging/sources/public-runtime/mimalloc-2.1.7.tar.gz': '0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d',
 'packaging/sources/appimage-runtime/build-materials/patches/libfuse/mount.c.diff': '1c7fd9e26717545a476b226b083a9f9d05676c180edbd71a04bbd8a73599dc44',
}
for name, expected in inputs.items():
    with (root / name).open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if actual != expected:
        raise SystemExit(f'Input hash mismatch: {name}')
    print(actual, name)
PY
for archive in "$root"/packaging/sources/appimage-runtime/*.tar.* "$root/packaging/sources/public-runtime/mimalloc-2.1.7.tar.gz"; do
    tar -xf "$archive" -C "$work/src"
done
cd "$work/src/musl-1.2.5"
CC=gcc CFLAGS='-Os -fPIC' ./configure --prefix="$prefix" --disable-shared --enable-wrapper=gcc
make -j"$jobs"
make install
export CC="$prefix/bin/musl-gcc"
export CFLAGS='-Os -fPIC -ffunction-sections -fdata-sections -MD'
export CPPFLAGS="-I$prefix/include"
export LDFLAGS="-static -L$prefix/lib"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
cd "$work/src/zlib-1.3.1"
./configure --prefix="$prefix" --static
make -j"$jobs"
make install
make -C "$work/src/zstd-1.5.6/lib" -j"$jobs" libzstd.a ZSTD_LEGACY_SUPPORT=1
install -m644 "$work/src/zstd-1.5.6/lib/libzstd.a" "$prefix/lib/"
install -m644 "$work/src/zstd-1.5.6/lib/zstd.h" "$work/src/zstd-1.5.6/lib/zstd_errors.h" "$prefix/include/"
cd "$work/src/fuse-3.15.0"
patch -p1 < "$root/packaging/sources/appimage-runtime/build-materials/patches/libfuse/mount.c.diff"
patch -p1 < "$root/packaging/sources/public-runtime/libfuse-modification-notice.patch"
# LGPL recipient path: optionally patch the retained library source and relink
# the application against that changed library using the same complete recipe.
if [[ -n "$consumer_patch" ]]; then
    patch -p1 < "$consumer_patch"
    cp "$consumer_patch" "$work/consumer-libfuse.patch"
fi
meson setup build --prefix="$prefix" --libdir=lib --buildtype=minsize --default-library=static -Dutils=false -Dexamples=false -Dtests=false -Duseroot=false -Ddisable-libc-symbol-version=true
ninja -C build -v
ninja -C build install
cd "$work/src/squashfuse-0.5.2"
./autogen.sh
./configure --prefix="$prefix" --enable-static --disable-shared --with-zlib="$prefix" --with-zstd="$prefix" --without-xz --without-lzo --without-lz4
make -j"$jobs" V=1
make install
mkdir -p "$prefix/include/squashfuse"
install -m644 ./*.h "$prefix/include/squashfuse/"
cmake -S "$work/src/mimalloc-2.1.7" -B "$work/mimalloc-build" -G Ninja -DCMAKE_C_COMPILER="$CC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DMI_INSTALL_TOPLEVEL=ON -DMI_LIBC_MUSL=ON -DMI_SECURE=ON -DMI_OVERRIDE=ON -DMI_BUILD_SHARED=OFF -DMI_BUILD_STATIC=ON -DMI_BUILD_OBJECT=OFF -DMI_BUILD_TESTS=OFF
cmake --build "$work/mimalloc-build" --parallel "$jobs" --verbose
cmake --install "$work/mimalloc-build"
# The musl-gcc wrapper appends musl specs after all user arguments. Invoke GCC
# directly so musl headers/libs are selected first and PIE startup/link specs
# override them last, omitting host GCC CRT/support objects from this runtime.
cat > "$work/static-pie.specs" <<EOF
*startfile:
$prefix/lib/rcrt1.o $prefix/lib/crti.o

*endfile:
$prefix/lib/crtn.o

*libgcc:
%{!static:-lgcc}

*link:
-static -pie --no-dynamic-linker -nostdlib
EOF
# Keep the upstream target/link recipe, but remove hardcoded host FUSE include
# paths so every dependency header comes from the owned musl/static prefix.
patch -d "$work/src/type2-runtime-20251108" -p1 < "$root/packaging/sources/public-runtime/runtime-includes.patch"
# Checked portable suffix construction preserves HOME/config behavior and fails
# closed for truncated readlink results or an overlong appended directory path.
patch -d "$work/src/type2-runtime-20251108" -p1 < "$root/packaging/sources/public-runtime/runtime-portable-path.patch"
cd "$work/src/type2-runtime-20251108/src/runtime"
printf '%s\n' 'dd6cebed-public-owned-20261008' > version
make CC="gcc -specs=$prefix/lib/musl-gcc.specs -specs=$work/static-pie.specs" CFLAGS="-std=gnu99 -Os -D_FILE_OFFSET_BITS=64 -DGIT_COMMIT=\\\"dd6cebed-public-owned-20261008\\\" -I$prefix/include -I$prefix/include/fuse3 -T data_sections.ld -ffunction-sections -fdata-sections -Wl,--gc-sections -static -static-pie -fPIE -Wall -Werror -save-temps=obj -MD -Wl,-Map,$work/runtime.map" LIBS="-L$prefix/lib -lsquashfuse -lsquashfuse_ll -lzstd -lz -lfuse3 -lmimalloc-secure -lpthread -ldl -lrt -lm"
cp runtime "$out/runtime-x86_64.unstripped"
objcopy --only-keep-debug runtime "$out/runtime-x86_64.debug"
strip --strip-debug --strip-unneeded runtime
cp runtime "$out/runtime-x86_64"
objcopy --add-gnu-debuglink="$out/runtime-x86_64.debug" "$out/runtime-x86_64"
printf 'AI\002' | dd of="$out/runtime-x86_64" bs=1 count=3 seek=8 conv=notrunc
# Runnable fail-closed check: native type2 marker, PIE and no loader/DT_NEEDED.
python3 - "$out/runtime-x86_64" <<'PY'
import pathlib, struct, subprocess, sys
path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
assert data[:4] == b'\x7fELF' and data[8:11] == b'AI\x02'
assert struct.unpack_from('<H', data, 16)[0] == 3
headers = subprocess.check_output(['readelf', '-lW', str(path)], text=True)
dynamic = subprocess.check_output(['readelf', '-dW', str(path)], text=True)
assert 'INTERP' not in headers and '(NEEDED)' not in dynamic
PY
cp "$work/runtime.map" "$out/runtime.map"
cp "$work/src/mimalloc-2.1.7/LICENSE" "$out/mimalloc-LICENSE"
# Standalone source layout: after extracting the evidence archive, enter
# rebuild-kit and run bash packaging/sources/public-runtime/build.sh "$PWD".
mkdir -p "$work/rebuild-kit/packaging/sources/appimage-runtime/build-materials/patches/libfuse" "$work/rebuild-kit/packaging/sources/public-runtime"
cp "$root"/packaging/sources/appimage-runtime/*.tar.* "$work/rebuild-kit/packaging/sources/appimage-runtime/"
cp "$root/packaging/sources/appimage-runtime/build-materials/patches/libfuse/mount.c.diff" "$work/rebuild-kit/packaging/sources/appimage-runtime/build-materials/patches/libfuse/"
cp "$root"/packaging/sources/public-runtime/{build.sh,runtime-includes.patch,runtime-portable-path.patch,libfuse-modification-notice.patch,mimalloc-2.1.7.tar.gz} "$work/rebuild-kit/packaging/sources/public-runtime/"
if [[ -n "$consumer_patch" ]]; then
    cp "$work/consumer-libfuse.patch" "$work/rebuild-kit/consumer-libfuse.patch"
fi
# Full configured library/application sources, generated files, object files,
# static archives, specs, linker map and exact commands: usable LGPL relinking.
tar -cJf "$out/build-evidence.tar.xz" -C "$work" src prefix mimalloc-build static-pie.specs runtime.map rebuild-kit
sha256sum "$out/runtime-x86_64" "$out/build-evidence.tar.xz" "$out/mimalloc-LICENSE"
printf 'Built runtime; retained local relinking directory: %s\n' "$work"
