/**
 * @file au_tests.c
 * @brief Black-box tests of the built Synth1 Audio Unit (src/audiounit/Synth1.cpp).
 *
 * Loads a Synth1.component bundle with CFBundle, registers its Synth1Factory in-process with
 * AudioComponentRegister (the system registrar does not see a new component without a GUI
 * login), and drives the AU only through the public Audio Unit API: open/initialize, stream
 * format, parameters, MIDI, ClassInfo state, Cocoa UI property, MaximumFramesPerSlice,
 * odd render sizes, repeated open/close, and golden render hashes.
 *
 * Usage:
 *   au_tests <Synth1.component> <reference.txt> <default_state.plist> [filter ...]
 *   au_tests --gen <Synth1.component> <reference.txt> <default_state.plist>
 *   au_tests --child <Synth1.component> <command> [args...]   (internal, see child_main())
 *
 * Determinism: renders are bit-identical for the same input in a FRESH PROCESS. Inside one
 * process they are not: with "osc phase shift" (parameter 91) at 0 every note starts its
 * oscillators at random phases drawn from the engine's process-wide generator g_rnd, which a
 * second instance does not reset. The golden tests therefore render each scenario in a child
 * process; the in-process comparisons use a patch with a phase shift > 0 (fixed phases).
 *
 * Slice sizes matter too: Process1() renders in chunks of up to 256 frames and the engine runs
 * its control-rate updates per chunk, so only slicings into multiples of 256 frames reproduce
 * the samples of another slicing; the golden scenarios fix their slice sizes.
 *
 * The host zero-fills malloc() (install_zero_fill()) because CSynthesizer1 leaves
 * m_hNotifyWnd uninitialized and SetParameter() would post to a garbage pointer.
 *
 * The scripts run the host with tests/au/au_isolation.c loaded, so the AU sees default settings
 * and creates its Synth1Patches folders in a temporary folder instead of ~/Documents. Child
 * processes are started directly (run_child()), so they inherit that library too.
 */

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

/* ------------------------------------------------------------------------------------------ */
/* t1-style harness                                                                           */

static int g_checksOk, g_checksFailed, g_testFailed, g_tests, g_testsFailed;
static const char* g_currentTest = "";

/** Watchdog: a test that runs longer than 60 s fails the whole run. */
static void on_alarm(int sig)
{
    (void)sig;
    printf("FAIL %s: timed out after 60 s\n", g_currentTest);
    fflush(stdout);
    _exit(1);
}

static void fail_at(const char* file, int line, const char* fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("    FAIL %s:%d: %s\n", file, line, msg);
    g_checksFailed++;
    g_testFailed = 1;
}

