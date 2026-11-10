# How the reproducible build works

A plain Xcode build of `src/` gives a working plugin. 
Making it byte-identical to the shipped binary needs the steps below. `scripts/legacy/build.sh` does all of them.

## 1. Paths

The original was built from `/Users/Ichiro/dev/synth1_mac113`, with DerivedData in
`/Users/Ichiro/Library/Developer/Xcode/DerivedData` and Xcode at `/Applications/Xcode941.app`.

The AU executable keeps its stabs debug map:
- the source and header paths (N_SO/N_SOL), including SDK headers inside Xcode;
- every object file's path and modification time (N_OSO).

`__FILE__` strings also embed the source path. So the sources are staged at the original
path and Xcode must be at the original path. The GUI bundle executable is stripped, but the path
lengths still matter for it (section 4).

Two more build inputs are fixed:
- The DerivedData folder name `Synth1_xc941-fdydavfctqwqidgmlwllxotqsiel` is derived from the
  project path, so it comes out the same automatically.
- `MAC_OS_X_PRODUCT_BUILD_VERSION=17G14042` reproduces the build machine's OS build, which
  Xcode writes into Info.plist as `BuildMachineOSBuild`.

## 2. Object timestamps

Each N_OSO entry records its object file's modification time. `scripts/legacy/release/oso_mtimes.txt` lists the
original values, read from the original debug map. After xcodebuild, the AU's objects get these
times and the AU is relinked with Xcode's own link command, taken from the build log.

## 3. Nondeterministic dylib order

The AU links CoreFoundation, Foundation, CoreGraphics, CoreText and libobjc only indirectly,
through the umbrella frameworks. ld64 (351.8) assigns indirect dylibs their ordinals in discovery
order. That order comes from walking a pointer-keyed set filled by parallel parser threads, so
it changes from one link to the next. It decides the dylib grouping of `__stub_helper`, and
through that the contents of `__la_symbol_ptr`.

`scripts/legacy/build.sh` relinks with `-Xlinker -t` until the order matches the original's: CoreFoundation,
Foundation, CoreGraphics, CoreText, libobjc. This usually takes 1–10 links.

The original also compiled its Objective-C files without modules. Modules add autolink load
options, which would fix a different dylib order.

## 4. The GUI bundle's LC_UUID (build-process MATCH-HACK)

ld64 computes LC_UUID as an MD5 over the output file's leaf name and the unstripped link output.
The stab nlists, the stab strings and the `__LINKEDIT` and LC_SYMTAB load commands are left out
of the hash. The stabs still change it in two ways:
- **The stab count** shifts symbol indices.
- **The total stab-string length** shifts later string offsets.

Xcode strips the GUI bundle's stabs (`strip -S`) when it copies the bundle into the component,
but the UUID was computed before that.

A search over the possible lengths (tools in the development repository) found that the original
had exactly the same 7,253 stabs, with strings **316 bytes shorter** than ours. The build layout
that produced this is unknown: the UUID only fixes the total. A five-character-shorter
target name would explain −320.

`scripts/legacy/build.sh` therefore relinks the GUI bundle executable from copies of its 64 objects:
- in a directory 5 characters shorter (`Synth1ViewFactory.build`), giving −320;
- with `synth1.o` renamed `synth1-pad.o`, giving +4.

It repeats the link until the UUID is `4AFD8BF6-627F-32CA-98F6-62E758161E75`, then repeats Xcode's
strip-and-copy step.

## 5. Verification

`scripts/legacy/release/expected.sha256` holds the SHA-256 of every file in the original bundle. The build passes
only if all 28 match and there are no extra files.
