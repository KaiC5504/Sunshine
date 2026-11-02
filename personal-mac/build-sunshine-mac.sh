#!/usr/bin/env bash
# Fallback: build the personal Sunshine fork on the Mac and install it to /Applications.
# Normally you install the signed DMG from https://github.com/KaiC5504/Sunshine/releases instead.
#
# Usage:  bash build-sunshine-mac.sh
# Env:    SRC_DIR=<checkout dir>  (default ~/src/sunshine-personal)
#         BUILD_TESTS=ON          also build and run the clipboard and capture unit tests
set -euo pipefail

REPO=https://github.com/KaiC5504/Sunshine.git
BRANCH=personal
SRC_DIR="${SRC_DIR:-$HOME/src/sunshine-personal}"
BUILD_TESTS="${BUILD_TESTS:-OFF}"

die() { echo "error: $*" >&2; exit 1; }

[[ "$(uname)" == Darwin ]] || die "run this on the Mac"
macos_major="$(sw_vers -productVersion | cut -d. -f1)"
(( macos_major >= 15 )) || die "macOS 15 or newer is required (found $(sw_vers -productVersion))"
xcode-select -p >/dev/null 2>&1 || die "install the Xcode command line tools first: xcode-select --install"
command -v brew >/dev/null || die "install Homebrew first: https://brew.sh"
if pgrep -ix sunshine >/dev/null; then
  die "Sunshine is running. Quit it from the menu bar (or 'brew services stop sunshine') and re-run."
fi

brew install boost cmake node pkgconf icu4c@78 miniupnpc openssl@4 opus qtbase qtsvg

if [[ ! -d "$SRC_DIR/.git" ]]; then
  mkdir -p "$(dirname "$SRC_DIR")"
  git clone --branch "$BRANCH" "$REPO" "$SRC_DIR"
fi
cd "$SRC_DIR"
git fetch origin "$BRANCH"
git checkout -f -B "$BRANCH" "origin/$BRANCH"
git submodule sync --recursive
git submodule update --init --recursive --depth 1 --jobs 8 || git submodule update --init --recursive --jobs 8

icu4c_root="$(brew --prefix icu4c@78)"
export CPPFLAGS="${CPPFLAGS:+$CPPFLAGS }-I$icu4c_root/include"
export LDFLAGS="${LDFLAGS:+$LDFLAGS }-L$icu4c_root/lib"
export LIBRARY_PATH="$icu4c_root/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
export MACOSX_DEPLOYMENT_TARGET=15.0

cmake -B build -S . \
  -DBUILD_DOCS=OFF \
  -DBUILD_TESTS="$BUILD_TESTS" \
  -DCMAKE_BUILD_TYPE=Release \
  -DICU_ROOT="$icu4c_root" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@4)" \
  -DOpus_ROOT_DIR="$(brew --prefix opus)" \
  -DQt6_DIR="$(brew --prefix qtbase)/lib/cmake/Qt6" \
  -DQt6Svg_DIR="$(brew --prefix qtsvg)/lib/cmake/Qt6Svg"
cmake --build build -j "$(sysctl -n hw.ncpu)"

if [[ "$BUILD_TESTS" == ON ]]; then
  (cd build/tests && ./test_sunshine --gtest_filter='SCCaptureLoop.*:ClipboardSync*:ClipboardFiles*')
fi

cpack -G DragNDrop --config build/CPackConfig.cmake
dmg="$SRC_DIR/build/cpack_artifacts/Sunshine.dmg"
[[ -f "$dmg" ]] || die "cpack did not produce $dmg"

mnt="$(mktemp -d)"
hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$dmg" >/dev/null
trap 'hdiutil detach "$mnt" >/dev/null 2>&1 || true' EXIT

if [[ -d /Applications/Sunshine.app ]]; then
  backup="$SRC_DIR/Sunshine.app.backup-$(date +%Y%m%d-%H%M%S)"
  mv /Applications/Sunshine.app "$backup"
  echo "Previous /Applications/Sunshine.app moved to $backup"
fi
ditto "$mnt/Sunshine.app" /Applications/Sunshine.app

# The packaging step strips signatures; Apple Silicon refuses to run unsigned code, so sign ad hoc.
xattr -cr /Applications/Sunshine.app
codesign --force --deep --sign - /Applications/Sunshine.app

cat <<'DONE'

Installed /Applications/Sunshine.app (ad hoc signed: macOS asks for permissions again after every rebuild).

Next:
  1. If you had the Homebrew package: brew services stop sunshine; brew uninstall sunshine
  2. System Settings > Privacy & Security: remove old Sunshine entries from
     Screen & System Audio Recording and Accessibility.
  3. Open /Applications/Sunshine.app and grant both permissions again, then relaunch it.
DONE
