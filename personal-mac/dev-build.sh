#!/usr/bin/env bash
# Fast development loop on the Mac: incremental build, optional tests, signed install, restart.
# The checkout accepts pushes from the development PC (git push mac personal), so nothing has to go
# through GitHub first. Releases still come from GitHub Actions.
#
# Usage:  dev-build.sh [--test GTEST_FILTER] [--no-install]
# Setup:  Homebrew, then setup-signing.sh once; see PERSONAL.md "Building on the Mac".
set -euo pipefail

SRC_DIR="${SRC_DIR:-$HOME/src/sunshine-personal}"
BUILD_DIR="$SRC_DIR/build-dev"
STAGE_DIR="$SRC_DIR/build-dev-stage"
KEYCHAIN="$HOME/Library/Keychains/sunshine-signing.keychain-db"
PASSWORD_FILE="$HOME/.config/sunshine-build/keychain-password"
APP=/Applications/Sunshine.app

test_filter=""
install=1
while (($#)); do
  case "$1" in
    --test) test_filter="${2:?--test needs a gtest filter}"; shift 2 ;;
    --no-install) install=0; shift ;;
    *) echo "usage: $0 [--test GTEST_FILTER] [--no-install]" >&2; exit 2 ;;
  esac
done

die() { echo "error: $*" >&2; exit 1; }
step() { printf '\n== %s (%ss)\n' "$1" "$SECONDS"; }

[[ -x /opt/homebrew/bin/brew ]] || die "install Homebrew first"
eval "$(/opt/homebrew/bin/brew shellenv)"
deps=(boost cmake ninja node pkgconf icu4c@78 miniupnpc openssl@4 opus qtbase qtsvg)
missing=()
for dep in "${deps[@]}"; do
  [[ -d "$(brew --prefix)/opt/$dep" ]] || missing+=("$dep")
done
if ((${#missing[@]})); then
  step "Installing ${missing[*]}"
  brew install "${missing[@]}"
fi

cd "$SRC_DIR"
step "Submodules"
git submodule update --init --recursive --jobs 8

identity=""
if [[ -f "$PASSWORD_FILE" && -f "$KEYCHAIN" ]]; then
  security unlock-keychain -p "$(cat "$PASSWORD_FILE")" "$KEYCHAIN"
  # The SHA-1 hash, so the certificate holder's name stays out of the build output.
  identity="$(security find-identity -v -p codesigning "$KEYCHAIN" | awk '/"Developer ID Application/ { print $2; exit }')"
fi
((install == 0)) || [[ -n "$identity" ]] ||
  die "no signing identity; run setup-signing.sh first (an unsigned build would lose its macOS permissions)"

icu4c_root="$(brew --prefix icu4c@78)"
export CPPFLAGS="-I$icu4c_root/include"
export LDFLAGS="-L$icu4c_root/lib"
export LIBRARY_PATH="$icu4c_root/lib"
# Built for this Mac only, so target its own macOS like the Homebrew libraries it links.
export MACOSX_DEPLOYMENT_TARGET="$(sw_vers -productVersion | cut -d. -f1).0"

# A release-style version, so the web UI doesn't offer releases from before this build as updates.
# Day granularity: the version is a compile definition, and changing it rebuilds everything.
export BRANCH=personal
export BUILD_VERSION="$(date -u +%Y.%-m%d).0"
# Same -Werror as the release workflow, so a release never fails on a warning seen here first.
settings=(-DBUILD_WERROR=ON)
[[ -z "$identity" ]] || settings+=(-DAPPLE_CODESIGN_IDENTITY="$identity")
stamp="$BUILD_DIR/dev-build-settings"

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
  step "Configuring"
  cmake -G Ninja -B "$BUILD_DIR" -S . \
    -DBUILD_DOCS=OFF \
    -DBUILD_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DICU_ROOT="$icu4c_root" \
    -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@4)" \
    -DOpus_ROOT_DIR="$(brew --prefix opus)" \
    -DQt6_DIR="$(brew --prefix qtbase)/lib/cmake/Qt6" \
    -DQt6Svg_DIR="$(brew --prefix qtsvg)/lib/cmake/Qt6Svg" \
    "${settings[@]}"
elif [[ "$(cat "$stamp" 2>/dev/null)" != "$BUILD_VERSION ${settings[*]}" ]]; then
  # CMake doesn't notice environment changes by itself, and reconfiguring every time costs seconds.
  step "Reconfiguring"
  cmake -B "$BUILD_DIR" "${settings[@]}" >/dev/null
fi
echo "$BUILD_VERSION ${settings[*]}" > "$stamp"

step "Building"
cmake --build "$BUILD_DIR" --target sunshine web-ui

if [[ -n "$test_filter" ]]; then
  step "Testing $test_filter"
  cmake --build "$BUILD_DIR" --target test_sunshine
  (cd "$BUILD_DIR/tests" && ./test_sunshine --gtest_filter="$test_filter")
fi

((install)) || { step "Built"; exit 0; }

step "Packaging and signing"
rm -rf "$STAGE_DIR"
SHOULD_SIGN=true cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR" >/dev/null
codesign --verify --strict "$STAGE_DIR/Sunshine.app"

step "Installing"
if pgrep -x Sunshine >/dev/null; then
  pkill -TERM -x Sunshine
  for _ in {1..20}; do pgrep -x Sunshine >/dev/null || break; sleep 0.5; done
  pgrep -x Sunshine >/dev/null && die "Sunshine did not quit"
fi
rm -rf "$APP"
ditto "$STAGE_DIR/Sunshine.app" "$APP"
open -a "$APP"
sleep 3
pgrep -x Sunshine >/dev/null || die "Sunshine did not start; see ~/.config/sunshine/sunshine.log"
step "Installed $(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString), running"