#define CHECK(c) do { if (c) g_checksOk++; else fail_at(__FILE__, __LINE__, "CHECK(%s)", #c); } while (0)
#define REQUIRE(c) do { if (c) g_checksOk++; else { fail_at(__FILE__, __LINE__, "REQUIRE(%s)", #c); return; } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); \
    if (a_ == b_) g_checksOk++; else fail_at(__FILE__, __LINE__, "CHECK_EQ(%s, %s): %lld != %lld", #a, #b, a_, b_); } while (0)
#define CHECK_CLOSE(a, b, t) do { double a_ = (a), b_ = (b), t_ = (t); \
    if (fabs(a_ - b_) <= t_) g_checksOk++; else fail_at(__FILE__, __LINE__, "CHECK_CLOSE(%s, %s, %s): %.9g vs %.9g", #a, #b, #t, a_, b_); } while (0)
#define CHECK_STREQ(a, b) do { const char *a_ = (a), *b_ = (b); \
    if (a_ && b_ && !strcmp(a_, b_)) g_checksOk++; else fail_at(__FILE__, __LINE__, "CHECK_STREQ(%s, %s): \"%s\" != \"%s\"", #a, #b, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); } while (0)
#define CHECK_OK(x) do { OSStatus e_ = (x); if (e_ == noErr) g_checksOk++; else fail_at(__FILE__, __LINE__, "%s -> %d", #x, (int)e_); } while (0)
#define REQUIRE_OK(x) do { OSStatus e_ = (x); if (e_ == noErr) g_checksOk++; else { fail_at(__FILE__, __LINE__, "%s -> %d", #x, (int)e_); return; } } while (0)

/* ------------------------------------------------------------------------------------------ */
/* zero-filled heap                                                                           */

/*
 * CSynthesizer1's constructor does not initialize m_hNotifyWnd (only the Cocoa view sets it),
 * and Synth1::SetParameter() posts a message to it when it is non-NULL. With a recycled heap
 * block the field holds garbage and SetParameter() crashes in PostEventMac (seen on the VM:
 * objc_msgSend on hWnd 0x0a008b008a008900). That is a bug of the original release, so the
 * test host makes malloc() return zero-filled memory, which is what a fresh process gives the
 * first instance anyway, and keeps every run deterministic.
 */
static void* (*g_zoneCalloc[16])(malloc_zone_t*, size_t, size_t);
static malloc_zone_t* g_zones[16];
static int g_nZones;

#define ZERO_MALLOC(i) static void* zero_malloc##i(malloc_zone_t* z, size_t n) { return g_zoneCalloc[i](z, 1, n); }
ZERO_MALLOC(0) ZERO_MALLOC(1) ZERO_MALLOC(2) ZERO_MALLOC(3) ZERO_MALLOC(4) ZERO_MALLOC(5) ZERO_MALLOC(6) ZERO_MALLOC(7)
ZERO_MALLOC(8) ZERO_MALLOC(9) ZERO_MALLOC(10) ZERO_MALLOC(11) ZERO_MALLOC(12) ZERO_MALLOC(13) ZERO_MALLOC(14) ZERO_MALLOC(15)
static void* (*const kZeroMalloc[16])(malloc_zone_t*, size_t) = {
    zero_malloc0, zero_malloc1, zero_malloc2, zero_malloc3, zero_malloc4, zero_malloc5, zero_malloc6, zero_malloc7,
    zero_malloc8, zero_malloc9, zero_malloc10, zero_malloc11, zero_malloc12, zero_malloc13, zero_malloc14, zero_malloc15
};

static void patch_zone(malloc_zone_t* z)
{
    int i;
    vm_address_t page;
    if (!z || g_nZones >= 16) return;
    for (i = 0; i < g_nZones; i++) if (g_zones[i] == z) return;
    page = trunc_page((vm_address_t)z);
    if (vm_protect(mach_task_self(), page, round_page((vm_address_t)z + sizeof *z) - page, 0,
                   VM_PROT_READ | VM_PROT_WRITE) != KERN_SUCCESS) return;
    g_zones[g_nZones] = z;
    g_zoneCalloc[g_nZones] = z->calloc;
    z->malloc = kZeroMalloc[g_nZones];
    g_nZones++;
    vm_protect(mach_task_self(), page, round_page((vm_address_t)z + sizeof *z) - page, 0, VM_PROT_READ);
}

static void install_zero_fill(void)
{
    vm_address_t* zones = NULL;
    unsigned int n = 0, i;
    patch_zone(malloc_default_zone());
    if (malloc_get_all_zones(mach_task_self(), NULL, &zones, &n) == KERN_SUCCESS)
        for (i = 0; i < n; i++) patch_zone((malloc_zone_t*)zones[i]);
}

/* ------------------------------------------------------------------------------------------ */
/* AU helpers                                                                                 */

static const char* g_componentPath;
static const char* g_referencePath;
static const char* g_statePath;
static char g_selfPath[2048];
static AudioComponent g_comp;
static CFBundleRef g_bundle;

#define SYNTH1_PARAM_NUM        99
#define kSynth1Property_Synth   64000
#define MAX_FRAMES_DEFAULT      1156

/* parameter IDs used below (g_midiAssignParams in src/techpop/synmidi.cpp) */
enum {
    P_OSC1_SHAPE = 0, P_OSC_MIX = 5, P_FILTER_TYPE = 14, P_FILTER_FREQ = 19, P_FILTER_RES = 20,
    P_FILTER_AMT = 21, P_AMP_ATTACK = 25, P_AMP_DECAY = 26, P_AMP_SUSTAIN = 27, P_AMP_RELEASE = 28,
    P_ARP_ON = 59, P_ARP_TYPE = 31, P_ARP_BEAT = 33, P_DELAY_TIME = 35, P_DELAY_FEEDBACK = 36,
    P_LFO1_TYPE = 42, P_LFO1_SPEED = 43, P_LFO1_DEPTH = 44, P_LFO1_ON = 57, P_CHORUS_ON = 66,
    P_DELAY_ON = 65, P_UNISON_MODE = 73, P_UNISON_DETUNE = 75, P_OSC1_DETUNE = 76,
    P_EFFECT_ON = 77, P_EFFECT_TYPE = 78, P_EFFECT_CTL1 = 79, P_PHASE_SHIFT = 91,
    P_UNISON_NUM = 93, P_UNNAMED = 94
};

static void make_format(AudioStreamBasicDescription* f, double sr, UInt32 channels)
{
    memset(f, 0, sizeof *f);
    f->mSampleRate = sr;
    f->mFormatID = kAudioFormatLinearPCM;
    f->mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f->mBytesPerPacket = 4;
    f->mFramesPerPacket = 1;
    f->mBytesPerFrame = 4;
    f->mChannelsPerFrame = channels;
    f->mBitsPerChannel = 32;
}

/** Opens an instance, sets the output format and max frames, initializes. 0 on success. */
static OSStatus au_open(AudioUnit* pAU, double sr, UInt32 maxFrames)
{
    AudioStreamBasicDescription f;
    OSStatus e = AudioComponentInstanceNew(g_comp, pAU);
    if (e) return e;
    make_format(&f, sr, 2);
    e = AudioUnitSetProperty(*pAU, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f);
    if (!e && maxFrames)
        e = AudioUnitSetProperty(*pAU, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    if (!e) e = AudioUnitInitialize(*pAU);
    if (e) { AudioComponentInstanceDispose(*pAU); *pAU = NULL; }
    return e;
}

static void au_close(AudioUnit au)
{
    if (!au) return;
    AudioUnitUninitialize(au);
    AudioComponentInstanceDispose(au);
}

static OSStatus set_param(AudioUnit au, UInt32 id, float v)
{
    return AudioUnitSetParameter(au, id, kAudioUnitScope_Global, 0, v, 0);
}

static float get_param(AudioUnit au, UInt32 id)
{
    AudioUnitParameterValue v = -1;
    AudioUnitGetParameter(au, id, kAudioUnitScope_Global, 0, &v);
    return v;
}

/** A scheduled MIDI event (absolute frame) or a parameter change (status 0, data1 = id). */
typedef struct {
    long frame;
    UInt8 status, d1, d2;
    float value;
} Event;

/** State of a render run: sample time and the stereo output (optionally kept). */
typedef struct {
    AudioUnit au;
    Float64 sampleTime;
    float* L;           /* whole output if non-NULL (capacity cap frames) */
    float* R;
    long cap;
    long pos;           /* frames rendered so far */
    uint64_t hash;      /* FNV-1a 64 of interleaved L/R float bytes */
    double peak;
    int nonFinite;
    OSStatus err;
} Run;

static void run_init(Run* r, AudioUnit au, long cap)
{
    memset(r, 0, sizeof *r);
    r->au = au;
    r->hash = 1469598103934665603ULL;
    if (cap > 0) {
        r->L = (float*)calloc(cap, sizeof(float));
        r->R = (float*)calloc(cap, sizeof(float));
        r->cap = cap;
    }
}

static void run_free(Run* r)
{
    free(r->L);
    free(r->R);
    r->L = r->R = NULL;
}

static void hash_bytes(uint64_t* h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; i++) { *h ^= b[i]; *h *= 1099511628211ULL; }
}

/** Renders one slice of n frames (n <= 8192) into the run. */
static OSStatus render_block(Run* r, UInt32 n)
{
    static float L[8192], R[8192];
    struct { UInt32 n; AudioBuffer b[2]; } bl;
    AudioTimeStamp ts;
    AudioUnitRenderActionFlags fl = 0;
    UInt32 i;
    OSStatus e;
    memset(&ts, 0, sizeof ts);
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = r->sampleTime;
    bl.n = 2;
    bl.b[0].mNumberChannels = bl.b[1].mNumberChannels = 1;
    bl.b[0].mDataByteSize = bl.b[1].mDataByteSize = n * sizeof(float);
    bl.b[0].mData = L;
    bl.b[1].mData = R;
    e = AudioUnitRender(r->au, &fl, &ts, 0, n, (AudioBufferList*)&bl);
    if (e) { if (!r->err) r->err = e; return e; }
    for (i = 0; i < n; i++) {
        double a;
        if (!isfinite(L[i]) || !isfinite(R[i])) r->nonFinite++;
        a = fabs(L[i]) > fabs(R[i]) ? fabs(L[i]) : fabs(R[i]);
        if (a > r->peak) r->peak = a;
        hash_bytes(&r->hash, &L[i], 4);
        hash_bytes(&r->hash, &R[i], 4);
        if (r->L && r->pos + (long)i < r->cap) {
            r->L[r->pos + i] = L[i];
            r->R[r->pos + i] = R[i];
        }
    }
    r->pos += n;
    r->sampleTime += n;
    return noErr;
}

/**
 * Renders total frames in slices given by the cyclic list sizes[], sending the events whose
 * frame falls into a slice with the matching offset before rendering it.
 */
static void render_events(Run* r, long total, const UInt32* sizes, int nSizes, const Event* ev, int nEv)
{
    int si = 0, ei = 0;
    long end = r->pos + total;
    while (r->pos < end) {
        UInt32 n = sizes[si++ % nSizes];
        if (r->pos + (long)n > end) n = (UInt32)(end - r->pos);
        while (ei < nEv && ev[ei].frame < r->pos + (long)n) {
            long off = ev[ei].frame - r->pos;
            if (off < 0) off = 0;
            if (ev[ei].status == 0)
                set_param(r->au, ev[ei].d1, ev[ei].value);
            else
                MusicDeviceMIDIEvent(r->au, ev[ei].status, ev[ei].d1, ev[ei].d2, (UInt32)off);
            ei++;
        }
        if (render_block(r, n)) return;
    }
}

static void render_frames(Run* r, long total, UInt32 block)
{
    render_events(r, total, &block, 1, NULL, 0);
}

static double rms(const float* p, long n)
{
    double s = 0;
    long i;
    for (i = 0; i < n; i++) s += (double)p[i] * p[i];
    return n ? sqrt(s / n) : 0;
}

static double rms_lr(const Run* r, long from, long to)
{
    if (to > r->pos) to = r->pos;
    if (from >= to) return 0;
    return sqrt((rms(r->L + from, to - from) * rms(r->L + from, to - from) +
                 rms(r->R + from, to - from) * rms(r->R + from, to - from)) / 2);
}

/*
 * The output is never exactly zero: right after construction the chain emits a decaying offset
 * of about 1e-6, and tails decay asymptotically. "Sound" therefore means above a threshold.
 */
#define AUDIBLE 1e-3

/** First frame whose |L| or |R| exceeds thr, or -1. */
static long first_above(const Run* r, double thr)
{
    long i;
    for (i = 0; i < r->pos && i < r->cap; i++)
        if (fabs(r->L[i]) > thr || fabs(r->R[i]) > thr) return i;
    return -1;
}

/** Last frame whose |L| or |R| exceeds thr, or -1. */
static long last_above(const Run* r, double thr)
{
    long i;
    for (i = (r->pos < r->cap ? r->pos : r->cap) - 1; i >= 0; i--)
        if (fabs(r->L[i]) > thr || fabs(r->R[i]) > thr) return i;
    return -1;
}

/** Estimates the fundamental (Hz) of x[0..n) by normalized autocorrelation over 50..2000 Hz. */
static double estimate_pitch(const float* x, long n, double sr)
{
    long lagMin = (long)(sr / 2000), lagMax = (long)(sr / 50), lag, best = 0, i;
    double bestVal = -1e30, r0 = 0;
    double* ac = (double*)calloc(lagMax + 2, sizeof(double));
    for (i = 0; i < n; i++) r0 += (double)x[i] * x[i];
    for (lag = lagMin; lag <= lagMax + 1; lag++) {
        double s = 0;
        for (i = 0; i + lag < n; i++) s += (double)x[i] * x[i + lag];
        ac[lag] = s / (n - lag);
    }
    /* first lag after the first zero dip whose value is within 90% of the global maximum */
    for (lag = lagMin + 1; lag <= lagMax; lag++)
        if (ac[lag] > bestVal) bestVal = ac[lag];
    for (lag = lagMin + 1; lag <= lagMax; lag++) {
        if (ac[lag] >= 0.9 * bestVal && ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1]) { best = lag; break; }
    }
    if (best > 0) {
        /* parabolic interpolation */
        double a = ac[best - 1], b = ac[best], c = ac[best + 1], d = a - 2 * b + c;
        double frac = d != 0 ? 0.5 * (a - c) / d : 0;
        free(ac);
        return sr / (best + frac);
    }
    free(ac);
    (void)r0;
    return 0;
}

static CFPropertyListRef save_state(AudioUnit au)
{
    CFPropertyListRef pl = NULL;
    UInt32 sz = sizeof pl;
    if (AudioUnitGetProperty(au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, &sz)) return NULL;
    return pl;
}

static OSStatus restore_state(AudioUnit au, CFPropertyListRef pl)
{
    return AudioUnitSetProperty(au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &pl, sizeof pl);
}

static CFDataRef state_data(CFPropertyListRef pl)
{
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) return NULL;
    return (CFDataRef)CFDictionaryGetValue((CFDictionaryRef)pl, CFSTR("data"));
}

static int cfstr(CFStringRef s, char* buf, size_t size)
{
    buf[0] = 0;
    return s && CFStringGetCString(s, buf, size, kCFStringEncodingUTF8);
}

static CFPropertyListRef load_plist(const char* path)
{
    FILE* f = fopen(path, "rb");
    long n;
    unsigned char* buf;
    CFDataRef d;
    CFPropertyListRef pl;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char*)malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    d = CFDataCreate(NULL, buf, n);
    free(buf);
    pl = CFPropertyListCreateWithData(NULL, d, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(d);
    return pl;
}

static int write_plist(const char* path, CFPropertyListRef pl)
{
    CFDataRef d = CFPropertyListCreateData(NULL, pl, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    FILE* f;
    if (!d) return 0;
    f = fopen(path, "wb");
    if (!f) { CFRelease(d); return 0; }
    fwrite(CFDataGetBytePtr(d), 1, CFDataGetLength(d), f);
    fclose(f);
    CFRelease(d);
    return 1;
}

/** Sets up a patch with fixed oscillator phases (phase shift > 0), so in-process renders repeat. */
static void fixed_phase_patch(AudioUnit au)
{
    set_param(au, P_PHASE_SHIFT, 0.3f);
}

/* ------------------------------------------------------------------------------------------ */
/* golden scenarios (rendered in a fresh child process each)                                  */

typedef struct {
    const char* name;
    double sr;
    long frames;
    const UInt32* sizes;
    int nSizes;
    const Event* ev;
    int nEv;
} Scenario;

#define SEC(s, sr) ((long)((s) * (sr)))
static const UInt32 kB512[] = { 512 };
static const UInt32 kOdd[] = { 333, 17, 1024, 1, 250 };

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
static long scale_frame(const Scenario* s, long f)
{
    if (s->sr == 48000) return f;
    return (long)((double)f * s->sr / 44100.0);
}

/** Renders a scenario (state restored from statePath first). Returns 0 and the hash on success. */
static int render_scenario(const Scenario* s, const char* statePath, uint64_t* pHash, double* pPeak)
{
    AudioUnit au;
    Run r;
    Event ev[64];
    int i;
    CFPropertyListRef pl;
    if (au_open(&au, s->sr, 4096)) return 1;
    pl = load_plist(statePath);
    if (!pl || restore_state(au, pl)) { au_close(au); return 2; }
    CFRelease(pl);
    for (i = 0; i < s->nEv; i++) { ev[i] = s->ev[i]; ev[i].frame = scale_frame(s, ev[i].frame); }
    run_init(&r, au, 0);
    render_events(&r, s->frames, s->sizes, s->nSizes, ev, s->nEv);
    au_close(au);
    if (r.err || r.nonFinite) return 3;
    *pHash = r.hash;
    *pPeak = r.peak;
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* child process mode                                                                         */

static int load_component(const char* path);

/**
 * Internal entry used by the tests to get a fresh process (fresh engine globals and g_rnd):
 *   --child <component> scenario <index> <state.plist>   -> "hash <hex> peak <x>"
 *   --child <component> save-default <out.plist>         -> writes a fresh instance's ClassInfo
 *   --child <component> setsave <out.plist>              -> sets the "restore" patch, saves it,
 *                                                           renders it: "hash <hex>"
 *   --child <component> restore <in.plist>               -> restores it, renders: "hash <hex>"
 */
static int child_main(int argc, char** argv)
{
    const char* cmd;
    if (argc < 4 || load_component(argv[2])) return 2;
    cmd = argv[3];
    if (!strcmp(cmd, "scenario") && argc >= 6) {
        uint64_t h;
        double peak;
        int idx = atoi(argv[4]);
        if (idx < 0 || idx >= NUM_SCENARIOS) return 2;
        if (render_scenario(&kScenarios[idx], argv[5], &h, &peak)) return 1;
        printf("hash %016llx peak %.6f\n", (unsigned long long)h, peak);
        return 0;
    }
    if (!strcmp(cmd, "save-default") && argc >= 5) {
        AudioUnit au;
        CFPropertyListRef pl;
        if (au_open(&au, 44100, 0)) return 1;
        pl = save_state(au);
        au_close(au);
        return pl && write_plist(argv[4], pl) ? 0 : 1;
    }
    if ((!strcmp(cmd, "setsave") || !strcmp(cmd, "restore")) && argc >= 5) {
        AudioUnit au;
        Run r;
        Event ev[2] = { { 0, 0x90, 62, 100, 0 }, { 20000, 0x80, 62, 0, 0 } };
        if (au_open(&au, 44100, 0)) return 1;
        if (!strcmp(cmd, "setsave")) {
            CFPropertyListRef pl;
            set_param(au, P_FILTER_FREQ, 0.4f);
            set_param(au, P_FILTER_RES, 0.6f);
            set_param(au, P_OSC1_SHAPE, 0.4f);      /* shape 1: see test_classinfo_initial_sound */
            set_param(au, P_AMP_RELEASE, 0.2f);
            pl = save_state(au);
            if (!pl || !write_plist(argv[4], pl)) return 1;
            CFRelease(pl);
        } else {
            CFPropertyListRef pl = load_plist(argv[4]);
            if (!pl || restore_state(au, pl)) return 1;
            CFRelease(pl);
        }
        run_init(&r, au, 0);
        render_events(&r, 60000, kB512, 1, ev, 2);
        au_close(au);
        if (r.err) return 1;
        printf("hash %016llx\n", (unsigned long long)r.hash);
        return 0;
    }
    return 2;
}

/**
 * Runs this program in child mode; returns the first output line (static) or NULL.
 * args are the child's arguments, separated by spaces; '...' quotes an argument with spaces.
 * The child is started directly, not through a shell: macOS drops DYLD_INSERT_LIBRARIES when it
 * starts /bin/sh, and the child needs it for au_isolation.c.
 */
static const char* run_child(const char* args)
{
    static char line[256];
    char buf[8192], rest[256], *p = buf, *argv[16];
    int argc = 0, fds[2], status;
    pid_t pid;
    posix_spawn_file_actions_t actions;
    FILE* out;

    argv[argc++] = g_selfPath;
    argv[argc++] = "--child";
    argv[argc++] = (char*)g_componentPath;
    snprintf(buf, sizeof buf, "%s", args);
    while (argc < 15) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '\'') {
            argv[argc++] = ++p;
            while (*p && *p != '\'') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        if (*p) *p++ = 0;
    }
    argv[argc] = NULL;

    if (pipe(fds) != 0) return NULL;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    status = posix_spawn(&pid, g_selfPath, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (status != 0) { close(fds[0]); return NULL; }

    line[0] = 0;
    out = fdopen(fds[0], "r");
    if (out) {
        if (!fgets(line, sizeof line, out)) line[0] = 0;
        while (fgets(rest, sizeof rest, out)) {}   /* let the child finish writing */
        fclose(out);
    } else {
        close(fds[0]);
    }
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return NULL;
    return line;
}

static int child_hash(const char* args, uint64_t* h)
{
    const char* line = run_child(args);
    unsigned long long v;
    if (!line || sscanf(line, "hash %llx", &v) != 1) return 0;
    *h = v;
    return 1;
}

/* ------------------------------------------------------------------------------------------ */
/* reference file                                                                             */

typedef struct { char name[64]; uint64_t hash; } RefEntry;
static RefEntry g_ref[32];
static int g_nRef = -1;

static void load_reference(void)
{
    FILE* f;
    char line[512];
    g_nRef = 0;
    f = fopen(g_referencePath, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && g_nRef < 32) {
        char name[64];
        unsigned long long h;
        if (line[0] == '#') continue;
        if (sscanf(line, "%63s %llx", name, &h) == 2) {
            strcpy(g_ref[g_nRef].name, name);
            g_ref[g_nRef].hash = h;
            g_nRef++;
        }
    }
    fclose(f);
}

static int find_ref(const char* name, uint64_t* h)
{
    int i;
    if (g_nRef < 0) load_reference();
    for (i = 0; i < g_nRef; i++)
        if (!strcmp(g_ref[i].name, name)) { *h = g_ref[i].hash; return 1; }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* tests                                                                                      */

/** The host's heap hands out zeroed blocks, even recycled ones (see install_zero_fill()). */
static void test_host_zero_fill(void)
{
    int k;
    CHECK(g_nZones > 0);
    for (k = 0; k < 4; k++) {
        size_t n = k % 2 ? 0xCF50 : 7128, i;
        unsigned char* p = (unsigned char*)malloc(n);
        int clean = 1;
        REQUIRE(p != NULL);
        for (i = 0; i < n; i++) if (p[i]) clean = 0;
        CHECK(clean);
        memset(p, 0xA5, n);
        free(p);
    }
}

/** The bundle loads, exports Synth1Factory and its Info.plist describes aumu/Syn1/daiC 1.0.0. */
static void test_bundle_component(void)
{
    CFDictionaryRef info = CFBundleGetInfoDictionary(g_bundle);
    CFArrayRef comps;
    CFDictionaryRef c;
    char s[256];
    UInt32 ver = 0;
    AudioComponentDescription d;
    REQUIRE(info != NULL);
    CHECK(cfstr((CFStringRef)CFDictionaryGetValue(info, CFSTR("CFBundleIdentifier")), s, sizeof s));
    CHECK_STREQ(s, "jp.daichi.synth1.audiounit");
    comps = (CFArrayRef)CFDictionaryGetValue(info, CFSTR("AudioComponents"));
    REQUIRE(comps && CFArrayGetCount(comps) == 1);
    c = (CFDictionaryRef)CFArrayGetValueAtIndex(comps, 0);
    cfstr((CFStringRef)CFDictionaryGetValue(c, CFSTR("type")), s, sizeof s);
    CHECK_STREQ(s, "aumu");
    cfstr((CFStringRef)CFDictionaryGetValue(c, CFSTR("subtype")), s, sizeof s);
    CHECK_STREQ(s, "Syn1");
    cfstr((CFStringRef)CFDictionaryGetValue(c, CFSTR("manufacturer")), s, sizeof s);
    CHECK_STREQ(s, "daiC");
    cfstr((CFStringRef)CFDictionaryGetValue(c, CFSTR("factoryFunction")), s, sizeof s);
    CHECK_STREQ(s, "Synth1Factory");
    cfstr((CFStringRef)CFDictionaryGetValue(c, CFSTR("name")), s, sizeof s);
    CHECK_STREQ(s, "Daichi Laboratory: Synth1");
    REQUIRE_OK(AudioComponentGetDescription(g_comp, &d));
    CHECK_EQ(d.componentType, kAudioUnitType_MusicDevice);
    CHECK_EQ(d.componentSubType, 'Syn1');
    CHECK_EQ(d.componentManufacturer, 'daiC');
    CHECK_OK(AudioComponentGetVersion(g_comp, &ver));
    CHECK_EQ(ver, 0x10000);
}

/** Open + initialize succeeds at 44.1/48/96 kHz, the rate reads back and A4 sounds at 440 Hz. */
static void test_open_init_rates(void)
{
    static const double rates[] = { 44100, 48000, 96000 };
    int k;
    for (k = 0; k < 3; k++) {
        AudioUnit au;
        AudioStreamBasicDescription f;
        UInt32 sz = sizeof f;
        Run r;
        double hz, sr = rates[k];
        long n = (long)(0.5 * sr);
        Event ev[1] = { { 0, 0x90, 69, 100, 0 } };
        REQUIRE_OK(au_open(&au, sr, 0));
        CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, &sz));
        CHECK_EQ((long)f.mSampleRate, (long)sr);
        /* single plain oscillator: osc mix fully osc1, no detune, filter wide open */
        set_param(au, P_OSC_MIX, 0.0f);
        set_param(au, P_OSC1_DETUNE, 0.0f);
        set_param(au, P_FILTER_FREQ, 1.0f);
        set_param(au, P_CHORUS_ON, 0.0f);
        set_param(au, P_DELAY_ON, 0.0f);
        run_init(&r, au, n);
        render_events(&r, n, kB512, 1, ev, 1);
        CHECK_EQ(r.err, 0);
        CHECK(r.peak > 0.01);
        hz = estimate_pitch(r.L + n / 2, n / 2, sr);
        if (fabs(hz - 440.0) > 4.0) fail_at(__FILE__, __LINE__, "pitch at %.0f Hz: %.2f Hz, expected 440", sr, hz);
        else g_checksOk++;
        run_free(&r);
        au_close(au);
    }
}

/** Default stream format: stereo non-interleaved float32 at 44.1k; one output bus, no input. */
static void test_stream_format(void)
{
    AudioUnit au;
    AudioStreamBasicDescription f, g;
    UInt32 sz = sizeof f, count = 99;
    REQUIRE_OK(AudioComponentInstanceNew(g_comp, &au));
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, &sz));
    CHECK_EQ((long)f.mSampleRate, 44100);
    CHECK_EQ(f.mFormatID, kAudioFormatLinearPCM);
    CHECK(f.mFormatFlags & kAudioFormatFlagIsFloat);
    CHECK(f.mFormatFlags & kAudioFormatFlagIsNonInterleaved);
    CHECK_EQ(f.mChannelsPerFrame, 2);
    CHECK_EQ(f.mBitsPerChannel, 32);
    CHECK_EQ(f.mBytesPerFrame, 4);
    sz = sizeof count;
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &count, &sz));
    CHECK_EQ(count, 1);
    sz = sizeof count;
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &count, &sz));
    CHECK_EQ(count, 0);
    /* a new rate is accepted and reads back */
    make_format(&g, 96000, 2);
    CHECK_OK(AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &g, sizeof g));
    sz = sizeof f;
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, &sz));
    CHECK_EQ((long)f.mSampleRate, 96000);
    CHECK_EQ(f.mChannelsPerFrame, 2);
    /* there is no output bus 1 */
    CHECK(AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &g, sizeof g) != noErr);
    CHECK_OK(AudioUnitInitialize(au));
    au_close(au);
}

