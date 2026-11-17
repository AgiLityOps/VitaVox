#!/usr/bin/env bash
# Idempotent host + VitaSDK toolchain installer for Vita Vox.
#
# Reproduces the toolchain used to build v0.2.x VPKs on this box:
#   - CMake 3.31.8 official linux-x86_64 binary
#   - GNU make 4.4.1 from source (apt on this image often has empty/unsigned lists)
#   - bzip2 1.0.8 from source (vdpm seed archives are .tar.bz2)
#   - zstd 1.5.7 from source (optional; used to write the restore archive)
#   - VitaSDK via vdpm, channel 2026.08, toolchain sdk-snapshot-20260825.611.1
#   - Vita packages from packages-2026.08-snapshot-20261115.68.1
#     (the later 20261116.70.1 snapshot is incomplete: a ~7 KiB vita.db
#     that omits zlib, libpng, libvita2d, freetype, libjpeg-turbo, bzip2)
#
# Usage:
#   scripts/setup_toolchain.sh              # install or no-op if already present
#   scripts/setup_toolchain.sh --archive    # also write $CACHE/vitasdk.tar.zst
#
# Environment (all optional):
#   VITASDK   default /usr/local/vitasdk
#   CACHE     default /workspace/cache
#   SRC       download/build scratch, default /tmp/toolchain-src
#   PREFIX    host prefix for cmake/make/bzip2/zstd, default /usr/local
#
# After a successful run:
#   export VITASDK=/usr/local/vitasdk
#   export PATH="$VITASDK/bin:/usr/local/bin:$PATH"
# Cross-build Vita FFmpeg only if third_party/ffmpeg is missing:
#   ./scripts/build_ffmpeg.sh
#
# This project vendors cJSON and does not link libjansson. The official
# jansson package is PIC (R_ARM_GOT32). Rebuild it with -fno-pic only if
# vita-elf-create fails with R_ARM_BASE_PREL; pass --jansson-nopic then.
set -euo pipefail

PREFIX=${PREFIX:-/usr/local}
VITASDK=${VITASDK:-$PREFIX/vitasdk}
CACHE=${CACHE:-/workspace/cache}
SRC=${SRC:-/tmp/toolchain-src}
CHANNEL=${VITASDK_CHANNEL:-2026.08}
PACKAGES_SNAPSHOT=${PACKAGES_SNAPSHOT:-packages-2026.08-snapshot-20261115.68.1}
PACKAGES_BASE="https://github.com/vitasdk/vitasdk-autobuild/releases/download/${PACKAGES_SNAPSHOT}"
# Official 68.1 databases (pacman -Sy against this Server can still drop a
# 6964-byte incomplete vita.db; always install these files when zlib is missing).
VITA_DB_SHA256=b84bc710027f5c96b024c05c4d1b74b3a11133085a825681219201059fbd5b9d
VITA_FILES_SHA256=ce92ebb25f5a76b11c6aa4bb66373469e720468f77d8df3dd898e4e50481e525
VITA_DB_SIZE=18251

CMAKE_VER=3.31.8
CMAKE_SHA256=630615d8e98ac33eba7fbe472626dff5c899c85af3c024585ae109166a6909d0
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VER}/cmake-${CMAKE_VER}-linux-x86_64.tar.gz"

MAKE_VER=4.4.1
MAKE_SHA256=dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3
MAKE_URL="https://ftp.gnu.org/gnu/make/make-${MAKE_VER}.tar.gz"

BZIP2_VER=1.0.8
BZIP2_SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
BZIP2_URL="https://sourceware.org/pub/bzip2/bzip2-${BZIP2_VER}.tar.gz"

ZSTD_VER=1.5.7
ZSTD_SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
ZSTD_URL="https://github.com/facebook/zstd/releases/download/v${ZSTD_VER}/zstd-${ZSTD_VER}.tar.gz"

VITA_PACKAGES=(zlib bzip2 libpng libjpeg-turbo freetype libvita2d jansson)

