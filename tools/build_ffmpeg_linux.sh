#!/usr/bin/env bash
# build_ffmpeg_linux: tools/build_ffmpeg.sh's trimmed FFmpeg, for Linux: the
# same version, decoders and demuxers, as libavcodec.so.62 and the rest, into
# deps/ffmpeg-<version>-band3-linux-amd64.zip with their headers and licenses
# (cmake/ffmpeg.cmake). band3 carries its own because distributions ship
# different FFmpeg major versions (Ubuntu 24.04's 6, SteamOS's 7), and band3
# reads FFmpeg's structures, so it runs only with the one its headers are.
#
# Run on the oldest Linux band3 is built on (the libraries need its glibc or
# newer), with gcc, make, nasm, meson and ninja:
#   bash tools/build_ffmpeg_linux.sh <work folder>
# The sources are downloaded into the work folder and checked against the
# hashes below.

set -euo pipefail

FFMPEG_VERSION=8.1.3
FFMPEG_SHA256=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
DAV1D_VERSION=1.5.1
DAV1D_SHA256=fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695
NAME=ffmpeg-$FFMPEG_VERSION-band3-linux-amd64

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mkdir -p "${1:?usage: build_ffmpeg_linux.sh <work folder>}" && cd "$1" && pwd)
PREFIX="$WORK/prefix"

fetch() {  # url file sha256
    if [ ! -f "$WORK/$2" ]; then curl -fsSL -o "$WORK/$2" "$1"; fi
    echo "$3  $WORK/$2" | sha256sum -c --quiet
}
fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" \
      "ffmpeg-$FFMPEG_VERSION.tar.xz" "$FFMPEG_SHA256"
fetch "https://code.videolan.org/videolan/dav1d/-/archive/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.gz" \
      "dav1d-$DAV1D_VERSION.tar.gz" "$DAV1D_SHA256"
rm -rf "$WORK/src" "$PREFIX"
mkdir -p "$WORK/src" "$PREFIX"
tar -xf "$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz" -C "$WORK/src"
tar -xf "$WORK/dav1d-$DAV1D_VERSION.tar.gz" -C "$WORK/src"

# dav1d, static (position-independent), into avcodec
cd "$WORK/src/dav1d-$DAV1D_VERSION"
meson setup build --prefix="$PREFIX" --libdir=lib --buildtype=release \
    --default-library=static -Db_staticpic=true \
    -Denable_tools=false -Denable_tests=false -Denable_examples=false
meson install -C build

# FFmpeg: build_ffmpeg.sh's configure line, with pthreads for w32threads.
# Each library finds the others beside it ($ORIGIN), wherever band3 is
cd "$WORK/src/ffmpeg-$FFMPEG_VERSION"
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" ./configure --prefix="$PREFIX" \
    --enable-shared --disable-static --disable-debug --disable-doc --disable-programs \
    --disable-everything --disable-network --disable-autodetect --enable-pthreads \
    --disable-avdevice --disable-avfilter \
    --enable-avcodec --enable-avformat --enable-swscale --enable-swresample \
    --enable-libdav1d --pkg-config-flags=--static \
    --enable-decoder=h264,hevc,vp8,vp9,libdav1d,mpeg4,msmpeg4v3,mpeg1video,mpeg2video,vc1,wmv3,opus,aac,vorbis,mp3float,flac \
    --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,vc1,opus,aac,vorbis,mpegaudio,flac \
    --enable-demuxer=matroska,mov,avi,asf,mpegts,mpegps,m4v,h264,hevc,ivf \
    --enable-protocol=file
# the runpath set past configure, which would expand $ORIGIN: the link rule's two expansions
# by make turn $$$$ into $, and the quotes keep the shell off it
sed -i "s/^LDSOFLAGS=.*/LDSOFLAGS=-Wl,-rpath,'\$\$\$\$ORIGIN'/" ffbuild/config.mak
make -j"$(nproc)"
make install

# the zip: the libraries under their sonames (stripped), the headers, the licenses
OUT="$WORK/$NAME"
rm -rf "$OUT"
mkdir -p "$OUT/lib"
for lib in avutil swresample avcodec avformat swscale; do
    soname=$(readelf -d "$PREFIX/lib/lib$lib.so" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
    cp -L "$PREFIX/lib/lib$lib.so" "$OUT/lib/$soname"
done
strip --strip-unneeded "$OUT"/lib/*.so.*
cp -r "$PREFIX/include" "$OUT/include"
rm -rf "$OUT/include/dav1d"
cp COPYING.LGPLv2.1 "$OUT/COPYING.LGPLv2.1"
cp "$WORK/src/dav1d-$DAV1D_VERSION/COPYING" "$OUT/COPYING.dav1d"
cat > "$OUT/LICENSE.txt" <<EOF
FFmpeg $FFMPEG_VERSION (https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz), LGPL 2.1 or
later (COPYING.LGPLv2.1), with dav1d $DAV1D_VERSION
(https://code.videolan.org/videolan/dav1d/-/archive/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.gz,
BSD 2-clause, COPYING.dav1d) built into avcodec: the avcodec, avformat, avutil, swresample and
swscale libraries beside band3, built unmodified by band3's tools/build_ffmpeg_linux.sh, which
has the configure line.
EOF
mkdir -p "$REPO/deps"
rm -f "$REPO/deps/$NAME.zip"
(cd "$WORK" && python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[2])" \
    "$REPO/deps/$NAME" "$NAME")
ls -la "$OUT/lib" "$REPO/deps/$NAME.zip"