/** The global scope lists the 99 parameters 0..98; other scopes list none. */
static void test_parameter_list(void)
{
    AudioUnit au;
    AudioUnitParameterID ids[128];
    UInt32 sz = 0, i;
    Boolean w;
    REQUIRE_OK(au_open(&au, 44100, 0));
    CHECK_OK(AudioUnitGetPropertyInfo(au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &sz, &w));
    CHECK_EQ(sz / sizeof(AudioUnitParameterID), SYNTH1_PARAM_NUM);
    sz = sizeof ids;
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids, &sz));
    REQUIRE(sz == SYNTH1_PARAM_NUM * sizeof(AudioUnitParameterID));
    for (i = 0; i < SYNTH1_PARAM_NUM; i++) CHECK_EQ(ids[i], i);
    sz = 123;
    if (AudioUnitGetPropertyInfo(au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Output, 0, &sz, &w) == noErr)
        CHECK_EQ(sz, 0);
    au_close(au);
}

/* names in g_midiAssignParams order by ID (NULL: ID 94 is not in the table) */
static const char* kParamNames[SYNTH1_PARAM_NUM] = {
    "osc1 shape", "osc2 shape", "osc2 pitch", "osc2 fine tune", "osc2 kbd track",
    "osc mix", "osc2 sync", "osc2 ring modulation", "osc pulse width", "osc key shift",
    "osc mod env on/off", "osc mod env amount", "osc mod env attack", "osc mod env decay", "filter type",
    "filter attack", "filter decay", "filter sustain", "filter release", "*filter freq",
    "*filter resonance", "filter amount", "filter kbd track", "filter saturation", "filter velocity switch",
    "amp attack", "amp decay", "amp sustain", "amp release", "amp gain",
    "amp velocity sens", "arpeggiator type", "arpeggiator oct range", "arpeggiator beat", "arpeggiator gate",
    "delay time", "delay feedback", "delay dry/wet", "play mode type", "portament time",
    "pitch bend range", "lfo1 destination", "lfo1 type", "lfo1 speed", "lfo1 depth",
    "osc1 FM", "lfo2 destination", "lfo2 type", "lfo2 speed", "lfo2 depth",
    "midi ctrl sens1", "midi ctrl sens2", "chorus delay time", "chorus depth", "chorus rate",
    "chorus feedback", "chorus level", "lfo1 on/off", "lfo2 on/off", "arpeggiator on/off",
    "equalizer tone", "equalizer freq", "equalizer level", "equalizer Q", "chorus type",
    "delay on/off", "chorus on/off", "lfo1 tempo sync", "lfo1 key sync", "lfo2 tempo sync",
    "lfo2 key sync", "osc mod dest", "osc1,2 fine tune", "unison mode", "portament auto mode",
    "unison detune", "osc1 detune", "effect on/off", "effect type", "effect control1",
    "effect control2", "effect level/mix", "delay type", "delay time spread", "unison pan spread",
    "unison pitch", "midi ctrl src1", "midi ctrl assign1", "midi ctrl src2", "midi ctrl assign2",
    "pan", "osc phase shift", "unison phase shift", "unison voice num", NULL,
    "osc1 sub gain", "osc1 sub shape", "osc1 sub octave", "delay tone",
};

