/**
 * @file golden_scenarios.h
 * @brief The golden render scenarios and their reference file, shared by the plug-in test hosts.
 *
 * Each scenario restores the default patch (default_state.plist), then plays its events in
 * blocks of the given sizes; the FNV-1a 64 hash of the interleaved little-endian float32 L/R
 * output must match the one in reference.txt, which was generated from the original release.
 * Plain C (C99), so C and C++ test hosts can include it.
 */
#ifndef GOLDEN_SCENARIOS_H
#define GOLDEN_SCENARIOS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* parameter IDs used by the tests (g_midiAssignParams in src/techpop/synmidi.cpp) */
enum {
    P_OSC1_SHAPE = 0, P_OSC_MIX = 5, P_FILTER_TYPE = 14, P_FILTER_FREQ = 19, P_FILTER_RES = 20,
    P_FILTER_AMT = 21, P_AMP_ATTACK = 25, P_AMP_DECAY = 26, P_AMP_SUSTAIN = 27, P_AMP_RELEASE = 28,
    P_ARP_ON = 59, P_ARP_TYPE = 31, P_ARP_BEAT = 33, P_DELAY_TIME = 35, P_DELAY_FEEDBACK = 36,
    P_LFO1_TYPE = 42, P_LFO1_SPEED = 43, P_LFO1_DEPTH = 44, P_LFO1_ON = 57, P_CHORUS_ON = 66,
    P_DELAY_ON = 65, P_UNISON_MODE = 73, P_UNISON_DETUNE = 75, P_OSC1_DETUNE = 76,
    P_EFFECT_ON = 77, P_EFFECT_TYPE = 78, P_EFFECT_CTL1 = 79, P_PHASE_SHIFT = 91,
    P_UNISON_NUM = 93, P_UNNAMED = 94
};

/** A scheduled MIDI event (absolute frame) or a parameter change (status 0, data1 = id). */
typedef struct {
    long frame;
    uint8_t status, d1, d2;
    float value;
} Event;

/** Adds n bytes to an FNV-1a 64 hash (start from 1469598103934665603). */
static inline void hash_bytes(uint64_t* h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; i++) { *h ^= b[i]; *h *= 1099511628211ULL; }
}

/** A golden scenario: sample rate, length, block sizes (used cyclically) and events. */
typedef struct {
    const char* name;
    double sr;
    long frames;
    const uint32_t* sizes;
    int nSizes;
    const Event* ev;
    int nEv;
} Scenario;

#define SEC(s, sr) ((long)((s) * (sr)))
static const uint32_t kB512[] = { 512 };
static const uint32_t kOdd[] = { 333, 17, 1024, 1, 250 };

static const Event kEvNote[] = {
    { 0, 0x90, 60, 100, 0 },
    { SEC(1.0, 44100), 0x80, 60, 0, 0 },
};

static const Event kEvChord[] = {
    { 0, 0x90, 48, 64, 0 },
    { SEC(0.1, 44100), 0x90, 55, 80, 0 },
    { SEC(0.2, 44100), 0x90, 64, 100, 0 },
    { SEC(0.3, 44100), 0x90, 71, 127, 0 },
    { SEC(1.5, 44100), 0x80, 48, 0, 0 },
    { SEC(1.5, 44100), 0x80, 55, 0, 0 },
    { SEC(1.6, 44100), 0x90, 64, 0, 0 },     /* velocity 0 = note off */
    { SEC(1.7, 44100), 0x80, 71, 0, 0 },
};

static const Event kEvFilter[] = {
    { 0, 0, P_OSC1_SHAPE, 0, 0.0f },
    { 0, 0, P_FILTER_TYPE, 0, 0.6f },
    { 0, 0, P_FILTER_FREQ, 0, 0.25f },
    { 0, 0, P_FILTER_RES, 0, 0.8f },
    { 0, 0, P_FILTER_AMT, 0, 0.9f },
    { 0, 0, P_AMP_RELEASE, 0, 0.3f },
    { 0, 0, P_DELAY_ON, 0, 0.0f },
    { 0, 0x90, 36, 110, 0 },
    { SEC(0.5, 44100), 0x80, 36, 0, 0 },
    { SEC(0.6, 44100), 0x90, 48, 90, 0 },
    { SEC(0.9, 44100), 0xE0, 0x00, 0x60, 0 }, /* pitch bend up */
    { SEC(1.2, 44100), 0xE0, 0x00, 0x40, 0 }, /* center */
    { SEC(1.3, 44100), 0, P_FILTER_FREQ, 0, 0.6f },
    { SEC(1.4, 44100), 0x80, 48, 0, 0 },
    { SEC(1.5, 44100), 0x90, 60, 127, 0 },
    { SEC(2.2, 44100), 0x80, 60, 0, 0 },
};

