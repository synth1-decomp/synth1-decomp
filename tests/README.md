# Tests

Two test suites cover everything except the GUI and the Win32 window/Cocoa emulation. Neither touches
`src/` or the reproducible build: the byte-identical release build (`scripts/legacy/build.sh`) is unaffected.

Requirements: macOS 10.13.6 with Xcode 9.4.1 at `/Applications/Xcode941.app` (the engine needs libstdc++,
which later Xcode versions removed), the same as the release build.

## Unit tests (`tests/unit/`)

```
scripts/legacy/run_tests.sh                                # build and run all unit tests
scripts/legacy/run_tests.sh vcf lfo                        # only tests whose "suite.name" contains vcf or lfo
scripts/legacy/run_tests.sh --files 'unit/test_vc*.cpp'    # compile only some test files
scripts/legacy/run_tests.sh --coverage                     # instrumented build + coverage report of the engine sources
```

`--coverage` builds a separate `-O0` instrumented variant in `build/tests-coverage/` (clang source-based
coverage) and prints line, function and region coverage (currently 99.5% of lines and every function) for `src/techpop/` (GUI files excluded). The
functions that write Synth1's real preferences (`setEnableBankSelect`, `setEnableProgramChange`,
`setCCMap`, `setIniString`, …) are exercised against the in-memory preference store described below.

The engine sources (`src/techpop/` minus the GUI files, the Win32 file/INI emulation in
`src/winemu_au.cpp`, Ooura's FFT and zlib) are compiled with the release compiler flags
(`-Os`, gnu++98, libstdc++) into `build/tests/`, linked with the tests, and run. The four Cocoa
functions the engine calls are replaced by recording stubs (`tests/harness/cocoa_stubs.cpp`).

| Files                                                                                                                                                                   | Covers                                                                                                                                                                                   |
|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `test_common`, `test_common2`, `test_list`, `test_fftwin`, `test_sigdev`, `test_mixdev`, `test_inst`, `test_ssai`, `test_debug`                                         | common.h helpers and random generators, CCList, the FFT routines, CSignalDevice, CMixDevice, CInst MIDI dispatch, the audio interface, debug helpers                                     |
| `test_vco`, `test_vcomain`                                                                                                                                              | oscillators: wavetables and band limiting, pitch tables, every waveform, pulse width, mix, noise, sub, ring, sync, FM, super saw, glides, all 480 render variants                        |
| `test_vcf`, `test_envelope`, `test_vca`                                                                                                                                 | filters (all five types, response, resonance, stability, CBQF), the envelope (ADSR timing and shapes), the VCA                                                                           |
| `test_lfo`, `test_voice`, `test_voicectl`, `test_arp`, `test_stepvox`                                                                                                   | LFO, voice, keyboard state and voice allocation (poly/mono/legato/unison), arpeggiator, step-vox                                                                                         |
| `test_distortion`, `test_compressor`, `test_chorus`, `test_delay`, `test_eq`, `test_pan`, `test_phaser`, `test_phatcont`, `test_ringmod`, `test_decimator`, `test_efxs` | the effect chain: distortion, compressor, chorus, delay, EQ, pan, phaser, phatcont, ring mod, decimator, effector                                                                        |
| `test_sound`, `test_soundlib`                                                                                                                                           | the sound library: patch files, CPatch/CBank, bank selection, zip banks (fixtures in `tests/data/sound/`)                                                                                |
| `test_synth1`, `test_synth1conv`, `test_synmidi`, `test_synth1if`                                                                                                       | CSynthesizer1: parameters, `convValIntFloat` round trips, patch-version upgrades, plugin state chunks, MIDI, rendering, GUI notifications, the C interface                               |
| `test_prefs`, `test_synth1_settings`, `test_soundlib_setup`                                                                                                             | the settings code against the preference and folder fakes: INI helpers, enable flags, CC map persistence, MIDI learn, bank/zip folder settings, the sound library's first-instance setup |

The tests never touch the user's preferences, Documents folder or temporary folder. In the test build
only, `scripts/legacy/run_tests.sh` compiles the three engine files that use these services (`src/winemu_au.cpp`,
`src/techpop/common.cpp`, `src/techpop/sound.cpp`) with `-D` redirects to test doubles
(`tests/harness/os_fakes.h`):

- `CFPreferencesCopyAppValue` / `SetAppValue` / `AppSynchronize` use an in-memory preference store, so
  the settings code (enable flags, CC map, bank and zip folders, INI helpers) can be tested and
  inspected without changing the user's Synth1 settings;
- `FSFindFolder` resolves Documents, the temporary folder and other system folders inside a fake home
  folder under `/tmp`, created for the run and removed at exit.

The release build never sees these flags. Tests that create a synthesizer or sound library also point it
at a temporary bank folder (`soundtest::IsolatedLibrary` in `tests/unit/sound_fixture.h`).

## Audio Unit tests (`tests/au/`)

```
scripts/legacy/build.sh                                         # build/Synth1.component
scripts/legacy/run_au_tests.sh                                  # test build/Synth1.component
scripts/legacy/run_au_tests.sh path/to/Synth1.component [filter ...]
```

A small C host loads the built component, registers its factory in-process (so no logged-in session or
component registration is needed) and tests the Audio Unit through the public AU API: formats, sample
rates, the 99 parameters, MIDI, saved state round trips, the Cocoa UI property, frames per slice, and
repeated open/close. Six rendered scenarios are compared with golden hashes in `tests/au/reference.txt`,
which were generated from the **original release binary**; the rebuilt component matches them.

Notes:
- Creating the Audio Unit creates `~/Documents/Synth1Patches/soundbank00` and `zipbank` if missing, exactly
  like the plugin does in a host. This cannot be redirected from outside, so the AU tests do the same.
- The golden scenarios assume Synth1's default preferences (no `jp.daichi.synth1` domain).
- The engine never initializes the window pointer it posts parameter notifications to; with recycled
  memory a host can crash in `SetParameter` (a bug in the original release). The test host makes
  allocations zero-filled so that this cannot happen during the tests.

## Behavior pinned by the tests

The code reproduces the original release, so its behavior is the reference, including quirks. Tests
document these instead of "fixing" them; the most notable:

- `CSeqSyncAudioInterface::getSoundData` writes every sequencer chunk to the start of the buffer.
- The 12 dB filter's SSE path clips only the output above resonance 0.9 (the scalar path also clips its
  state).
- Oscillator sync computes the restart phase in 32-bit arithmetic that overflows.
- The 1.05→1.07 patch upgrade maps every chorus feedback below 127 to 64 (integer division).
- Chorus type 3 does not survive a round trip through the host parameter value.
- `CharLower()` does nothing, so upper-case `.ZIP` bank files are ignored.
- `AudioUnitReset` does not stop sounding notes.
