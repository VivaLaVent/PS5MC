#!/usr/bin/env bash
# Phase 0: prepare a WSL2 Ubuntu 24.04 host for building Kodi for PS5.
#
#   1. ps5-payload-sdk (prebuilt release) -> /opt/ps5-payload-sdk (+ libc++)
#   2. pacbrew-repo: ~100 ported libraries built into the SDK sysroot
#   3. ps5-opengl: OpenGL 4.6 / EGL SDK -> /opt/ps5-opengl-gl46
#
# Everything is idempotent; re-run after a failure. Budget 1-3 hours of build
# time for pacbrew and ps5-opengl (Mesa) on an 8-core machine.
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export PS5_OPENGL_PREFIX="${PS5_OPENGL_PREFIX:-/opt/ps5-opengl-gl46}"
WORK="${WORK:-$HOME/ps5-work}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$WORK"

echo "==> host packages"
# Keep sudo's credential cache alive for the whole run. ci-libs.sh installs each
# package with `sudo pacman -U`; llvm and mesa build for longer than sudo's
# 15-minute timeout, so without this the run would stop at a password prompt
# after an hour of compiling. Same tty as the build, so the refresh counts.
sudo -v
( while kill -0 "$$" 2>/dev/null; do sudo -n true 2>/dev/null; sleep 60; done ) &
SUDO_KEEPALIVE=$!
trap 'kill "$SUDO_KEEPALIVE" 2>/dev/null' EXIT

sudo apt-get update
# SDK + pacbrew prerequisites (from their READMEs) plus what Kodi's own
# configure step and native tools need on the host.
sudo apt-get install -y \
  bash clang clang-18 lld lld-18 llvm llvm-18 wget curl git unzip socat \
  cmake ninja-build meson pkg-config pkgconf python3 python3-pyelftools \
  build-essential autoconf automake libtool libtool-bin yasm nasm bison flex gperf \
  libarchive-tools autopoint po4a doxygen makepkg pacman-package-manager \
  python3-mako python3-glad python3-yaml python3-ply python3-setuptools gettext tcl \
  flatbuffers-compiler default-jre-headless swig \
  liblzo2-dev libpng-dev libgif-dev libjpeg-dev zlib1g-dev

echo "==> ps5-payload-sdk"
# pacbrew installs the SDK itself as its first package (ps5-payload-sdk, via
# pacman) and libc++ as its third (libcxx). Do NOT unpack the GitHub release
# ZIP into /opt first: pacman refuses to overwrite files it does not own
# ("conflicting files"). If a manual copy is there, remove it.
if [ -d "$PS5_PAYLOAD_SDK" ] && ! pacman -Qq ps5-payload-sdk >/dev/null 2>&1; then
  echo "    removing manually unpacked SDK at $PS5_PAYLOAD_SDK (pacbrew reinstalls it as a package)"
  sudo rm -rf "$PS5_PAYLOAD_SDK"
fi

echo "==> pacbrew-repo (ported libraries)"
if [ ! -d "$WORK/pacbrew-repo" ]; then
  git clone --depth 1 https://github.com/ps5-payload-dev/pacbrew-repo.git "$WORK/pacbrew-repo"
fi
if [ ! -f "$PS5_PAYLOAD_SDK/target/user/homebrew/lib/pkgconfig/libavcodec.pc" ]; then
  # Builds and installs every package (pacman -U) into the sysroot, SDK first,
  # in the order of upstream's ci-libs.sh - but through our own loop, because
  # upstream's list has glu/glew before the mesa they depend on, and because
  # Kodi does not use pacbrew's GL stack at all (ps5-opengl builds its own
  # compiler and Mesa). Skipping llvm+mesa alone saves 1-2 hours.
  # Set PACBREW_SKIP="" to build everything (then build glu/glew after mesa).
  SKIP="${PACBREW_SKIP-glu glew llvm mesa love}" \
    bash "$(dirname "${BASH_SOURCE[0]}")/01-pacbrew-resume.sh" sdk
fi

echo "==> ps5-opengl -> $PS5_OPENGL_PREFIX"
# Extra host tools for ps5-opengl (docs/building.md): shader-compiler checks
# need glslangValidator + SPIR-V Tools; Mesa 26 needs a newer Meson than the
# 1.3 Ubuntu 24.04 ships (ps5-opengl records Meson 1.10), so take it from pip.
sudo apt-get install -y glslang-tools spirv-tools python3-packaging python3-pip python3-venv
# Kodi's Python bindings generator (SWIG + Groovy, which needs a Java runtime)
sudo apt-get install -y swig default-jre-headless
if ! dpkg --compare-versions "$(meson --version 2>/dev/null || echo 0)" ge 1.10; then
  pip3 install --user --break-system-packages 'meson>=1.10'