DO_ARCHIVE=0
JANSSON_NOPIC=0
for arg in "$@"; do
  case "$arg" in
    --archive) DO_ARCHIVE=1 ;;
    --jansson-nopic) JANSSON_NOPIC=1 ;;
    -h|--help)
      sed -n '2,40p' "$0"
      exit 0
      ;;
    *)
      echo "unknown argument: $arg" >&2
      exit 2
      ;;
  esac
done

export PATH="$VITASDK/bin:$PREFIX/bin:$PREFIX/cmake-${CMAKE_VER}-linux-x86_64/bin:$PATH"
export VITASDK
mkdir -p "$SRC"

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# sudo on this box sanitizes PATH; always invoke tools by absolute path.
run_root() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
    sudo --preserve-env=VITASDK,VITASDK_CHANNEL,PATH "$@"
  else
    die "need root (or passwordless sudo) to install into $PREFIX"
  fi
}

install_file() {
  # install_file src dest [mode]
  local src=$1 dest=$2 mode=${3:-0755}
  run_root install -D -m "$mode" "$src" "$dest"
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

sha256_ok() {
  printf '%s  %s\n' "$1" "$2" | sha256sum -c -
}

download() {
  # download url dest sha256
  local url=$1 dest=$2 sha=$3
  if [ -f "$dest" ] && sha256_ok "$sha" "$dest" >/dev/null 2>&1; then
    return 0
  fi
  log "downloading $url"
  curl -fL --retry 3 --retry-delay 2 --max-time 180 -o "$dest.part" "$url"
  sha256_ok "$sha" "$dest.part"
  mv "$dest.part" "$dest"
}

# Best-effort apt. This image's apt lists are often empty or unsigned; ignore failure.
try_apt() {
  command -v apt-get >/dev/null 2>&1 || return 1
  if run_root apt-get update -qq && \
     run_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@"; then
    return 0
  fi
  log "apt install failed; falling back to official/source tarballs"
  return 1
}

cmd_version() {
  # first token of `$1 --version`, e.g. cmake / make / zstd
  local bin=$1
  command -v "$bin" >/dev/null 2>&1 || return 1
  "$bin" --version 2>/dev/null | head -n1 | tr -d '\r'
}

ensure_cmake() {
  if cmake --version 2>/dev/null | grep -q "cmake version ${CMAKE_VER}"; then
    log "cmake ${CMAKE_VER} already present: $(command -v cmake)"
    return 0
  fi
  try_apt cmake || true
  if cmake --version 2>/dev/null | grep -q "cmake version ${CMAKE_VER}"; then
    return 0
  fi
  # A different cmake from apt is not the pinned binary; install alongside.
  local tarball="$SRC/cmake-${CMAKE_VER}-linux-x86_64.tar.gz"
  download "$CMAKE_URL" "$tarball" "$CMAKE_SHA256"
  log "installing cmake ${CMAKE_VER} into $PREFIX"
  run_root tar -C "$PREFIX" -xzf "$tarball"
  local bindir="$PREFIX/cmake-${CMAKE_VER}-linux-x86_64/bin"
  for b in cmake cpack ctest; do
    run_root ln -sfn "$bindir/$b" "$PREFIX/bin/$b"
  done
  cmake --version | head -n1
}

ensure_make() {
  if make --version 2>/dev/null | grep -q "GNU Make ${MAKE_VER}"; then
    log "make ${MAKE_VER} already present: $(command -v make)"
    return 0
  fi
  try_apt make || true
  if make --version 2>/dev/null | grep -q "GNU Make ${MAKE_VER}"; then
    return 0
  fi
  local tarball="$SRC/make-${MAKE_VER}.tar.gz"
  download "$MAKE_URL" "$tarball" "$MAKE_SHA256"
  log "building GNU make ${MAKE_VER}"
  rm -rf "$SRC/make-${MAKE_VER}"
  tar -C "$SRC" -xzf "$tarball"
  (
    cd "$SRC/make-${MAKE_VER}"
    # configure's dependency-tracking bootstrap needs an existing make.
    # ./build.sh does not.
    if command -v make >/dev/null 2>&1; then
      ./configure --prefix="$PREFIX" --disable-dependency-tracking
      make -j"$(nproc 2>/dev/null || echo 2)"
      if ! run_root make install; then
        log "make install failed; installing the binary only"
        install_file make "$PREFIX/bin/make" 0755
      fi
    else
      chmod +x ./build.sh
      ./build.sh
      install_file make "$PREFIX/bin/make" 0755
    fi
  )
  run_root ln -sfn "$PREFIX/bin/make" "$PREFIX/bin/gmake"
  make --version | head -n1
}

ensure_bzip2() {
  if command -v bzip2 >/dev/null 2>&1 && command -v bunzip2 >/dev/null 2>&1; then
    log "bzip2 already present: $(command -v bzip2)"
    return 0
  fi
  try_apt bzip2 || true
  if command -v bzip2 >/dev/null 2>&1 && command -v bunzip2 >/dev/null 2>&1; then
    return 0
  fi
  local tarball="$SRC/bzip2-${BZIP2_VER}.tar.gz"
  download "$BZIP2_URL" "$tarball" "$BZIP2_SHA256"
  log "building bzip2 ${BZIP2_VER}"
  rm -rf "$SRC/bzip2-${BZIP2_VER}"
  tar -C "$SRC" -xzf "$tarball"
  (
    cd "$SRC/bzip2-${BZIP2_VER}"
    make -j"$(nproc 2>/dev/null || echo 2)"
    run_root make install PREFIX="$PREFIX"
  )
  bzip2 --help >/dev/null
  log "installed bzip2 ${BZIP2_VER}"
}

ensure_zstd() {
  if command -v zstd >/dev/null 2>&1; then
    log "zstd already present: $(command -v zstd)"
    return 0
  fi
  try_apt zstd || true
  if command -v zstd >/dev/null 2>&1; then
    return 0
  fi
  local tarball="$SRC/zstd-${ZSTD_VER}.tar.gz"
  download "$ZSTD_URL" "$tarball" "$ZSTD_SHA256"
  log "building zstd ${ZSTD_VER}"
  rm -rf "$SRC/zstd-${ZSTD_VER}"
  tar -C "$SRC" -xzf "$tarball"
  (
    cd "$SRC/zstd-${ZSTD_VER}"
    make -j"$(nproc 2>/dev/null || echo 2)" zstd
    run_root make PREFIX="$PREFIX" install
  )
  zstd --version
}

vdpm_bin() {
  if [ -x "$VITASDK/bin/vdpm" ]; then
    printf '%s\n' "$VITASDK/bin/vdpm"
  else
    die "vdpm not found under $VITASDK/bin"
  fi
}

vdpm_run() {
  run_root "$(vdpm_bin)" "$@"
}

ensure_vitasdk() {
  if [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ] && [ -f "$VITASDK/share/vita.toolchain.cmake" ]; then
    log "VitaSDK already present at $VITASDK"
    return 0
  fi
  need_cmd git
  need_cmd bunzip2
  if [ ! -d "$SRC/vdpm/.git" ]; then
    log "cloning vdpm"
    git clone --depth 1 https://github.com/vitasdk/vdpm.git "$SRC/vdpm"
  fi
  log "bootstrapping VitaSDK channel ${CHANNEL} into $VITASDK"
  export VITASDK_CHANNEL="$CHANNEL"
  run_root env VITASDK="$VITASDK" VITASDK_CHANNEL="$CHANNEL" \
    bash "$SRC/vdpm/bootstrap-vitasdk.sh" --install-dir "$VITASDK"
  [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ] || die "bootstrap did not install arm-vita-eabi-gcc"
}

pin_package_db() {
  local conf="$VITASDK/etc/pacman.conf"
  [ -f "$conf" ] || die "missing $conf"
  local server="Server = ${PACKAGES_BASE}"
  if grep -q '^\[vita\]' "$conf"; then
    # Rewrite only the Server line that follows [vita].
    local tmp
    tmp=$(mktemp)
    awk -v srv="$server" '
      BEGIN { in_vita=0 }
      /^\[vita\]/ { in_vita=1; print; next }
      /^\[/ { in_vita=0 }
      in_vita && /^Server[ \t]*=/ { print srv; next }
      { print }
    ' "$conf" > "$tmp"
    if ! grep -Fxq "$server" "$tmp"; then
      die "failed to pin [vita] Server to $PACKAGES_SNAPSHOT"
    fi
    run_root install -m 0644 "$tmp" "$conf"
    rm -f "$tmp"
  else
    die "pacman.conf has no [vita] section"
  fi

  local sync="$VITASDK/var/lib/pacman/sync"
  run_root mkdir -p "$sync"
  local db="$sync/vita.db"
  local files="$sync/vita.files"
  if [ -f "$db" ] && [ "$(wc -c < "$db")" -eq "$VITA_DB_SIZE" ] && \
     sha256_ok "$VITA_DB_SHA256" "$db" >/dev/null 2>&1 && \
     vdpm_run pacman -- -Si zlib >/dev/null 2>&1; then
    log "pinned $PACKAGES_SNAPSHOT database already in place"
    return 0
  fi

  log "installing official $PACKAGES_SNAPSHOT vita.db (do not trust pacman -Sy alone)"
  download "$PACKAGES_BASE/vita.db" "$SRC/vita.db" "$VITA_DB_SHA256"
  download "$PACKAGES_BASE/vita.files" "$SRC/vita.files" "$VITA_FILES_SHA256"
  run_root install -m 0644 "$SRC/vita.db" "$db"
  run_root install -m 0644 "$SRC/vita.files" "$files"
  vdpm_run pacman -- -Si zlib >/dev/null || die "zlib still missing from the Vita package database"
  log "vita.db now lists zlib and the rest of the 68.1 set"
}

ensure_packages() {
  local missing=()
  local pkg
  for pkg in "${VITA_PACKAGES[@]}"; do
    if ! vdpm_run pacman -- -Q "$pkg" >/dev/null 2>&1; then
      missing+=("$pkg")
    fi
  done
  if [ ${#missing[@]} -eq 0 ]; then
    log "Vita packages already installed: ${VITA_PACKAGES[*]}"
    return 0
  fi
  log "installing Vita packages: ${missing[*]}"
  vdpm_run pacman -- -S --noconfirm --needed "${missing[@]}"
  for pkg in zlib libpng libjpeg-turbo freetype libvita2d bzip2; do
    vdpm_run pacman -- -Q "$pkg" >/dev/null || die "package $pkg failed to install"
  done
  local libdir="$VITASDK/arm-vita-eabi/lib"
  for lib in libz.a libpng.a libjpeg.a libfreetype.a libvita2d.a libbz2.a; do
    [ -f "$libdir/$lib" ] || die "missing $libdir/$lib after package install"
  done
}

rebuild_jansson_nopic() {
  # Only invoked with --jansson-nopic. Official jansson is PIC.
  local prefix="$VITASDK/arm-vita-eabi"
  local srcdir="$SRC/jansson-nopic"
  log "rebuilding jansson without PIC into $prefix"
  need_cmd git
  rm -rf "$srcdir"
  git clone --depth 1 --branch v2.15.1 https://github.com/akheron/jansson.git "$srcdir" \
    || git clone --depth 1 https://github.com/akheron/jansson.git "$srcdir"
  (
    cd "$srcdir"
    ./configure --host=arm-vita-eabi --prefix="$prefix" --enable-static --disable-shared \
      CC="$VITASDK/bin/arm-vita-eabi-gcc" \
      CFLAGS="-O2 -g -fno-pic -fno-pie -ffunction-sections -fdata-sections" \
      --disable-dependency-tracking || true
    # Fallback if the tree has no configure yet.
    if [ ! -f Makefile ]; then
      cmake -S . -B build \
        -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_C_FLAGS="-fno-pic -fno-pie" \
        -DJANSSON_BUILD_SHARED_LIBS=OFF \
        -DJANSSON_BUILD_DOCS=OFF \
        -DJANSSON_EXAMPLES=OFF \
        -DJANSSON_WITHOUT_TESTS=ON
      cmake --build build -j"$(nproc 2>/dev/null || echo 2)"
      run_root cmake --install build
    else
      make -j"$(nproc 2>/dev/null || echo 2)"
      run_root make install
    fi
  )
  log "installed non-PIC libjansson"
}

zstd_bin() {
  if command -v zstd >/dev/null 2>&1; then
    command -v zstd
  elif [ -x "$PREFIX/bin/zstd" ]; then
    printf '%s
' "$PREFIX/bin/zstd"
  else
    die "zstd not found"
  fi
}

maybe_restore_from_cache() {
  if [ -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
    return 0
  fi
  local archive="$CACHE/vitasdk.tar.zst"
  if [ ! -f "$archive" ]; then
    return 0
  fi
  log "restoring toolchain from $archive"
  # sudo PATH is sanitized; pass zstd by absolute path.
  run_root tar -I "$(zstd_bin)" -xf "$archive" -C /
}

write_archive() {
  command -v zstd >/dev/null 2>&1 || die "zstd is required for --archive"
  mkdir -p "$CACHE"
  local archive="$CACHE/vitasdk.tar.zst"
  local list="$SRC/vitasdk-tar-list.txt"
  : > "$list"
  local p
  for p in \
    "${VITASDK#/}" \
    "usr/local/cmake-${CMAKE_VER}-linux-x86_64" \
    usr/local/bin/cmake usr/local/bin/cpack usr/local/bin/ctest \
    usr/local/bin/make usr/local/bin/gmake \
    usr/local/bin/bzip2 usr/local/bin/bunzip2 usr/local/bin/bzcat \
    usr/local/bin/zstd usr/local/bin/unzstd usr/local/bin/zstdcat
  do
    if [ -e "/$p" ] || [ -L "/$p" ]; then
      printf '%s\n' "$p" >> "$list"
    fi
  done
  log "writing $archive"
  # Compress from / so paths restore in place with tar -xf -C /.
  # Prefer writing as the current user into $CACHE; fall back to root.
  if tar -I "$(zstd_bin) -T0" -cf "$archive" -C / -T "$list"; then
    chmod 0644 "$archive" || true
  else
    run_root tar -I "$(zstd_bin) -T0" -cf "$archive" -C / -T "$list"
    run_root chmod 0644 "$archive" || true
  fi
  ls -lh "$archive"
}

# ---------------------------------------------------------------------------
need_cmd curl
need_cmd tar
need_cmd sha256sum
need_cmd gcc

log "trying apt for host tools (safe to fail)"
try_apt cmake make bzip2 zstd ca-certificates curl git pkg-config || true

ensure_cmake
ensure_make
ensure_bzip2
ensure_zstd
# Restore after zstd is available so a later box can skip bootstrap + packages.
maybe_restore_from_cache
ensure_vitasdk
pin_package_db
ensure_packages
if [ "$JANSSON_NOPIC" -eq 1 ]; then
  rebuild_jansson_nopic
fi

if [ "$DO_ARCHIVE" -eq 1 ]; then
  write_archive
fi

log "toolchain ready"
printf 'export VITASDK=%s\n' "$VITASDK"
printf 'export PATH="%s/bin:%s/bin:$PATH"\n' "$VITASDK" "$PREFIX"
"$VITASDK/bin/arm-vita-eabi-gcc" --version | head -n1
cmake --version | head -n1
make --version | head -n1
vdpm_run list || true