/** ParameterInfo: 98 named, writable 0..1 (default 0.5) parameters; ID 94 read-only and unnamed. */
static void test_parameter_info(void)
{
    AudioUnit au;
    UInt32 id;
    REQUIRE_OK(au_open(&au, 44100, 0));
    for (id = 0; id < SYNTH1_PARAM_NUM; id++) {
        AudioUnitParameterInfo pi;
        UInt32 sz = sizeof pi;
        char s[256];
        memset(&pi, 0, sizeof pi);
        CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &pi, &sz));
        if (kParamNames[id] == NULL) {
            CHECK_EQ(pi.flags, kAudioUnitParameterFlag_IsReadable);
            CHECK(pi.cfNameString == NULL);
            CHECK_EQ(pi.name[0], 0);
            continue;
        }
        CHECK_EQ(pi.flags, kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable |
                           kAudioUnitParameterFlag_HasCFNameString | kAudioUnitParameterFlag_CFNameRelease);
        CHECK_STREQ(pi.name, kParamNames[id]);
        CHECK(cfstr(pi.cfNameString, s, sizeof s));
        CHECK_STREQ(s, kParamNames[id]);
        CHECK_EQ(pi.unit, kAudioUnitParameterUnit_Generic);
        CHECK_CLOSE(pi.minValue, 0.0, 0);
        CHECK_CLOSE(pi.maxValue, 1.0, 0);
        CHECK_CLOSE(pi.defaultValue, 0.5, 0);
        if (pi.cfNameString) CFRelease(pi.cfNameString);
    }
    {
        AudioUnitParameterInfo pi;
        UInt32 sz = sizeof pi;
        memset(&pi, 0, sizeof pi);
        CHECK_EQ(AudioUnitGetProperty(au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, SYNTH1_PARAM_NUM, &pi, &sz),
                 kAudioUnitErr_InvalidParameter);
        sz = sizeof pi;
        CHECK(AudioUnitGetProperty(au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Output, 5, &pi, &sz) != noErr);
    }
    au_close(au);
}

/**
 * Set/get round trips: each value reads back inside 0..1, quantized to the parameter's steps,
 * and setting the read-back value again is a fixed point. Values of 0 and 1 are preserved for
 * continuous parameters, and get() is monotonic in set() over a sweep.
 */
static void test_parameter_roundtrip(void)
{
    static const float probes[] = { 0.0f, 0.1f, 0.25f, 0.5f, 0.6f, 0.77f, 0.9f, 1.0f };
    AudioUnit au;
    UInt32 id;
    int k;
    REQUIRE_OK(au_open(&au, 44100, 0));
    {
        AudioUnitParameterValue v;
        CHECK(AudioUnitGetParameter(au, SYNTH1_PARAM_NUM, kAudioUnitScope_Global, 0, &v) != noErr);
    }
    for (id = 0; id < SYNTH1_PARAM_NUM; id++) {
        float prev = -1;
        int monotonic = 1;
        if (id == P_UNNAMED) continue;
        for (k = 0; k < 8; k++) {
            float g1, g2;
            CHECK_OK(set_param(au, id, probes[k]));
            g1 = get_param(au, id);
            if (!(g1 >= 0.0f && g1 <= 1.0f)) fail_at(__FILE__, __LINE__, "param %u set %g -> get %g out of 0..1", id, probes[k], g1);
            else g_checksOk++;
            CHECK_OK(set_param(au, id, g1));
            g2 = get_param(au, id);
            if (g2 != g1) fail_at(__FILE__, __LINE__, "param %u: get %g not a fixed point (-> %g)", id, g1, g2);
            else g_checksOk++;
            if (g1 < prev) monotonic = 0;
            prev = g1;
        }
        if (!monotonic) fail_at(__FILE__, __LINE__, "param %u not monotonic", id);
        else g_checksOk++;
    }
    /* continuous 0..127 parameters read back as bin centres (2v + 1) / 256 */
    CHECK_OK(set_param(au, P_FILTER_FREQ, 0.0f));
    CHECK_CLOSE(get_param(au, P_FILTER_FREQ), 1.0 / 256, 0);
    CHECK_OK(set_param(au, P_FILTER_FREQ, 1.0f));
    CHECK_CLOSE(get_param(au, P_FILTER_FREQ), 255.0 / 256, 0);
    CHECK_OK(set_param(au, P_FILTER_FREQ, 0.5f));
    CHECK_CLOSE(get_param(au, P_FILTER_FREQ), 129.0 / 256, 0);
    /* on/off switches read back as 0.25 / 0.75 */
    CHECK_OK(set_param(au, P_DELAY_ON, 0.9f));
    CHECK_CLOSE(get_param(au, P_DELAY_ON), 0.75, 0);
    CHECK_OK(set_param(au, P_DELAY_ON, 0.1f));
    CHECK_CLOSE(get_param(au, P_DELAY_ON), 0.25, 0);
    /* osc1 shape: four choices at 1/8, 3/8, 5/8, 7/8 */
    CHECK_OK(set_param(au, P_OSC1_SHAPE, 0.3f));
    CHECK_CLOSE(get_param(au, P_OSC1_SHAPE), 0.375, 0);
    CHECK_OK(set_param(au, P_OSC1_SHAPE, 0.99f));
    CHECK_CLOSE(get_param(au, P_OSC1_SHAPE), 0.875, 0);
    /* values are per instance */
    {
        AudioUnit au2;
        REQUIRE_OK(au_open(&au2, 44100, 0));
        set_param(au, P_FILTER_RES, 1.0f);
        set_param(au2, P_FILTER_RES, 0.0f);
        CHECK_CLOSE(get_param(au, P_FILTER_RES), 255.0 / 256, 0);
        CHECK_CLOSE(get_param(au2, P_FILTER_RES), 1.0 / 256, 0);
        au_close(au2);
    }
    au_close(au);
}

