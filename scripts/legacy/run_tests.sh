#!/bin/bash
# Build and run the Synth1 engine unit tests (macOS 10.13 + Xcode 9.4.1).
#
#   scripts/legacy/run_tests.sh [--build-dir DIR] [--files GLOB] [--coverage] [filter ...]
#
# --files GLOB  compile only the test files matching GLOB (relative to tests/, default unit/*.cpp)
# --coverage    build an instrumented -O0 variant (in build/tests-coverage) and print a line,
#               function and region coverage report for the engine sources after the run
#
# The engine sources in src/ (everything except the GUI, the Win32 window emulation and the
# Cocoa/AU glue) are compiled with the release compiler flags into a separate build directory,
# linked with the tests in tests/unit/ and the Cocoa stubs in tests/harness/, and run. Filters
# select tests whose "suite.name" contains one of them. Nothing in src/ is modified, and the
# release build (scripts/legacy/build.sh) is not affected.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/src
T=$ROOT/tests
BUILD=$ROOT/build/tests
FILTERS=()
FILES="unit/*.cpp"
COVERAGE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD=$2; shift ;;
    --files) FILES=$2; shift ;;
    --coverage) COVERAGE=1 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) FILTERS+=("$1") ;;
  esac
  shift
done

if [ -d /Applications/Xcode941.app/Contents/Developer ]; then
  export DEVELOPER_DIR=/Applications/Xcode941.app/Contents/Developer
fi
SDK=$(xcrun --show-sdk-path)
CXX="xcrun clang++"
CC="xcrun clang"
INC="-I$SRC -I$SRC/techpop -I$SRC/audiounit -I$SRC/zlib125/zlib-1.2.5 -I$SRC/zlib125/zlib-1.2.5/contrib/minizip -I$T/harness"
COMMON="-arch x86_64 -isysroot $SDK -mmacosx-version-min=10.8 -DUSE_FILE32API -fasm-blocks -fstrict-aliasing -g -w"
OPT="-Os"
LDCOV=""
if [ $COVERAGE = 1 ]; then
  OPT="-O0 -fprofile-instr-generate -fcoverage-mapping"
  LDCOV="-fprofile-instr-generate"
  [ "$BUILD" = "$ROOT/build/tests" ] && BUILD=$ROOT/build/tests-coverage
fi
CXXFLAGS="$COMMON -stdlib=libstdc++ $OPT"
CFLAGS="$COMMON $OPT"

# engine sources under test (GUI, Win32 window emulation in winobj_au.m and the AU/Cocoa glue excluded;
# winemu_au.cpp is the Win32 file/INI/time emulation the engine needs at link time)
ENGINE_CPP="techpop/arp techpop/chorus techpop/common techpop/compressor techpop/debug techpop/decimator
  techpop/delay techpop/distortion techpop/efxs techpop/envelope techpop/eq techpop/fftwin techpop/inst
  techpop/lfo techpop/list techpop/mixdev techpop/myasiodev techpop/pan techpop/phaser techpop/phatcont
  techpop/ringmod techpop/sigdev techpop/sound techpop/ssai techpop/ssechk techpop/stepvox techpop/synmidi
  techpop/synth1 techpop/vca techpop/vcf techpop/vco techpop/vcoMain techpop/voice techpop/voicectl
  windef winemu_au"
ENGINE_C="techpop/fft4g zlib125/zlib-1.2.5/adler32 zlib125/zlib-1.2.5/compress zlib125/zlib-1.2.5/crc32
  zlib125/zlib-1.2.5/deflate zlib125/zlib-1.2.5/gzclose zlib125/zlib-1.2.5/gzlib zlib125/zlib-1.2.5/gzread
  zlib125/zlib-1.2.5/gzwrite zlib125/zlib-1.2.5/infback zlib125/zlib-1.2.5/inffast zlib125/zlib-1.2.5/inflate
  zlib125/zlib-1.2.5/inftrees zlib125/zlib-1.2.5/trees zlib125/zlib-1.2.5/uncompr zlib125/zlib-1.2.5/zutil
  zlib125/zlib-1.2.5/contrib/minizip/unzip zlib125/zlib-1.2.5/contrib/minizip/ioapi"

