#!/bin/bash
# Rebuild Synth1.component (v1.13 beta11, macOS AU) byte-identically from src/.
#
#   scripts/legacy/build.sh            build, then verify against scripts/legacy/release/expected.sha256
#   scripts/legacy/build.sh --verify   only verify an existing build/Synth1.component
#   scripts/legacy/build.sh --clean    remove the staging copy and DerivedData folder afterwards
#   scripts/legacy/build.sh --force    skip the macOS version check and ownership safety checks
#
# Requirements
#   - macOS 10.13.6
#   - Xcode 9.4.1 (build 9F2000) installed as /Applications/Xcode941.app
#     (the path is recorded in the binary's debug map, so it must be exactly this)
#   - permission to create /Users/Ichiro (sudo is used once if it doesn't exist)
#
# Why the build does what it does
#   The original was built by user "Ichiro" from /Users/Ichiro/dev/synth1_mac113 with
#   DerivedData in /Users/Ichiro/Library/Developer/Xcode/DerivedData. Both paths are
#   recorded in the AU's debug map (and in __FILE__ strings), so the sources are staged
#   and built there. After xcodebuild:
#     1. the AU's object files get the modification times recorded in its debug map,
#        and the AU is relinked with Xcode's own link command;
#     2. ld64 orders indirectly loaded dylibs nondeterministically (it varies between
#        links), and that order decides the __stub_helper layout, so both executables
#        are relinked until the original order comes up;
#     3. the GUI bundle executable is relinked from copies of its objects whose paths
#        are 316 bytes shorter in total, which reproduces its original LC_UUID
#        (a build-process MATCH-HACK; see docs/HOW_IT_WORKS.md), then stripped again
#        exactly as Xcode's copy step did.
#   The result is checked file by file against the SHA-256 hashes of the original.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
RELEASE=$ROOT/scripts/legacy/release
XCODE=/Applications/Xcode941.app
HOMEDIR=/Users/Ichiro
SRCDIR=$HOMEDIR/dev/synth1_mac113
DD=$HOMEDIR/Library/Developer/Xcode/DerivedData
DDPROJ=$DD/Synth1_xc941-fdydavfctqwqidgmlwllxotqsiel
INTER=$DDPROJ/Build/Intermediates.noindex/Synth1_xc941.build/Release
AUOBJ=$INTER/Synth1AudioUnit.build/Objects-normal/x86_64
VFOBJ=$INTER/Synth1CocoaViewFactory.build/Objects-normal/x86_64
VFSHORT=$INTER/Synth1ViewFactory.build/Objects-normal/x86_64
PRODUCT=$DDPROJ/Build/Products/Release/Synth1.component
MARKER=$HOMEDIR/.synth1-build-owner
OUT=$ROOT/build
WORK=$OUT/work
WANT_DYLIBS="CoreFoundation Foundation CoreGraphics CoreText libobjc.A"
VF_UUID=4AFD8BF6-627F-32CA-98F6-62E758161E75
MAX_LINKS=200

# Print a progress message to stdout.
#   $* = message
say()  { printf '==> %s\n' "$*"; }

# Print an error message to stderr and exit with status 1.
#   $* = message
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# Check build/Synth1.component against scripts/legacy/release/expected.sha256, comparing both
# the hashes and the file count (.DS_Store files aren't counted). Prints any mismatches,
# leaves the full shasum output in build/verify.log, and dies if the build differs.
verify() {
  say "verifying build/Synth1.component against scripts/legacy/release/expected.sha256"
  [ -d "$OUT/Synth1.component" ] || die "no build/Synth1.component to verify"
  local bad=0 n
  (cd "$OUT" && shasum -a 256 -c "$RELEASE/expected.sha256") > "$OUT/verify.log" 2>&1 || bad=1
  grep -v ': OK$' "$OUT/verify.log" || true
  n=$(cd "$OUT" && find Synth1.component -type f ! -name .DS_Store | wc -l | tr -d ' ')
  local want; want=$(wc -l < "$RELEASE/expected.sha256" | tr -d ' ')
  [ "$n" = "$want" ] || { echo "file count: $n (expected $want)"; bad=1; }
  if [ $bad = 0 ]; then
    say "OK: all $want files are byte-identical to the original release"
  else
    die "the build differs from the original (see build/verify.log)"
  fi
}