/** A parameter change is audible: closing the filter makes the output much quieter. */
static void test_parameter_affects_sound(void)
{
    AudioUnit au;
    Run a, b;
    Event ev[1] = { { 0, 0x90, 60, 100, 0 } };
    REQUIRE_OK(au_open(&au, 44100, 0));
    fixed_phase_patch(au);
    set_param(au, P_FILTER_FREQ, 1.0f);
    set_param(au, P_DELAY_ON, 0.0f);
    set_param(au, P_CHORUS_ON, 0.0f);
    run_init(&a, au, 22050);
    render_events(&a, 22050, kB512, 1, ev, 1);
    MusicDeviceMIDIEvent(au, 0xB0, 123, 0, 0);
    render_frames(&a, 44100, 512);
    set_param(au, P_FILTER_FREQ, 0.0f);
    run_init(&b, au, 22050);
    render_events(&b, 22050, kB512, 1, ev, 1);
    CHECK(rms_lr(&a, 4410, 22050) > 0.02);
    CHECK(rms_lr(&b, 4410, 22050) < 0.25 * rms_lr(&a, 4410, 22050));
    run_free(&a);
    run_free(&b);
    au_close(au);
}

/** Near-silence before any note; a note sounds while held and decays below -80 dB after note off. */
static void test_note_on_off(void)
{
    AudioUnit au;
    Run r;
    long sr = 44100, offAt = sr, total = 4 * sr, last;
    Event ev[2] = { { (long)(0.25 * 44100), 0x90, 60, 100, 0 }, { 44100, 0x80, 60, 0, 0 } };
    REQUIRE_OK(au_open(&au, 44100, 0));
    run_init(&r, au, total);
    render_events(&r, total, kB512, 1, ev, 2);
    CHECK_EQ(r.err, 0);
    CHECK_EQ(r.nonFinite, 0);
    CHECK(first_above(&r, 1e-5) >= ev[0].frame);      /* only the tiny start-up offset before */
    CHECK(first_above(&r, 1e-5) < ev[0].frame + 64);
    CHECK(rms_lr(&r, ev[0].frame + 2205, offAt) > 0.02);
    CHECK(r.peak < 2.0);
    /* decays after release: each 0.25 s window quieter than the held level */
    CHECK(rms_lr(&r, offAt + sr / 2, offAt + 3 * sr / 4) < 0.5 * rms_lr(&r, offAt - sr / 4, offAt));
    last = last_above(&r, 1e-4);
    CHECK(last > offAt);
    CHECK(last < total - sr);       /* below 1e-4 for the last second (the default patch has delay on) */
    run_free(&r);
    au_close(au);
}

/** Two held notes are louder than one (polyphony) and note-on with velocity 0 releases a note. */
static void test_polyphony_and_velocity0(void)
{
    AudioUnit au;
    Run one, two, v0;
    Event e1[1] = { { 0, 0x90, 60, 100, 0 } };
    Event e2[2] = { { 0, 0x90, 60, 100, 0 }, { 0, 0x90, 67, 100, 0 } };
    Event e3[2] = { { 0, 0x90, 60, 100, 0 }, { 11025, 0x90, 60, 0, 0 } };
    long n = 22050;
    REQUIRE_OK(au_open(&au, 44100, 0));
    fixed_phase_patch(au);
    set_param(au, P_DELAY_ON, 0.0f);
    set_param(au, P_CHORUS_ON, 0.0f);
    set_param(au, P_AMP_RELEASE, 0.0f);
    run_init(&one, au, n);
    render_events(&one, n, kB512, 1, e1, 1);
    MusicDeviceMIDIEvent(au, 0xB0, 123, 0, 0);
    render_frames(&one, 44100, 512);
    run_init(&two, au, n);
    render_events(&two, n, kB512, 1, e2, 2);
    MusicDeviceMIDIEvent(au, 0xB0, 123, 0, 0);
    render_frames(&two, 44100, 512);
    run_init(&v0, au, n);
    render_events(&v0, n, kB512, 1, e3, 2);
    CHECK(rms_lr(&two, 4410, n) > 1.2 * rms_lr(&one, 4410, n));
    CHECK(rms_lr(&v0, 4410, 11025) > 0.02);
    CHECK(rms_lr(&v0, 11025 + 8820, n) < 0.1 * rms_lr(&v0, 4410, 11025));
    run_free(&one);
    run_free(&two);
    run_free(&v0);
    au_close(au);
}

/** CC 123 (all notes off) and CC 120 (all sound off) release every held voice. */
static void test_all_notes_off(void)
{
    static const UInt8 ccs[2] = { 123, 120 };
    int k;
    for (k = 0; k < 2; k++) {
        AudioUnit au;
        Run r;
        long n = 3 * 44100, off = 22050;
        Event ev[4] = { { 0, 0x90, 48, 100, 0 }, { 0, 0x90, 60, 100, 0 }, { 0, 0x90, 64, 100, 0 },
                        { 22050, 0xB0, 0, 0, 0 } };
        ev[3].d1 = ccs[k];
        REQUIRE_OK(au_open(&au, 44100, 0));
        set_param(au, P_DELAY_ON, 0.0f);
        set_param(au, P_CHORUS_ON, 0.0f);
        run_init(&r, au, n);
        render_events(&r, n, kB512, 1, ev, 4);
        CHECK(rms_lr(&r, 4410, off) > 0.02);
        CHECK(rms_lr(&r, off + 44100, off + 2 * 44100) < 0.01 * rms_lr(&r, 4410, off));
        CHECK(last_above(&r, 1e-4) < n - 44100 / 2);
        run_free(&r);
        au_close(au);
    }
}

/**
 * AudioUnitReset succeeds but does not stop the engine's voices (Synth1 does not override Reset;
 * the base class only resets its own unused TestNotes). Characterization of the release.
 */
static void test_reset(void)
{
    AudioUnit au;
    Run a, b;
    Event ev[1] = { { 0, 0x90, 60, 100, 0 } };
    REQUIRE_OK(au_open(&au, 44100, 0));
    set_param(au, P_DELAY_ON, 0.0f);
    set_param(au, P_CHORUS_ON, 0.0f);
    run_init(&a, au, 22050);
    render_events(&a, 22050, kB512, 1, ev, 1);
    CHECK_OK(AudioUnitReset(au, kAudioUnitScope_Global, 0));
    run_init(&b, au, 22050);
    render_frames(&b, 22050, 512);
    CHECK(rms_lr(&a, 4410, 22050) > 0.02);
    CHECK(rms_lr(&b, 0, 22050) > 0.5 * rms_lr(&a, 4410, 22050));   /* still sounding */
    /* the parameters survive a reset */
    set_param(au, P_FILTER_FREQ, 0.25f);
    {
        float before = get_param(au, P_FILTER_FREQ);
        CHECK_OK(AudioUnitReset(au, kAudioUnitScope_Global, 0));
        CHECK_CLOSE(get_param(au, P_FILTER_FREQ), before, 0);
    }
    /* all notes off still silences it after the reset */
    MusicDeviceMIDIEvent(au, 0xB0, 123, 0, 0);
    run_free(&b);
    run_init(&b, au, 3 * 44100);
    render_frames(&b, 3 * 44100, 512);
    CHECK(rms_lr(&b, 2 * 44100, 3 * 44100) < 0.001);
    run_free(&a);
    run_free(&b);
    au_close(au);
}

/** ClassInfo: dictionary keys, chunk header, parameter round trip, preset name, error cases. */
static void test_classinfo_roundtrip(void)
{
    AudioUnit a, b;
    CFPropertyListRef pl, pl2;
    CFDictionaryRef d;
    CFDataRef data;
    CFNumberRef num;
    SInt32 v;
    char s[256];
    UInt32 id;
    AUPreset pr;
    UInt32 sz;
    REQUIRE_OK(au_open(&a, 44100, 0));
    REQUIRE_OK(au_open(&b, 44100, 0));
    set_param(a, P_FILTER_FREQ, 0.2f);
    set_param(a, P_FILTER_RES, 0.9f);
    set_param(a, P_OSC1_SHAPE, 0.7f);
    set_param(a, P_DELAY_ON, 0.0f);
    set_param(a, P_UNISON_MODE, 1.0f);
    set_param(a, P_LFO1_SPEED, 0.33f);
    pl = save_state(a);
    REQUIRE(pl != NULL);
    REQUIRE(CFGetTypeID(pl) == CFDictionaryGetTypeID());
    d = (CFDictionaryRef)pl;
    num = (CFNumberRef)CFDictionaryGetValue(d, CFSTR("type"));
    CHECK(num && CFNumberGetValue(num, kCFNumberSInt32Type, &v) && v == 'aumu');
    num = (CFNumberRef)CFDictionaryGetValue(d, CFSTR("subtype"));
    CHECK(num && CFNumberGetValue(num, kCFNumberSInt32Type, &v) && v == 'Syn1');
    num = (CFNumberRef)CFDictionaryGetValue(d, CFSTR("manufacturer"));
    CHECK(num && CFNumberGetValue(num, kCFNumberSInt32Type, &v) && v == 'daiC');
    num = (CFNumberRef)CFDictionaryGetValue(d, CFSTR("version"));
    CHECK(num && CFNumberGetValue(num, kCFNumberSInt32Type, &v) && v == 0);
    CHECK(cfstr((CFStringRef)CFDictionaryGetValue(d, CFSTR("name")), s, sizeof s) && s[0]);
    data = state_data(pl);
    REQUIRE(data != NULL);
    CHECK(CFDataGetLength(data) > 16 && CFDataGetLength(data) <= 0x1000);
    CHECK(!memcmp(CFDataGetBytePtr(data), "Synth1 VST Chunk", 16));
    /* restore into another instance: every parameter matches */
    REQUIRE_OK(restore_state(b, pl));
    for (id = 0; id < SYNTH1_PARAM_NUM; id++) {
        float x = get_param(a, id), y = get_param(b, id);
        if (x != y) fail_at(__FILE__, __LINE__, "param %u: %g after restore, %g saved", id, y, x);
        else g_checksOk++;
    }
    /* saving the restored state gives the same chunk bytes */
    pl2 = save_state(b);
    REQUIRE(pl2 != NULL);
    CHECK(CFEqual(state_data(pl), state_data(pl2)));
    CHECK(CFEqual(CFDictionaryGetValue(d, CFSTR("name")), CFDictionaryGetValue((CFDictionaryRef)pl2, CFSTR("name"))));
    /* the restore makes the sound's name the present preset */
    sz = sizeof pr;
    memset(&pr, 0, sizeof pr);
    CHECK_OK(AudioUnitGetProperty(b, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &pr, &sz));
    CHECK_EQ(pr.presetNumber, 0);
    {
        char name[256];
        CHECK(cfstr(pr.presetName, name, sizeof name));
        CHECK_STREQ(name, s);
        if (pr.presetName) CFRelease(pr.presetName);
    }
    /* a dictionary without "data" is rejected; unrecognized data is ignored */
    {
        CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(NULL, 0, d);
        float before = get_param(b, P_FILTER_RES);
        static const UInt8 junk[64] = "this is not a synth1 chunk";
        CFDataRef jd = CFDataCreate(NULL, junk, sizeof junk);
        CFDictionaryRemoveValue(m, CFSTR("data"));
        CHECK(restore_state(b, m) != noErr);
        CFDictionarySetValue(m, CFSTR("data"), jd);
        CHECK_OK(restore_state(b, m));
        CHECK_CLOSE(get_param(b, P_FILTER_RES), before, 0);
        CFRelease(jd);
        CFRelease(m);
    }
    CFRelease(pl);
    CFRelease(pl2);
    au_close(a);
    au_close(b);
}