static const Event kEvFx48[] = {
    { 0, 0, P_UNISON_MODE, 0, 1.0f },
    { 0, 0, P_UNISON_NUM, 0, 0.5f },
    { 0, 0, P_UNISON_DETUNE, 0, 0.4f },
    { 0, 0, P_EFFECT_ON, 0, 1.0f },
    { 0, 0, P_EFFECT_TYPE, 0, 0.3f },
    { 0, 0, P_EFFECT_CTL1, 0, 0.7f },
    { 0, 0, P_DELAY_TIME, 0, 0.3f },
    { 0, 0, P_DELAY_FEEDBACK, 0, 0.6f },
    { 0, 0x90, 57, 100, 0 },
    { SEC(0.25, 48000), 0x90, 64, 100, 0 },
    { SEC(0.8, 48000), 0x80, 57, 0, 0 },
    { SEC(0.8, 48000), 0x80, 64, 0, 0 },
    { SEC(1.0, 48000), 0x90, 69, 70, 0 },
    { SEC(1.6, 48000), 0x80, 69, 0, 0 },
};

static const Event kEvArpLfo[] = {
    { 0, 0, P_ARP_ON, 0, 1.0f },
    { 0, 0, P_ARP_TYPE, 0, 0.1f },
    { 0, 0, P_ARP_BEAT, 0, 0.5f },
    { 0, 0, P_LFO1_ON, 0, 1.0f },
    { 0, 0, P_LFO1_TYPE, 0, 0.0f },
    { 0, 0, P_LFO1_SPEED, 0, 0.6f },
    { 0, 0, P_LFO1_DEPTH, 0, 0.7f },
    { 0, 0x90, 60, 100, 0 },
    { 0, 0x90, 64, 100, 0 },
    { 0, 0x90, 67, 100, 0 },
    { SEC(2.0, 44100), 0x80, 60, 0, 0 },
    { SEC(2.0, 44100), 0x80, 64, 0, 0 },
    { SEC(2.0, 44100), 0x80, 67, 0, 0 },
};

static const Event kEvMidi[] = {
    { 0, 0xB0, 64, 127, 0 },                 /* sustain pedal down */
    { 100, 0x90, 52, 90, 0 },
    { SEC(0.2, 44100), 0x80, 52, 0, 0 },     /* held by the pedal */
    { SEC(0.4, 44100), 0x90, 59, 90, 0 },
    { SEC(0.5, 44100), 0x80, 59, 0, 0 },
    { SEC(0.8, 44100), 0xB0, 7, 60, 0 },      /* volume */
    { SEC(1.0, 44100), 0xB0, 64, 0, 0 },     /* pedal up */
    { SEC(1.2, 44100), 0x90, 76, 120, 0 },
    { SEC(1.5, 44100), 0xB0, 123, 0, 0 },    /* all notes off */
};

static const Scenario kScenarios[] = {
    { "init_note_44k",   44100, SEC(2.5, 44100), kB512, 1, kEvNote,   sizeof kEvNote / sizeof(Event) },
    { "init_chord_44k",  44100, SEC(3.0, 44100), kB512, 1, kEvChord,  sizeof kEvChord / sizeof(Event) },
    { "filter_bend_44k", 44100, SEC(3.0, 44100), kB512, 1, kEvFilter, sizeof kEvFilter / sizeof(Event) },
    { "unison_fx_48k",   48000, SEC(2.5, 48000), kOdd,  5, kEvFx48,   sizeof kEvFx48 / sizeof(Event) },
    { "arp_lfo_44k",     44100, SEC(3.0, 44100), kB512, 1, kEvArpLfo, sizeof kEvArpLfo / sizeof(Event) },
    { "midi_cc_96k",     96000, SEC(2.0, 96000), kB512, 1, kEvMidi,   sizeof kEvMidi / sizeof(Event) },
};
#define NUM_SCENARIOS ((int)(sizeof kScenarios / sizeof kScenarios[0]))

/** Scales event frames given at 44.1k to the scenario rate (event tables are written at 44.1k
    except unison_fx_48k, which is already at 48k). */
static inline long scale_frame(const Scenario* s, long f)
{
    if (s->sr == 48000) return f;
    return (long)((double)f * s->sr / 44100.0);
}

/**
 * Looks up a scenario's hash in a reference file ("name hash peak" lines, '#' comments).
 * Returns 1 and the hash if found.
 */
static inline int find_reference(const char* path, const char* name, uint64_t* h)
{
    FILE* f = fopen(path, "r");
    char line[512];
    int found = 0;
    if (!f) return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char n[64];
        unsigned long long v;
        if (line[0] != '#' && sscanf(line, "%63s %llx", n, &v) == 2 && !strcmp(n, name)) {
            *h = v;
            found = 1;
        }
    }
    fclose(f);
    return found;
}

#endif
