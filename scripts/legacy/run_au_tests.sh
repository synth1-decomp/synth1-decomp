#!/bin/bash
# Build and run the black-box Audio Unit tests against a built Synth1.component (macOS).
#
#   scripts/legacy/run_au_tests.sh [path/to/Synth1.component] [filter ...]
#   scripts/legacy/run_au_tests.sh --gen path/to/Synth1.component
#
# Default component: build/Synth1.component in the repo root (as produced by scripts/legacy/build.sh).
# The test host au_tests.c loads the bundle, registers its factory in-process and drives the AU
# through the public Audio Unit API; nothing is linked from src/. Filters select tests whose
# name contains one of them.
#
# --gen regenerates tests/au/reference.txt and tests/au/default_state.plist from the given
# component. Only do this with the ORIGINAL release binary: the stored values are the reference
# the rebuild must reproduce.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/tests/au     # the test host source and its reference files
BUILD=$ROOT/build/au-tests
GEN=0
if [ "${1:-}" = --gen ]; then GEN=1; shift; fi
case "${1:-}" in
  -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
  *.component|*.component/) COMP=${1%/}; shift ;;
  *) COMP=$ROOT/build/Synth1.component ;;
esac
[ -d "$COMP" ] || { echo "no component at $COMP" >&2; exit 2; }
COMP=$(cd "$COMP" && pwd)

if [ -d /Applications/Xcode941.app/Contents/Developer ]; then
  export DEVELOPER_DIR=/Applications/Xcode941.app/Contents/Developer
fi
mkdir -p "$BUILD"
TARGET="-arch x86_64 -isysroot $(xcrun --show-sdk-path) -mmacosx-version-min=10.8"
xcrun clang $TARGET -std=gnu99 -O1 -g -Wall -Wno-four-char-constants \
  "$HERE/au_tests.c" -framework AudioToolbox -framework AudioUnit -framework CoreFoundation -o "$BUILD/au_tests"
xcrun clang $TARGET -dynamiclib -O1 -g -Wall -Wno-deprecated-declarations \
  "$HERE/au_isolation.c" -framework CoreFoundation -framework CoreServices -o "$BUILD/libau_isolation.dylib"

# Run the host with au_isolation.c inserted: the AU sees default settings and creates its
# Synth1Patches folders in a temporary folder, so the user's settings and ~/Documents are untouched
TEST_HOME=$(mktemp -d "${TMPDIR:-/tmp}/synth1-au-tests.XXXXXX")
trap 'rm -rf "$TEST_HOME"' EXIT
export SYNTH1_TEST_HOME=$TEST_HOME
export DYLD_INSERT_LIBRARIES=$BUILD/libau_isolation.dylib

status=0
if [ $GEN = 1 ]; then
  "$BUILD/au_tests" --gen "$COMP" "$HERE/reference.txt" "$HERE/default_state.plist" || status=$?
else
  "$BUILD/au_tests" "$COMP" "$HERE/reference.txt" "$HERE/default_state.plist" "$@" || status=$?
fi
exit $status