/** A restored state reproduces the render of the saved one (in-process, fixed phases). */
static void test_classinfo_render(void)
{
    AudioUnit a, b, c;
    CFPropertyListRef pl;
    Run ra, rb, rc;
    Event ev[2] = { { 0, 0x90, 57, 100, 0 }, { 30000, 0x80, 57, 0, 0 } };
    REQUIRE_OK(au_open(&a, 44100, 0));
    fixed_phase_patch(a);
    set_param(a, P_FILTER_FREQ, 0.45f);
    set_param(a, P_FILTER_RES, 0.7f);
    set_param(a, P_OSC1_SHAPE, 0.4f);
    set_param(a, P_EFFECT_ON, 1.0f);
    pl = save_state(a);
    REQUIRE(pl != NULL);
    REQUIRE_OK(au_open(&b, 44100, 0));
    REQUIRE_OK(restore_state(b, pl));
    REQUIRE_OK(au_open(&c, 44100, 0));
    fixed_phase_patch(c);                   /* same phase mode, other settings default */
    run_init(&ra, a, 0);
    run_init(&rb, b, 0);
    run_init(&rc, c, 0);
    render_events(&ra, 66150, kB512, 1, ev, 2);
    render_events(&rb, 66150, kB512, 1, ev, 2);
    render_events(&rc, 66150, kB512, 1, ev, 2);
    CHECK(ra.peak > 0.01);
    if (ra.hash != rb.hash) fail_at(__FILE__, __LINE__, "restored render differs: %016llx vs %016llx",
                                    (unsigned long long)ra.hash, (unsigned long long)rb.hash);
    else g_checksOk++;
    CHECK(ra.hash != rc.hash);              /* and the patch did matter */
    CFRelease(pl);
    au_close(a);
    au_close(b);
    au_close(c);
}

/**
 * Restoring a state re-applies all 99 parameters in the chunk's order. For the initial sound
 * (osc1 shape 2 with LFO2 on) this does not leave the engine exactly as the constructor's
 * loadProgram() did: a restored instance renders the same sound, but not the same samples, as
 * the instance it was saved from. With any other osc1 shape the render is bit-identical (see
 * test_classinfo_render). Characterization of the release.
 */
static void test_classinfo_initial_sound(void)
{
    AudioUnit a, b;
    CFPropertyListRef pl;
    Run ra, rb;
    long n = 44100, w;
    Event ev[2] = { { 0, 0x90, 62, 100, 0 }, { 20480, 0x80, 62, 0, 0 } };
    REQUIRE_OK(au_open(&a, 44100, 0));
    fixed_phase_patch(a);
    pl = save_state(a);
    REQUIRE(pl != NULL);
    REQUIRE_OK(au_open(&b, 44100, 0));
    REQUIRE_OK(restore_state(b, pl));
    run_init(&ra, a, n);
    run_init(&rb, b, n);
    render_events(&ra, n, kB512, 1, ev, 2);
    render_events(&rb, n, kB512, 1, ev, 2);
    CHECK(ra.hash != rb.hash);
    for (w = 0; w + 2205 <= n; w += 2205) {
        double x = rms_lr(&ra, w, w + 2205), y = rms_lr(&rb, w, w + 2205);
        if (x < 0.005) continue;
        if (fabs(20 * log10(y / x)) > 1.0) fail_at(__FILE__, __LINE__, "window at %ld: rms %g restored vs %g", w, y, x);
        else g_checksOk++;
    }
    CHECK_CLOSE(estimate_pitch(rb.L + 4096, 8192, 44100), estimate_pitch(ra.L + 4096, 8192, 44100), 2.0);
    /* changing osc1 shape away from 2 in both makes them identical again */
    run_free(&ra);
    run_free(&rb);
    CFRelease(pl);
    au_close(a);
    au_close(b);
    REQUIRE_OK(au_open(&a, 44100, 0));
    fixed_phase_patch(a);
    set_param(a, P_OSC1_SHAPE, 0.4f);
    pl = save_state(a);
    REQUIRE(pl != NULL);
    REQUIRE_OK(au_open(&b, 44100, 0));
    REQUIRE_OK(restore_state(b, pl));
    run_init(&ra, a, 0);
    run_init(&rb, b, 0);
    render_events(&ra, n, kB512, 1, ev, 2);
    render_events(&rb, n, kB512, 1, ev, 2);
    CHECK(ra.hash == rb.hash);
    CFRelease(pl);
    au_close(a);
    au_close(b);
}

/** Across processes (random phases): saving in one process and restoring in a fresh one renders identically. */
static void test_classinfo_render_processes(void)
{
    char dir[] = "/tmp/au_tests.XXXXXX", path[256], args[512];
    uint64_t h1 = 0, h2 = 0;
    REQUIRE(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/state.plist", dir);
    snprintf(args, sizeof args, "setsave '%s'", path);
    CHECK(child_hash(args, &h1));
    snprintf(args, sizeof args, "restore '%s'", path);
    CHECK(child_hash(args, &h2));
    CHECK(h1 != 0);
    if (h1 != h2) fail_at(__FILE__, __LINE__, "restored render differs: %016llx vs %016llx",
                          (unsigned long long)h1, (unsigned long long)h2);
    else g_checksOk++;
    unlink(path);
    rmdir(dir);
}

/** No factory presets; the present preset starts as "Untitled" (-1) and can be set by the host. */
static void test_presets(void)
{
    AudioUnit au;
    UInt32 sz = 0;
    Boolean w;
    AUPreset pr, np;
    char s[256];
    REQUIRE_OK(au_open(&au, 44100, 0));
    CHECK_EQ(AudioUnitGetPropertyInfo(au, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &sz, &w),
             kAudioUnitErr_InvalidProperty);
    sz = sizeof pr;
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &pr, &sz));
    CHECK_EQ(pr.presetNumber, -1);
    CHECK(cfstr(pr.presetName, s, sizeof s));
    CHECK_STREQ(s, "Untitled");
    if (pr.presetName) CFRelease(pr.presetName);
    /* a user preset (number < 0) with a name is accepted and reads back */
    np.presetNumber = -1;
    np.presetName = CFSTR("my sound");
    CHECK_OK(AudioUnitSetProperty(au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &np, sizeof np));
    sz = sizeof pr;
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &pr, &sz));
    CHECK(cfstr(pr.presetName, s, sizeof s));
    CHECK_STREQ(s, "my sound");
    if (pr.presetName) CFRelease(pr.presetName);
    /* there is no factory preset 0 to select */
    np.presetNumber = 0;
    CHECK(AudioUnitSetProperty(au, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &np, sizeof np) != noErr);
    au_close(au);
}