# Print a command Xcode logged in build/xcodebuild.log: the indented lines that follow
# the first log entry matching a regex, up to the next blank line.
#   $1 = regex for the entry's first line (e.g. its Ld line)
link_cmd() {
  awk -v re="$1" '$0 ~ re {f=1; next} f && /^$/ {exit} f && /^    / {print}' "$OUT/xcodebuild.log"
}

# Rerun a link script until ld64 loads the dylibs in the order given by WANT_DYLIBS and,
# if a UUID is given, the output file has that LC_UUID. Gives up and dies after
# MAX_LINKS attempts.
#   $1 = link script
#   $2 = output file (only needed with $3)
#   $3 = wanted LC_UUID (optional)
relink_until() {
  local i got uuid=""
  for i in $(seq 1 $MAX_LINKS); do
    # Link once with -t added to the end of the command, which makes ld64 list each file it
    # loads. Reduce that list to the five dylib names, in order, separated by spaces, so
    # it can be compared with WANT_DYLIBS.
    got=$(sed '$s/$/ -Xlinker -t/' "$1" | bash 2>/dev/null \
      | grep -oE '/(CoreFoundation|Foundation|CoreGraphics|CoreText|libobjc\.A)\.tbd$' \
      | sed 's#^/##; s#\.tbd$##' | tr '\n' ' ' | sed 's/ $//')
    if [ -n "${3:-}" ]; then uuid=$(xcrun dwarfdump --uuid "$2" | awk '{print $2}'); fi
    # With no wanted UUID, both sides of the UUID check are empty, so only the order counts
    if [ "$got" = "$WANT_DYLIBS" ] && [ "$uuid" = "${3:-}" ]; then
      echo "    $(basename "$1"): matched after $i link(s)"; return 0
    fi
  done
  die "$(basename "$1"): no match after $MAX_LINKS links (dylib order: $got${3:+, UUID $uuid})"
}

# Handle command line arguments
DO_BUILD=1; CLEAN=0; FORCE=0
for a in "$@"; do
  case "$a" in
    --verify) DO_BUILD=0 ;;
    --clean) CLEAN=1 ;;
    --force) FORCE=1 ;;
    -h|--help) sed -n '2,29p' "$0"; exit 0 ;;
    *) echo "unknown option: $a (see --help)" >&2; exit 2 ;;
  esac
done

# With --verify, just check an existing build and skip everything below
if [ $DO_BUILD = 0 ]; then verify; exit 0; fi

# Make sure this machine has the exact macOS and Xcode versions and the tools we need
say "checking requirements"
[ "$(uname)" = Darwin ] || die "this must run on macOS"
osv=$(sw_vers -productVersion)
if [ "$osv" != 10.13.6 ]; then
  [ $FORCE = 1 ] && echo "warning: macOS $osv, not 10.13.6 (continuing because of --force)" \
                 || die "macOS 10.13.6 is required (this is $osv); use --force to try anyway"
fi
[ -d "$XCODE/Contents/Developer" ] || die "Xcode 9.4.1 must be installed as $XCODE"
export DEVELOPER_DIR=$XCODE/Contents/Developer
xcv=$(xcodebuild -version 2>/dev/null | tr '\n' ' ') || die "xcodebuild failed (has the Xcode license been accepted? try: sudo xcodebuild -license accept)"
case "$xcv" in
  *"Xcode 9.4.1"*"Build version 9F2000"*) ;;
  *) die "$XCODE is not Xcode 9.4.1 (9F2000): $xcv" ;;
esac
for t in rsync perl shasum awk sed; do command -v $t >/dev/null || die "missing tool: $t"; done

# Set up the build location under $HOMEDIR. A marker file records that this script
# created it, so we don't write into a directory that belongs to someone else.
if [ ! -d "$HOMEDIR" ]; then
  say "creating $HOMEDIR (needs sudo once; it will be owned by $(id -un))"
  sudo mkdir -p "$HOMEDIR"
  sudo chown "$(id -u):$(id -g)" "$HOMEDIR"
  echo "created by synth1 build.sh for $(id -un)" > "$MARKER"
elif [ ! -f "$MARKER" ] && [ $FORCE = 0 ]; then
  die "$HOMEDIR exists but was not created by this script; refusing to write into it (use --force)"
fi
[ -w "$HOMEDIR" ] || die "$HOMEDIR is not writable by $(id -un)"
mkdir -p "$SRCDIR" "$DD" "$WORK"