# Test-build-only redirects of the OS services used for settings and folders (see
# tests/harness/os_fakes.h): these engine files call the fakes instead of the real system, so a
# test run never reads or writes the user's preferences, Documents or temporary folder.
FAKES="-DCFPreferencesCopyAppValue=t1_CFPreferencesCopyAppValue -DCFPreferencesSetAppValue=t1_CFPreferencesSetAppValue -DCFPreferencesAppSynchronize=t1_CFPreferencesAppSynchronize -DFSFindFolder=t1_FSFindFolder"
FAKED_FILES=" winemu_au techpop/common techpop/sound "

mkdir -p "$BUILD/engine" "$BUILD/tests"
# rebuild everything when the compiler flags change
STAMP="$CXXFLAGS|$CFLAGS|$FAKES"
if [ "$(cat "$BUILD/.flags" 2>/dev/null)" != "$STAMP" ]; then
  rm -f "$BUILD"/engine/*.o "$BUILD"/tests/*.o
  printf '%s' "$STAMP" > "$BUILD/.flags"
fi
objs=()
build_obj() {  # $1 = compiler cmd, $2 = flags, $3 = source, $4 = object
  if [ ! -f "$4" ] || [ "$3" -nt "$4" ] || [ -n "$(find "$SRC" "$T/harness" "$T/unit" -name '*.h' -newer "$4" -print -quit)" ]; then
    $1 $2 $INC -c "$3" -o "$4"
  fi
  objs+=("$4")
}
for s in $ENGINE_CPP; do
  extra=""
  case "$FAKED_FILES" in *" $s "*) extra=$FAKES ;; esac
  build_obj "$CXX" "-x c++ $CXXFLAGS $extra" "$SRC/$s.cpp" "$BUILD/engine/$(echo $s | tr / _).o"
done
for s in $ENGINE_C; do build_obj "$CC" "-x c $CFLAGS" "$SRC/$s.c" "$BUILD/engine/$(echo $s | tr / _).o"; done
for f in "$T"/harness/*.cpp $(cd "$T" && ls $FILES 2>/dev/null | sed "s#^#$T/#"); do
  [ -f "$f" ] || continue
  build_obj "$CXX" "-x c++ $CXXFLAGS" "$f" "$BUILD/tests/$(basename "$f" .cpp).o"
done

$CXX -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=10.8 -stdlib=libstdc++ $LDCOV "${objs[@]}" \
  -framework CoreFoundation -framework CoreServices -framework ApplicationServices \
  -framework AudioToolbox -framework Carbon -o "$BUILD/synth1_tests"

cd "$T/data" 2>/dev/null || cd "$T"
if [ $COVERAGE = 0 ]; then
  exec "$BUILD/synth1_tests" "${FILTERS[@]+"${FILTERS[@]}"}"
fi

rm -f "$BUILD"/*.profraw
status=0
LLVM_PROFILE_FILE="$BUILD/tests.profraw" "$BUILD/synth1_tests" "${FILTERS[@]+"${FILTERS[@]}"}" || status=$?
xcrun llvm-profdata merge -o "$BUILD/tests.profdata" "$BUILD/tests.profraw"
# report on the engine sources under test (GUI files and third-party code excluded)
REPORT_SRC=$(cd "$SRC" && ls techpop/*.cpp | grep -vE 'guiparts|synwin|synthdlg|ccwindow|stepvoxwin|drawgui|cdib' | sed "s#^#$SRC/#")
echo
echo "== coverage of the engine sources (src/techpop, excluding the GUI) =="
xcrun llvm-cov report "$BUILD/synth1_tests" -instr-profile="$BUILD/tests.profdata" $REPORT_SRC | sed "s#$SRC/##"
echo "(per-line detail: xcrun llvm-cov show $BUILD/synth1_tests -instr-profile=$BUILD/tests.profdata <source>)"
exit $status