fi
export PATH="$HOME/.local/bin:$PATH"

# The native-app boilerplate supplies the ELF -> folder-title tooling that the
# imgui-demo uses (and that Kodi's deploy step reuses). ps5-opengl's pinned
# boilerplate commit is no longer in upstream history; the next best is
# 81d4235 (the RELRO fix, same build layout as before). Newer main restructured
# the build (Ninja), which the tooling here has not been validated against.
if [ ! -d "$WORK/ps5-native-app-boilerplate" ]; then
  git clone https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git "$WORK/ps5-native-app-boilerplate"
  ( cd "$WORK/ps5-native-app-boilerplate" \
      && { git checkout -q 4e1d1277dd0531a9a9df8c780e446b9cc26534dd 2>/dev/null \
           || git checkout -q 81d4235346afc7490e98866af4020e94f255b204 2>/dev/null \
           || echo "    known boilerplate commits not found upstream; staying on main"; } )
fi
# Older boilerplate revisions wrote the RELRO LOAD segment at the wrong file
# offset and the console refused the title (CE-107750-0). Upstream fixed it in
# 81d4235 (relro_origin); for revisions before that, apply ProsperoLight's fix.
WRITER="$WORK/ps5-native-app-boilerplate/tooling/native/sce_module_writer.cpp"
if grep -q "relro_origin\|RELRO mapping must begin" "$WRITER"; then
  echo "    boilerplate RELRO fix present"
else
  ( cd "$WORK/ps5-native-app-boilerplate" \
      && patch -p1 < "$(dirname "${BASH_SOURCE[0]}")/../patches/ps5-native-app-boilerplate-relro.patch" )
fi
if [ ! -d "$WORK/ps5-native-app-boilerplate/.deps/native" ]; then
  # Downloads the SDK v0.42 release zip + zlib into the boilerplate's own .deps
  # (used only for packaging apps; the GL libraries below use our sysroot).
  ( cd "$WORK/ps5-native-app-boilerplate" && bash tools/setup-native-dependencies.sh )
fi

if [ ! -d "$WORK/ps5-opengl" ]; then
  # pinned: patches/ps5-opengl/kodi-additions.py is written against this revision
  PS5_OPENGL_COMMIT="$(tr -d '[:space:]' < "$(dirname "${BASH_SOURCE[0]}")/../patches/ps5-opengl/PS5-OPENGL-COMMIT")"
  git clone --no-checkout https://github.com/blackbearreloaded/ps5-opengl.git "$WORK/ps5-opengl"
  git -C "$WORK/ps5-opengl" checkout -q "$PS5_OPENGL_COMMIT"
fi
if [ ! -f "$PS5_OPENGL_PREFIX/lib/cmake/PS5OpenGL/PS5OpenGLConfig.cmake" ]; then
  # "Library-only workflow": PS5_PAYLOAD_SDK is exported above, so the compiler
  # and Mesa are built against OUR sysroot, i.e. the one Kodi links against.
  # If Mesa refuses to build against it, retry with the SDK the project was
  # validated with:
  #   PS5_PAYLOAD_SDK=$WORK/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk \
  #     make -C $WORK/ps5-opengl sdk-gl46
  ( cd "$WORK/ps5-opengl" && make source-fetch && make sdk-gl46 )
  sudo mkdir -p "$PS5_OPENGL_PREFIX"
  sudo cp -a "$WORK/ps5-opengl/build/sdk/ps5-opengl-gl46/." "$PS5_OPENGL_PREFIX/"
fi

cat <<MSG

Done. Add to ~/.bashrc:
  export PS5_PAYLOAD_SDK=$PS5_PAYLOAD_SDK
  export PS5_OPENGL_PREFIX=$PS5_OPENGL_PREFIX
  export PS5_HOST=<your console IP>

Sanity check before touching Kodi - confirm GL works on YOUR firmware:
  cd $WORK/ps5-opengl && make imgui-demo
  then upload build/native-app/PPSA99005/dist/PPSA99005/ to /data/homebrew/PPSA99005
  (FTP, port 2121) and launch the title from the home screen.
MSG
