#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Owned replacement recipe, not a reconstruction of CachyOS recipe 03509cd2.
set -euo pipefail
root=$(realpath "${1:?Pass the project root}")
materials="$root/packaging/licenses/libgcc"
out="$root/.work/github-publication/public-gcc-runtime"
[[ $(uname -m) == x86_64 ]] || { echo 'This recipe targets x86_64 GNU/Linux.' >&2; exit 1; }
[[ ${JOBS:-4} =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer.' >&2; exit 1; }
for command in gcc g++ make patch tar sed readelf sha256sum bison flex makeinfo ld xz tee realpath uname mktemp mkdir cp install ar as ranlib nm; do
  command -v "$command" >/dev/null || { echo "Missing build tool: $command" >&2; exit 1; }
done
# A colon-free work root is necessary for GCC search-path variables and make.
# Preserve the work tree on success or failure for source/generated-file review.
work=$(mktemp -d /tmp/pusu-public-gcc-runtime-XXXXXX)
mkdir -p "$out" "$work/build" "$work/rebuild-kit/packaging/licenses"
exec > >(tee "$out/build.log") 2>&1
trap 'echo "GCC runtime build failed at line $LINENO; retained work: $work" >&2' ERR
set -x
printf '%s\n' "$work" > "$out/work-directory.txt"
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR CONFIG_SITE GCC_EXEC_PREFIX COMPILER_PATH MAKEFLAGS
export LC_ALL=C SOURCE_DATE_EPOCH=1791417600
export CC=gcc CXX=g++
# Explicit prospective controls, based on the frozen v3 reference rather than
# guessed original flags. SSP/PIE defaults also come from configure below.
export CFLAGS='-march=x86-64-v3 -mtune=generic -O3 -g -pipe -fno-plt -fexceptions -Wp,-D_FORTIFY_SOURCE=3 -Wformat -fstack-clash-protection -fcf-protection -mpclmul'
export CXXFLAGS="$CFLAGS -Wp,-D_GLIBCXX_ASSERTIONS"
export ASFLAGS='-D__AVX__=1 -D__AVX2__=1 -msse2avx -D__FMA__=1'
export LDFLAGS='-Wl,-O1 -Wl,--sort-common -Wl,--as-needed -Wl,-z,relro -Wl,-z,now -Wl,-z,pack-relative-relocs -Wl,-z,max-page-size=0x1000 -Wl,--build-id'
archive=gcc-d564253eb6c859e266d3cae18e82fb4db9a88316.tar.gz
source=gcc-d564253eb6c859e266d3cae18e82fb4db9a88316
printf '%s  %s\n' 263fcf4d3c770bfd8b724a9b64c8a35fd50dcf669f4fb8736b00db53c552e067 "$materials/source/$archive" | sha256sum -c -
printf '%s  %s\n' 838e14d4ef76079107a5373316598025915eaf747a10522ec9f8842125ca9a8e "$materials/recipe-source/tune_branch_prediction_cost.patch" | sha256sum -c -
printf '%s  %s\n' 0ffc214920e8a76c46fe4eeeb16ce15677b79d8e4351d82f2c72530d55fdbdf3 "$materials/recipe-source/runtime-libdir.patch" | sha256sum -c -
cp -a "$materials" "$work/rebuild-kit/packaging/licenses/libgcc"
gcc --version
g++ --version
ld --version
make --version
bison --version
flex --version
makeinfo --version
tar -xf "$materials/source/$archive" -C "$work"
cd "$work/$source"
# Exact candidate runtime layout transformation and compiler tuning input.
# The Ada-only timestamp patch and c89/c99 launchers do not affect these targets.
patch --batch --forward --fuzz=0 -Np1 < "$materials/recipe-source/runtime-libdir.patch"
patch --batch --forward -Np1 < "$materials/recipe-source/tune_branch_prediction_cost.patch"
cd "$work/build"
"$work/$source/configure" \
  --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-gnu --target=x86_64-pc-linux-gnu \
  --prefix=/usr --libdir=/usr/lib --libexecdir=/usr/lib --with-slibdir=/usr/lib \
  --mandir=/usr/share/man --infodir=/usr/share/info \
  --with-bugurl=https://github.com/sergen213/pusu-uyanis-linux/issues \
  --with-gcc-major-version-only --with-linker-hash-style=gnu --with-system-zlib \
  --enable-languages=c,c++,lto --disable-bootstrap --disable-multilib \
  --enable-cet=auto --enable-checking=release --enable-clocale=gnu \
  --enable-default-pie --enable-default-ssp --enable-gnu-indirect-function \
  --enable-gnu-unique-object --enable-libstdcxx-backtrace \
  --enable-link-serialization=1 --enable-linker-build-id --enable-lto \
  --enable-plugin --enable-shared --enable-threads=posix --enable-libgomp \
  --disable-fixincludes --disable-libssp --disable-libstdcxx-pch --disable-werror
# Upstream configure-target-libgcc depends on all-gcc (generated target headers,
# machine modes and GCC internals). Build only the C/C++/LTO compiler prerequisite,
# not a bootstrapped/PGO/full-language compiler distribution or libgccjit.
make -O -j"${JOBS:-4}" all-gcc
make -O -j"${JOBS:-4}" \
  CFLAGS_FOR_TARGET="$CFLAGS" CXXFLAGS_FOR_TARGET="$CXXFLAGS" \
  LDFLAGS_FOR_TARGET="$LDFLAGS" \
  all-target-libgcc all-target-libgomp all-target-libstdc++-v3
# DESTDIR applies to every installation; the build never writes into host /usr.
make -O -j"${JOBS:-4}" DESTDIR="$work/prefix" \
  install-target-libgcc install-target-libgomp install-target-libstdc++-v3
for library in libgcc_s.so.1 libgomp.so.1 libstdc++.so.6; do
  test -f "$work/prefix/usr/lib/$library"
  readelf -dW "$work/prefix/usr/lib/$library"
  readelf -VW "$work/prefix/usr/lib/$library"
  sha256sum "$work/prefix/usr/lib/$library"
done
# One boundary-check source, exercised against both supported string ABIs.
for abi in 0 1; do
  g++ -std=c++17 -O2 -g -fexceptions -fstack-clash-protection -fcf-protection \
    -D_GLIBCXX_USE_CXX11_ABI="$abi" -pthread -fopenmp "$materials/runtime-check.cpp" \
    -L"$work/prefix/usr/lib" -Wl,-rpath,"$work/prefix/usr/lib" -ldl \
    -o "$work/runtime-check-$abi"
  PUSU_RUNTIME_PREFIX="$work/prefix/usr/lib" LD_LIBRARY_PATH="$work/prefix/usr/lib" \
    "$work/runtime-check-$abi"
done
# Keep original notices in source, plus standalone binary-recipient notices.
mkdir -p "$work/prefix/usr/share/licenses/pusu-gcc-runtime"
cp -a "$materials/upstream" "$work/prefix/usr/share/licenses/pusu-gcc-runtime/"
cp "$materials/RUNTIME-SOURCE-NOTICE.txt" "$materials/source/runtime-support-notice-index.json" \
  "$work/prefix/usr/share/licenses/pusu-gcc-runtime/"
cp -a "$work/prefix" "$out/"
# Source, patched source state, generated configuration/headers, commands,
# compiler prerequisite, runtime objects and recipient rebuild kit remain real.
tar -cJf "$out/build-evidence.tar.xz" -C "$work" "$source" build prefix rebuild-kit runtime-check-0 runtime-check-1
sha256sum "$out/build-evidence.tar.xz"
printf 'Owned GCC runtime outputs: %s\nRetained build tree: %s\n' "$out/prefix/usr/lib" "$work"