# Copy the sources to the staging location, then build them from scratch with Xcode
say "staging src/ at $SRCDIR"
rsync -a --delete --exclude 'xcuserdata/' --exclude '.DS_Store' "$ROOT/src/" "$SRCDIR/"

say "xcodebuild (Release); log: build/xcodebuild.log"
rm -rf "$DDPROJ"
( cd "$SRCDIR" && xcodebuild -project Synth1_xc941.xcodeproj -scheme Synth1AudioUnit \
    -configuration Release -IDECustomDerivedDataLocation="$DD" \
    MAC_OS_X_PRODUCT_BUILD_VERSION=17G14042 build ) > "$OUT/xcodebuild.log" 2>&1 \
  || { grep -E 'error:|\*\* BUILD' "$OUT/xcodebuild.log" | head -40; die "xcodebuild failed"; }
[ -d "$AUOBJ" ] && [ -d "$VFOBJ" ] && [ -d "$PRODUCT" ] || die "unexpected DerivedData layout (is this Xcode 9.4.1?)"

# Extract Xcode's own link and strip commands from the log so they can be rerun below
link_cmd '^Ld .*Synth1\.component/Contents/MacOS/Synth1 normal x86_64' > "$WORK/relink_au.sh"
link_cmd '^Ld .*Synth1CocoaViewFactory normal x86_64' > "$WORK/relink_vf.sh"
# The strip command is part of Xcode's step that copies the bundle (PBXCp), so find that
# log entry and keep only its strip line. link_cmd would print the whole step.
awk '/^PBXCp .*Synth1CocoaViewFactory\.bundle /{f=1;next} f&&/^$/{exit} f&&/\/strip -S /{print}' \
  "$OUT/xcodebuild.log" > "$WORK/restrip_vf.sh"
for f in relink_au.sh relink_vf.sh restrip_vf.sh; do
  [ -s "$WORK/$f" ] || die "could not find the command for $f in xcodebuild.log"
done

# Give the AU's object files the modification times recorded in the original's debug map
say "restoring the original object modification times"
while read -r obj t; do
  case "$obj" in ''|'#'*) continue ;; esac
  [ -f "$AUOBJ/$obj" ] || die "missing object $obj"
  perl -e 'utime $ARGV[0], $ARGV[0], $ARGV[1] or die "$ARGV[1]: $!\n"' "$t" "$AUOBJ/$obj"
done < "$RELEASE/oso_mtimes.txt"

# Relink the GUI bundle executable from copies of its objects at shorter paths, which
# reproduces the original LC_UUID (see docs/HOW_IT_WORKS.md), then strip it again
say "relinking the GUI bundle executable (original dylib order and LC_UUID)"
VFOUT=$(grep -oE ' -o [^ ]+$' "$WORK/relink_vf.sh" | cut -c5-)
# Copy the objects and write a new file list for them. The original's stab strings were
# 316 bytes shorter in total than a normal build's: the directory name is 5 characters
# shorter, which gives -320 across the 64 objects, and renaming synth1.o adds back 4.
rm -rf "$VFSHORT"; mkdir -p "$VFSHORT"; : > "$VFSHORT/Synth1CocoaViewFactory.LinkFileList"
while read -r p; do
  b=$(basename "$p"); [ "$b" = synth1.o ] && b=synth1-pad.o
  cp -p "$p" "$VFSHORT/$b"
  echo "$VFSHORT/$b" >> "$VFSHORT/Synth1CocoaViewFactory.LinkFileList"
done < "$VFOBJ/Synth1CocoaViewFactory.LinkFileList"
sed "s#-filelist [^ ]*#-filelist $VFSHORT/Synth1CocoaViewFactory.LinkFileList#" \
  "$WORK/relink_vf.sh" > "$WORK/relink_vf_short.sh"
relink_until "$WORK/relink_vf_short.sh" "$VFOUT" "$VF_UUID"
bash "$WORK/restrip_vf.sh"

# Relink the AU executable so it picks up the restored mtimes and the original dylib order
say "relinking the AU executable (original object mtimes and dylib order)"
relink_until "$WORK/relink_au.sh"

# Copy the finished component into build/ and check it against the original's hashes
rm -rf "$OUT/Synth1.component"
ditto "$PRODUCT" "$OUT/Synth1.component"
say "built build/Synth1.component"
verify

# With --clean, remove the build files left under $HOMEDIR
if [ $CLEAN = 1 ]; then
  say "removing the staging copy and DerivedData folder"
  rm -rf "$SRCDIR" "$DDPROJ"
fi
