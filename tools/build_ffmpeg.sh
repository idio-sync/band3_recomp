#!/usr/bin/env bash
# build_ffmpeg: the trimmed FFmpeg band3's music videos decode with on Windows
# (cmake/ffmpeg.cmake): its avcodec, avformat, avutil, swresample (Opus needs it)
# and swscale DLLs, with
# only the decoders and demuxers band3 uses (H.264, HEVC, VP8, VP9, AV1
# through dav1d, MPEG-1/2/4 and VC-1/WMV pictures; Opus, AAC, Vorbis, MP3 and
# FLAC sound, for auto-sync; WebM/MKV, MP4/MOV, AVI, ASF/WMV and MPEG
# streams), LGPL, into deps/ffmpeg-<version>-band3-win64.zip with their
# headers and licenses.
#
# Run from Git Bash on Windows, with MSYS2 installed (MSYS2=, C:/msys64 by
# default: its MinGW64 gcc compiles everything, and its shell runs FFmpeg's
# configure and make), Python 3 with meson importable (MESON_PYTHONPATH= a
# meson wheel works), nasm and ninja (NASM=, NINJA=, or on PATH):
#   NASM=.../nasm.exe NINJA=C:/ninja.exe MESON_PYTHONPATH=.../meson-1.6.1-py3-none-any.whl \
#       bash tools/build_ffmpeg.sh <work folder>
# The sources are downloaded into the work folder and checked against the
# hashes below. dav1d is built from this shell, not MSYS2's: meson's compiler
# checks fail under MSYS2, which hands native programs no usable temp folder.

set -euo pipefail

FFMPEG_VERSION=8.1.3
FFMPEG_SHA256=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
DAV1D_VERSION=1.5.1
DAV1D_SHA256=fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mkdir -p "${1:?usage: build_ffmpeg.sh <work folder>}" && cd "$1" && pwd)
PREFIX="$WORK/prefix"
MSYS2=${MSYS2:-/c/msys64}
PYTHON=${PYTHON:-python}
NASM=${NASM:-nasm}
NINJA=${NINJA:-ninja}
export PATH="$MSYS2/mingw64/bin:$(dirname "$(command -v "$NASM")"):$PATH"

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

# dav1d, static, into avcodec
meson() {
    NINJA="$NINJA" PYTHONPATH="${MESON_PYTHONPATH:-}" "$PYTHON" -m mesonbuild.mesonmain "$@"
}
cd "$WORK/src/dav1d-$DAV1D_VERSION"
meson setup build --prefix="$(cygpath -m "$PREFIX")" --libdir=lib --buildtype=release \
    --default-library=static -Denable_tools=false -Denable_tests=false -Denable_examples=false
meson install -C build

# FFmpeg, in MSYS2's MinGW64 shell (configure needs its tools). Paths go to
# it in Windows form: this shell's /tmp isn't MSYS2's.
WORK_W=$(cygpath -m "$WORK")
NASM_W=$(cygpath -m "$(dirname "$(command -v "$NASM")")")
cat > "$WORK/ffmpeg.sh" <<EOF
set -euo pipefail
WORK=\$(cygpath -u "$WORK_W")
PREFIX="\$WORK/prefix"
export PATH="\$(cygpath -u "$NASM_W"):\$PATH"
# FFmpeg finds dav1d through pkg-config; this answers for the one just built
cat > "\$WORK/pkg-config" <<'PC'
#!/usr/bin/env bash
prefix="\$(dirname "\$0")/prefix"
for a in "\$@"; do
    case "\$a" in
        --modversion) echo $DAV1D_VERSION; exit 0 ;;
        --cflags) echo "-I\$prefix/include"; exit 0 ;;
        --libs) echo "-L\$prefix/lib -ldav1d"; exit 0 ;;
    esac
done
exit 0
PC
chmod +x "\$WORK/pkg-config"
cd "\$WORK/src/ffmpeg-$FFMPEG_VERSION"
./configure --prefix="\$PREFIX" --target-os=mingw32 --arch=x86_64 \
    --enable-shared --disable-static --disable-debug --disable-doc --disable-programs \
    --disable-everything --disable-network --disable-autodetect --enable-w32threads \
    --disable-avdevice --disable-avfilter \
    --enable-avcodec --enable-avformat --enable-swscale --enable-swresample \
    --enable-libdav1d --pkg-config="\$WORK/pkg-config" \
    --enable-decoder=h264,hevc,vp8,vp9,libdav1d,mpeg4,msmpeg4v3,mpeg1video,mpeg2video,vc1,wmv3,opus,aac,vorbis,mp3float,flac \
    --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,vc1,opus,aac,vorbis,mpegaudio,flac \
    --enable-demuxer=matroska,mov,avi,asf,mpegts,mpegps,m4v,h264,hevc,ivf \
    --enable-protocol=file \
    --extra-ldflags="-static-libgcc -static"
make -j\$(nproc)
make install
EOF
MSYSTEM=MINGW64 CHERE_INVOKING=1 "$MSYS2/usr/bin/bash.exe" -l "$WORK_W/ffmpeg.sh"

# the zip: the DLLs (stripped), the headers, the licenses
cd "$WORK/src/ffmpeg-$FFMPEG_VERSION"
OUT="$WORK/ffmpeg-$FFMPEG_VERSION-band3-win64"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
for lib in avutil swresample avcodec avformat swscale; do
    cp "$PREFIX"/bin/$lib-*.dll "$OUT/bin/"
done
strip --strip-unneeded "$OUT"/bin/*.dll
cp -r "$PREFIX/include" "$OUT/include"
rm -rf "$OUT/include/dav1d"
cp COPYING.LGPLv2.1 "$OUT/COPYING.LGPLv2.1"
cp "$WORK/src/dav1d-$DAV1D_VERSION/COPYING" "$OUT/COPYING.dav1d"
cat > "$OUT/LICENSE.txt" <<EOF
FFmpeg $FFMPEG_VERSION (https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz), LGPL 2.1 or
later (COPYING.LGPLv2.1), with dav1d $DAV1D_VERSION
(https://code.videolan.org/videolan/dav1d/-/archive/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.gz,
BSD 2-clause, COPYING.dav1d) built into avcodec: the avcodec, avformat, avutil, swresample and
swscale DLLs
beside band3, built unmodified by band3's tools/build_ffmpeg.sh, which has the configure line.
EOF
mkdir -p "$REPO/deps"
rm -f "$REPO/deps/ffmpeg-$FFMPEG_VERSION-band3-win64.zip"
(cd "$WORK" && "$PYTHON" -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[2])" \
    "$(cygpath -m "$REPO/deps/ffmpeg-$FFMPEG_VERSION-band3-win64")" "ffmpeg-$FFMPEG_VERSION-band3-win64")
ls -la "$OUT/bin" "$REPO/deps/ffmpeg-$FFMPEG_VERSION-band3-win64.zip"
