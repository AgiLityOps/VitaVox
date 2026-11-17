#!/bin/sh
# Cross-compile a small LGPL FFmpeg for PlayStation Vita (no GPL, no encoders/muxers/network).
# Based on the official vitasdk/packages ffmpeg VITABUILD (flags trimmed to a decoder-only set).
# Usage: scripts/build_ffmpeg.sh [prefix]
# Default prefix: <repo>/third_party/ffmpeg
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PREFIX=${1:-"$ROOT/third_party/ffmpeg"}
SRC_ROOT=${FFMPEG_SRC_ROOT:-/tmp/ffmpeg-build}
FFMPEG_VER=${FFMPEG_VER:-9.0.2}
SRC="$SRC_ROOT/ffmpeg-$FFMPEG_VER"

if [ -z "${VITASDK:-}" ]; then
  echo "VITASDK is not set" >&2
  exit 1
fi

NPROC=$(nproc 2>/dev/null || echo 2)
CROSS=$VITASDK/bin/arm-vita-eabi-
VITA_INC=$VITASDK/arm-vita-eabi/include
VITA_LIB=$VITASDK/arm-vita-eabi/lib

mkdir -p "$SRC_ROOT"
if [ ! -f "$SRC/configure" ]; then
  tarball="$SRC_ROOT/ffmpeg-$FFMPEG_VER.tar.xz"
  if [ ! -f "$tarball" ]; then
    echo "Downloading FFmpeg $FFMPEG_VER..."
    curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz" -o "$tarball"
  fi
  # Official VITABUILD sha256 for 9.0.2
  echo "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e  $tarball" | sha256sum -c -
  tar -C "$SRC_ROOT" -xf "$tarball"
fi

# Optional decoders: enable only if this tree lists them.
optional_decoders="mp3float aac_latm"
enabled_optional=""
list=$("$SRC/configure" --list-decoders 2>/dev/null || true)
for d in $optional_decoders; do
  if echo "$list" | grep -qw "$d"; then
    enabled_optional="$enabled_optional,$d"
  else
    echo "Skipping decoder $d (not in this FFmpeg)"
  fi
done

# Native decoders only — no libmp3lame / libvorbis / libopus / libflac (keeps the build LGPL
# and free of those extra Vita packages).
DECODERS="aac,alac,mp3,flac,vorbis,opus,wmav1,wmav2,wmapro,wmalossless,ape,wavpack,tta,pcm_s8,pcm_u8,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be${enabled_optional}"
DEMUXERS="mov,mp3,aac,flac,ogg,matroska,wav,aiff,asf,ape,wv,caf,tta"
PARSERS="aac,mpegaudio,flac,opus,vorbis"

CFLAGS="-std=gnu11 -fno-pic -fno-pie -mfpu=neon -mfloat-abi=hard -ftree-vectorize -fomit-frame-pointer -ffast-math -O2 -D_BSD_SOURCE -I$VITA_INC -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types"
LDFLAGS="-L$VITA_LIB"

cd "$SRC"
# Reconfigure in a clean build dir so reruns are cheap.
BUILD="$SRC_ROOT/build-vita"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# Write a tiny pkg-config so --enable-zlib can find the Vita sysroot.
PKG_WRAP=$SRC_ROOT/vita-pkg-config
cat > "$PKG_WRAP" << 'PC'
#!/bin/sh
# Minimal pkg-config for the Vita sysroot (zlib).
if [ "${1:-}" = "--version" ]; then echo "0.29.2"; exit 0; fi
sysroot=${VITASDK}/arm-vita-eabi
mod=""
flags=""
for a in "$@"; do
  case "$a" in
    --exists) flags=exists ;;
    --cflags) flags=cflags ;;
    --libs) flags=libs ;;
    --modversion) flags=modversion ;;
    -*) ;;
    *) mod=$a ;;
  esac
done
case "$mod" in
  zlib|libz)
    case "$flags" in
      exists) exit 0 ;;
      cflags) echo "-I${sysroot}/include"; exit 0 ;;
      libs) echo "-L${sysroot}/lib -lz"; exit 0 ;;
      modversion) echo "1.3.2"; exit 0 ;;
    esac
    ;;
esac
exit 1
PC
chmod +x "$PKG_WRAP"
export PKG_CONFIG="$PKG_WRAP"

echo "Configuring FFmpeg $FFMPEG_VER for Vita -> $PREFIX"
"$SRC/configure" \
  --prefix="$PREFIX" \
  --enable-cross-compile \
  --cross-prefix="$CROSS" \
  --cc="${CROSS}gcc" \
  --ar="${CROSS}ar" \
  --nm="${CROSS}nm" \
  --ranlib="${CROSS}ranlib" \
  --strip="${CROSS}strip" \
  --pkg-config="$PKG_WRAP" \
  --arch=armv7-a \
  --cpu=cortex-a9 \
  --target-os=none \
  --enable-static \
  --disable-shared \
  # --enable-small shrinks tables only. The float AAC decoder still compiles
  # aacsbr + aacps (HE-AACv1/v2). Do not switch to aac_fixed: that path is a
  # separate decoder and is left disabled. "SBR+21" from FFmpeg is AOT 21
  # (ER TwinVQ), not parametric stereo.
  --enable-small \
  --enable-thumb \
  --disable-runtime-cpudetect \
  --disable-armv5te \
  --disable-armv6t2 \
  --disable-programs \
  --disable-doc \
  --disable-htmlpages \
  --disable-manpages \
  --disable-podpages \
  --disable-txtpages \
  --disable-network \
  --disable-debug \
  --disable-autodetect \
  --enable-zlib \
  --enable-pthreads \
  --disable-everything \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swscale \
  --enable-decoder="$DECODERS" \
  --enable-demuxer="$DEMUXERS" \
  --enable-parser="$PARSERS" \
  --enable-protocol=file \
  --disable-bzlib \
  --disable-iconv \
  --disable-lzma \
  --disable-sdl2 \
  --disable-xlib \
  --disable-securetransport \
  --extra-cflags="$CFLAGS" \
  --extra-ldflags="$LDFLAGS" \
  --extra-libs="-lz -lpthread -lm"

echo "Building..."
make -j"$NPROC"
# Fresh prefix
rm -rf "$PREFIX"
make install

# Record the config and sizes
{
  echo "FFmpeg $FFMPEG_VER (LGPL, Vita cross-build)"
  echo "prefix: $PREFIX"
  echo "decoders: $DECODERS"
  echo "demuxers: $DEMUXERS"
  echo "parsers: $PARSERS"
  echo
  if [ -f ffbuild/config.log ]; then
    rg -n "NEON|neon|HAVE_NEON|--enable-neon" ffbuild/config.h config.h 2>/dev/null | head -40 || true
  fi
  echo
  echo "Static libraries:"
  ls -l "$PREFIX"/lib/*.a
} | tee "$PREFIX/BUILD_INFO.txt"

echo "Installed to $PREFIX"