/** CocoaUI points at Contents/Resources/Synth1CocoaViewFactory.bundle with class Synth1CocoaViewFactory. */
static void test_cocoa_ui(void)
{
    AudioUnit au;
    UInt32 sz = 0;
    Boolean w = true;
    AudioUnitCocoaViewInfo vi;
    char p[2048], expect[2048], s[256], real1[PATH_MAX], real2[PATH_MAX];
    struct stat st;
    REQUIRE_OK(au_open(&au, 44100, 0));
    CHECK_OK(AudioUnitGetPropertyInfo(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &sz, &w));
    CHECK_EQ(sz, sizeof(AudioUnitCocoaViewInfo));
    CHECK(!w);
    sz = sizeof vi;
    memset(&vi, 0, sizeof vi);
    REQUIRE_OK(AudioUnitGetProperty(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &vi, &sz));
    REQUIRE(vi.mCocoaAUViewBundleLocation != NULL);
    CHECK(CFURLGetFileSystemRepresentation(vi.mCocoaAUViewBundleLocation, true, (UInt8*)p, sizeof p));
    snprintf(expect, sizeof expect, "%s/Contents/Resources/Synth1CocoaViewFactory.bundle", g_componentPath);
    CHECK(realpath(p, real1) != NULL);
    CHECK(realpath(expect, real2) != NULL);
    CHECK_STREQ(real1, real2);
    CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(cfstr(vi.mCocoaAUViewClass[0], s, sizeof s));
    CHECK_STREQ(s, "Synth1CocoaViewFactory");
    /* the view bundle is loadable and names its factory class */
    {
        CFBundleRef vb = CFBundleCreate(NULL, vi.mCocoaAUViewBundleLocation);
        CHECK(vb != NULL);
        if (vb) {
            CHECK(cfstr(CFBundleGetIdentifier(vb), s, sizeof s));
            CHECK_STREQ(s, "jp.daichi.synth1.audiounit.ui");
            CHECK(cfstr((CFStringRef)CFBundleGetValueForInfoDictionaryKey(vb, CFSTR("NSPrincipalClass")), s, sizeof s));
            CHECK_STREQ(s, "Synth1CocoaViewFactory");
            CFRelease(vb);
        }
    }
    CFRelease(vi.mCocoaAUViewBundleLocation);
    if (vi.mCocoaAUViewClass[0]) CFRelease(vi.mCocoaAUViewClass[0]);
    /* the private engine property: two pointers, read-only; the s1g half is shared by instances */
    {
        AudioUnit au2;
        void* p1[2] = { 0, 0 };
        void* p2[2] = { 0, 0 };
        sz = 0;
        w = true;
        CHECK_OK(AudioUnitGetPropertyInfo(au, kSynth1Property_Synth, kAudioUnitScope_Global, 0, &sz, &w));
        CHECK_EQ(sz, 2 * sizeof(void*));
        CHECK(!w);
        REQUIRE_OK(au_open(&au2, 44100, 0));
        sz = sizeof p1;
        CHECK_OK(AudioUnitGetProperty(au, kSynth1Property_Synth, kAudioUnitScope_Global, 0, p1, &sz));
        sz = sizeof p2;
        CHECK_OK(AudioUnitGetProperty(au2, kSynth1Property_Synth, kAudioUnitScope_Global, 0, p2, &sz));
        CHECK(p1[0] && p1[1] && p2[0] && p2[1]);
        CHECK(p1[0] != p2[0]);
        CHECK(p1[1] == p2[1]);
        au_close(au2);
    }
    au_close(au);
}

/** MaximumFramesPerSlice: default 1156, settable before init, slices up to it render, larger ones fail. */
static void test_max_frames(void)
{
    AudioUnit au;
    UInt32 mf = 0, sz = sizeof mf, big = 4096;
    Run r;
    Event ev[1] = { { 0, 0x90, 60, 100, 0 } };
    REQUIRE_OK(AudioComponentInstanceNew(g_comp, &au));
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mf, &sz));
    CHECK_EQ(mf, MAX_FRAMES_DEFAULT);
    CHECK_OK(AudioUnitInitialize(au));
    run_init(&r, au, 0);
    CHECK_OK(render_block(&r, MAX_FRAMES_DEFAULT));
    CHECK_EQ(render_block(&r, MAX_FRAMES_DEFAULT + 1), kAudioUnitErr_TooManyFramesToProcess);
    au_close(au);

    REQUIRE_OK(au_open(&au, 44100, big));
    sz = sizeof mf;
    CHECK_OK(AudioUnitGetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mf, &sz));
    CHECK_EQ(mf, big);
    run_init(&r, au, 3 * big);
    render_events(&r, 3 * big, &big, 1, ev, 1);
    CHECK_EQ(r.err, 0);
    CHECK(r.peak > 0.01);
    CHECK_EQ(render_block(&r, big + 1), kAudioUnitErr_TooManyFramesToProcess);
    run_free(&r);
    /* lowering it while initialized: the smaller limit applies */
    mf = 256;
    if (AudioUnitSetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mf, sizeof mf) == noErr) {
        run_init(&r, au, 0);
        CHECK_OK(render_block(&r, 256));
        CHECK_EQ(render_block(&r, 257), kAudioUnitErr_TooManyFramesToProcess);
    }
    au_close(au);
}

/** Max |a - b| over both channels of two kept runs. */
static double max_diff(const Run* a, const Run* b, long n)
{
    double m = 0;
    long i;
    for (i = 0; i < n; i++) {
        double d = fabs(a->L[i] - b->L[i]), e = fabs(a->R[i] - b->R[i]);
        if (d > m) m = d;
        if (e > m) m = e;
    }
    return m;
}

/**
 * Slice sizes. Synth1::Process1() renders in chunks of at most 256 frames, and the engine
 * updates its control-rate state (envelopes, LFOs, volume smoothing) once per chunk, so the
 * samples depend on how the host slices the stream: slices that are multiples of 256 give
 * exactly the 512-frame result, other sizes give the same sound (level envelope and pitch)
 * but not the same samples.
 */
static void test_odd_block_sizes(void)
{
    static const UInt32 mult256[] = { 256, 768, 1024, 256 };
    static const UInt32 odd[] = { 1, 7, 13, 100, 333, 1156, 2, 511, 17 };
    AudioUnit a, b, c;
    Run ra, rb, rc;
    long n = 44100, w;
    Event ev[2] = { { 0, 0x90, 57, 100, 0 }, { 30000, 0x80, 57, 0, 0 } };
    REQUIRE_OK(au_open(&a, 44100, 0));
    REQUIRE_OK(au_open(&b, 44100, 0));
    REQUIRE_OK(au_open(&c, 44100, 0));
    fixed_phase_patch(a);
    fixed_phase_patch(b);
    fixed_phase_patch(c);
    run_init(&ra, a, n);
    run_init(&rb, b, n);
    run_init(&rc, c, n);
    render_events(&ra, n, kB512, 1, ev, 2);
    render_events(&rb, n, mult256, 4, ev, 2);
    render_events(&rc, n, odd, 9, ev, 2);
    CHECK_EQ(ra.err + rb.err + rc.err, 0);
    CHECK_EQ(ra.nonFinite + rb.nonFinite + rc.nonFinite, 0);
    CHECK(ra.peak > 0.01);
    if (ra.hash != rb.hash) fail_at(__FILE__, __LINE__, "256-multiple slices differ from 512 (max diff %g)", max_diff(&ra, &rb, n));
    else g_checksOk++;
    CHECK(ra.hash != rc.hash);
    /* same level envelope (50 ms windows, within 1 dB while sounding) */
    for (w = 0; w + 2205 <= n; w += 2205) {
        double x = rms_lr(&ra, w, w + 2205), y = rms_lr(&rc, w, w + 2205);
        if (x < 0.005) continue;
        if (fabs(20 * log10(y / x)) > 1.0)
            fail_at(__FILE__, __LINE__, "window at %ld: rms %g with odd slices vs %g", w, y, x);
        else g_checksOk++;
    }
    /* same pitch */
    CHECK_CLOSE(estimate_pitch(rc.L + 8192, 8192, 44100), estimate_pitch(ra.L + 8192, 8192, 44100), 2.0);
    run_free(&ra);
    run_free(&rb);
    run_free(&rc);
    au_close(a);
    au_close(b);
    au_close(c);

    /* event offsets are rounded down to 16 frames: 0, 5, 15 -> slice start; 16, 20 -> +16; ... */
    {
        static const UInt32 offs[] = { 0, 5, 15, 16, 20, 47, 48, 500 };
        long base = -1;
        int k;
        for (k = 0; k < 8; k++) {
            AudioUnit au;
            Run r;
            long onset;
            REQUIRE_OK(au_open(&au, 44100, 0));
            fixed_phase_patch(au);
            run_init(&r, au, 4096);
            render_block(&r, 512);
            MusicDeviceMIDIEvent(au, 0x90, 60, 100, offs[k]);
            render_block(&r, 512);
            render_block(&r, 512);
            onset = first_above(&r, 1e-5);
            if (k == 0) base = onset;
            CHECK(onset >= 512);
            if (onset != base + (long)(offs[k] & ~15u))
                fail_at(__FILE__, __LINE__, "offset %u: onset %ld, expected %ld", offs[k], onset, base + (long)(offs[k] & ~15u));
            else g_checksOk++;
            run_free(&r);
            au_close(au);
        }
    }
}

/** Many events in one slice (the queue grows) are all played: 64 notes on, then all off. */
static void test_many_events(void)
{
    AudioUnit au;
    Run r;
    int i;
    REQUIRE_OK(au_open(&au, 44100, 0));
    set_param(au, P_DELAY_ON, 0.0f);
    set_param(au, P_CHORUS_ON, 0.0f);
    run_init(&r, au, 3 * 44100);
    for (i = 0; i < 64; i++) MusicDeviceMIDIEvent(au, 0x90, 36 + i, 100, i * 8);
    MusicDeviceMIDIEvent(au, 0xF0, 0, 0, 0);    /* sysex status: ignored */
    render_frames(&r, 22050, 512);
    for (i = 0; i < 64; i++) MusicDeviceMIDIEvent(au, 0x80, 36 + i, 0, 300);
    render_frames(&r, 2 * 44100 + 22050, 512);
    CHECK_EQ(r.err, 0);
    CHECK_EQ(r.nonFinite, 0);
    CHECK(rms_lr(&r, 4410, 22050) > 0.02);
    CHECK(rms_lr(&r, 2 * 44100, 3 * 44100) < 0.001);
    run_free(&r);
    au_close(au);
}

/** Open/initialize/render/uninitialize/close 40 times; instances can coexist and re-initialize. */
static void test_repeated_open_close(void)
{
    int i;
    AudioUnit keep;
    Run r;
    REQUIRE_OK(au_open(&keep, 44100, 0));
    for (i = 0; i < 40; i++) {
        AudioUnit au;
        Run q;
        OSStatus e = au_open(&au, i % 2 ? 48000 : 44100, 0);
        if (e) { fail_at(__FILE__, __LINE__, "open #%d -> %d", i, (int)e); return; }
        run_init(&q, au, 0);
        MusicDeviceMIDIEvent(au, 0x90, 60 + i % 12, 100, 0);
        render_frames(&q, 2048, 512);
        if (q.err || q.peak <= 0.001) fail_at(__FILE__, __LINE__, "instance #%d: err %d peak %g", i, (int)q.err, q.peak);
        else g_checksOk++;
        if (i % 5 == 0) {
            /* uninitialize and initialize again */
            CHECK_OK(AudioUnitUninitialize(au));
            CHECK_OK(AudioUnitInitialize(au));
            render_frames(&q, 512, 512);
            CHECK_EQ(q.err, 0);
        }
        CHECK_OK(AudioUnitUninitialize(au));
        CHECK_OK(AudioComponentInstanceDispose(au));
    }
    /* the long-lived instance still works */
    run_init(&r, keep, 0);
    MusicDeviceMIDIEvent(keep, 0x90, 60, 100, 0);
    render_frames(&r, 8192, 512);
    CHECK_EQ(r.err, 0);
    CHECK(r.peak > 0.01);
    au_close(keep);
}

