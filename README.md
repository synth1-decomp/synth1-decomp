# synth1-decomp

Reconstructed source code for the Synth1 synthesizer.

## Synth1

[Synth1](https://daichilab.sakura.ne.jp/softsynth/index.html) is a popular freeware VST/AU plugin developed by Daichi.

The code in this repo is a reconstruction of Synth1 for preservation and interoperability, not the original code.

Synth1 is copyright Daichi (Daichi Laboratory). All rights to Synth1 remain with its author.

## Third-party code

Synth1 leverages several third-party libraries. 

See: [THIRD_PARTY.md](docs/THIRD_PARTY.md)

## Requirements

This repo was built by reverse engineering the Synth1 AudioUnit v1.13 beta11, the final Mac release of Synth1.

To produce a byte-for-byte match of that release we need to replicate its build environment:

- macOS 10.13.6 (High Sierra)
- Xcode 9.4.1 (build 9F2000) installed at `/Applications/Xcode941.app`

## Building

The provided `scripts/legacy/build.sh` script configures the build environment, builds the plugin, and verifies it against the original.

Options:

| Option     | Effect                                                    |
|------------|-----------------------------------------------------------|
| `--verify` | re-check an existing build                                |
| `--clean`  | remove the staging copy and DerivedData folder afterwards |
| `--force`  | skip the macOS-version and ownership checks               |

## Testing

See: [tests/README.md](tests/README.md)