/**
 * Determinism: the same scenario in two fresh processes is bit-identical. Inside one process a
 * second fresh instance differs when oscillator phases are random (phase shift 0), because the
 * engine's random generator is process-wide; with fixed phases it is identical.
 */
static void test_determinism(void)
{
    char args[2048];
    const char* l1;
    char first[256];
    AudioUnit a, b;
    Run ra, rb;
    Event ev[1] = { { 0, 0x90, 60, 100, 0 } };
    snprintf(args, sizeof args, "scenario 0 '%s'", g_statePath);
    l1 = run_child(args);
    REQUIRE(l1 != NULL);
    snprintf(first, sizeof first, "%s", l1);
    l1 = run_child(args);
    REQUIRE(l1 != NULL);
    CHECK_STREQ(first, l1);

    REQUIRE_OK(au_open(&a, 44100, 0));
    run_init(&ra, a, 0);
    render_events(&ra, 22050, kB512, 1, ev, 1);
    au_close(a);
    REQUIRE_OK(au_open(&b, 44100, 0));
    run_init(&rb, b, 0);
    render_events(&rb, 22050, kB512, 1, ev, 1);
    au_close(b);
    CHECK(ra.hash != rb.hash);              /* random phases continue across instances */

    REQUIRE_OK(au_open(&a, 44100, 0));
    fixed_phase_patch(a);
    run_init(&ra, a, 0);
    render_events(&ra, 22050, kB512, 1, ev, 1);
    au_close(a);
    REQUIRE_OK(au_open(&b, 44100, 0));
    fixed_phase_patch(b);
    run_init(&rb, b, 0);
    render_events(&rb, 22050, kB512, 1, ev, 1);
    au_close(b);
    CHECK(ra.hash == rb.hash);
}

/** The fresh-instance state equals default_state.plist (the original release's "initial sound"). */
static void test_default_state(void)
{
    AudioUnit au;
    CFPropertyListRef ref = load_plist(g_statePath), cur;
    char name[256];
    REQUIRE(ref != NULL);
    REQUIRE_OK(au_open(&au, 44100, 0));
    cur = save_state(au);
    au_close(au);
    REQUIRE(cur != NULL);
    cfstr((CFStringRef)CFDictionaryGetValue((CFDictionaryRef)cur, CFSTR("name")), name, sizeof name);
    if (strcmp(name, "initial sound") != 0) {
        /* bank 0 of ~/Documents/Synth1Patches holds a sound: the default is the user's, skip */
        printf("    note: fresh sound is \"%s\", not \"initial sound\" (user bank); comparison skipped\n", name);
    } else {
        CHECK(CFEqual(state_data(ref), state_data(cur)));
    }
    CFRelease(ref);
    CFRelease(cur);
}

/** Golden render hashes (from the original release binary) for the fixed scenarios. */
static void test_golden(void)
{
    int i;
    for (i = 0; i < NUM_SCENARIOS; i++) {
        char args[2048];
        const char* line;
        unsigned long long h;
        uint64_t ref;
        if (!find_ref(kScenarios[i].name, &ref)) {
            fail_at(__FILE__, __LINE__, "no reference hash for %s in %s", kScenarios[i].name, g_referencePath);
            continue;
        }
        snprintf(args, sizeof args, "scenario %d '%s'", i, g_statePath);
        line = run_child(args);
        if (!line || sscanf(line, "hash %llx", &h) != 1) {
            fail_at(__FILE__, __LINE__, "%s: render failed", kScenarios[i].name);
            continue;
        }
        if (h != ref) fail_at(__FILE__, __LINE__, "%s: hash %016llx, reference %016llx", kScenarios[i].name, h, (unsigned long long)ref);
        else g_checksOk++;
    }
}

/* ------------------------------------------------------------------------------------------ */

typedef struct { const char* name; void (*fn)(void); } TestCase;
static const TestCase kTests[] = {
    { "au.host_zero_fill", test_host_zero_fill },
    { "au.bundle_component", test_bundle_component },
    { "au.open_init_rates", test_open_init_rates },
    { "au.stream_format", test_stream_format },
    { "au.parameter_list", test_parameter_list },
    { "au.parameter_info", test_parameter_info },
    { "au.parameter_roundtrip", test_parameter_roundtrip },
    { "au.parameter_affects_sound", test_parameter_affects_sound },
    { "au.note_on_off", test_note_on_off },
    { "au.polyphony_and_velocity0", test_polyphony_and_velocity0 },
    { "au.all_notes_off", test_all_notes_off },
    { "au.reset", test_reset },
    { "au.classinfo_roundtrip", test_classinfo_roundtrip },
    { "au.classinfo_render", test_classinfo_render },
    { "au.classinfo_initial_sound", test_classinfo_initial_sound },
    { "au.classinfo_render_processes", test_classinfo_render_processes },
    { "au.presets", test_presets },
    { "au.cocoa_ui", test_cocoa_ui },
    { "au.max_frames", test_max_frames },
    { "au.odd_block_sizes", test_odd_block_sizes },
    { "au.many_events", test_many_events },
    { "au.repeated_open_close", test_repeated_open_close },
    { "au.determinism", test_determinism },
    { "au.default_state", test_default_state },
    { "au.golden", test_golden },
};

static int load_component(const char* path)
{
    CFURLRef url;
    AudioComponentFactoryFunction fac;
    AudioComponentDescription d = { kAudioUnitType_MusicDevice, 'Syn1', 'daiC', 0, 0 };
    url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, strlen(path), true);
    g_bundle = url ? CFBundleCreate(NULL, url) : NULL;
    if (url) CFRelease(url);
    if (!g_bundle || !CFBundleLoadExecutable(g_bundle)) {
        fprintf(stderr, "cannot load bundle %s\n", path);
        return 1;
    }
    fac = (AudioComponentFactoryFunction)CFBundleGetFunctionPointerForName(g_bundle, CFSTR("Synth1Factory"));
    if (!fac) { fprintf(stderr, "Synth1Factory not found\n"); return 1; }
    g_comp = AudioComponentRegister(&d, CFSTR("Daichi Laboratory: Synth1 (test)"), 0x10000, fac);
    if (!g_comp) { fprintf(stderr, "AudioComponentRegister failed\n"); return 1; }
    g_componentPath = path;
    return 0;
}

/** --gen: writes default_state.plist and the reference hashes from the given component. */
static int gen_main(int argc, char** argv)
{
    char args[2048];
    FILE* f;
    int i;
    if (argc < 5) return 2;
    g_componentPath = argv[2];
    snprintf(args, sizeof args, "save-default '%s'", argv[4]);
    if (!run_child(args)) { fprintf(stderr, "save-default failed\n"); return 1; }
    f = fopen(argv[3], "w");
    if (!f) return 1;
    fprintf(f, "# Golden render hashes for tests/au/au_tests.c (scenario  FNV-1a-64 of the interleaved\n");
    fprintf(f, "# little-endian float32 L/R output  peak).\n");
    fprintf(f, "# Generated from the ORIGINAL release binary (Synth1.component from Synth1macau113beta11.zip)\n");
    fprintf(f, "# on macOS 10.13.6 (x86_64), each scenario in a fresh process after restoring default_state.plist\n");
    fprintf(f, "# (the ClassInfo of a fresh instance of the original, i.e. the \"initial sound\"),\n");
    fprintf(f, "# with no jp.daichi.synth1 preferences (default CC map). The rebuilt component is byte-identical,\n");
    fprintf(f, "# so it must reproduce these exactly.\n");
    for (i = 0; i < NUM_SCENARIOS; i++) {
        const char* line;
        unsigned long long h;
        double peak;
        snprintf(args, sizeof args, "scenario %d '%s'", i, argv[4]);
        line = run_child(args);
        if (!line || sscanf(line, "hash %llx peak %lf", &h, &peak) != 2) { fprintf(stderr, "%s failed\n", kScenarios[i].name); fclose(f); return 1; }
        fprintf(f, "%-16s %016llx  %.6f\n", kScenarios[i].name, h, peak);
        printf("%-16s %016llx  peak %.6f\n", kScenarios[i].name, h, peak);
    }
    fclose(f);
    return 0;
}

int main(int argc, char** argv)
{
    uint32_t sz = sizeof g_selfPath;
    int i, nTests = (int)(sizeof kTests / sizeof kTests[0]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_zero_fill();
    if (_NSGetExecutablePath(g_selfPath, &sz) != 0) return 2;
    if (argc >= 2 && !strcmp(argv[1], "--child")) return child_main(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--gen")) return gen_main(argc, argv);
    if (argc < 4) {
        fprintf(stderr, "usage: %s <Synth1.component> <reference.txt> <default_state.plist> [filter ...]\n", argv[0]);
        return 2;
    }
    if (load_component(argv[1])) { printf("FAIL au.load: cannot load %s\n", argv[1]); return 1; }
    g_referencePath = argv[2];
    g_statePath = argv[3];
    signal(SIGALRM, on_alarm);
    for (i = 0; i < nTests; i++) {
        int j, selected = argc <= 4;
        for (j = 4; j < argc; j++) if (strstr(kTests[i].name, argv[j])) selected = 1;
        if (!selected) continue;
        g_testFailed = 0;
        g_tests++;
        g_currentTest = kTests[i].name;
        alarm(60);
        kTests[i].fn();
        alarm(0);
        if (g_testFailed) g_testsFailed++;
        printf("%s %s\n", g_testFailed ? "FAIL" : "ok  ", kTests[i].name);
    }
    printf("\n%d tests, %d failed; %d checks passed, %d failed\n", g_tests, g_testsFailed, g_checksOk, g_checksFailed);
    return g_testsFailed ? 1 : 0;
}
