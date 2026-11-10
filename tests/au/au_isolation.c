/**
 * @file au_isolation.c
 * @brief Keeps the Audio Unit tests away from the user's Synth1 settings and files.
 *
 * The AU test scripts load this library into the test host: inserted with DYLD_INSERT_LIBRARIES,
 * or, from macOS 12 on, linked in. Its interpose table replaces these functions everywhere in the
 * process, including in the Synth1 component loaded later:
 * - CFPreferencesCopyAppValue / SetAppValue / AppSynchronize for the "jp.daichi.synth1" domain
 *   use an in-memory store that starts empty, so the Audio Unit sees the default settings;
 * - FSFindFolder for the user's Documents and temporary folders returns folders inside
 *   $SYNTH1_TEST_HOME, so Synth1Patches is created there.
 * Every other call goes to the real function. Child processes get their own empty store and
 * share the folders, because they inherit SYNTH1_TEST_HOME. An inserted library only reaches a
 * child that is started directly: macOS drops DYLD_INSERT_LIBRARIES when it starts /bin/sh.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char* g_home;
static CFMutableDictionaryRef g_prefs;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((constructor)) static void init(void)
{
    g_home = getenv("SYNTH1_TEST_HOME");
    if (!g_home || !*g_home) {
        fprintf(stderr, "au_isolation: set SYNTH1_TEST_HOME to a temporary folder\n");
        abort();
    }
    g_prefs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static int is_synth1_domain(CFStringRef appID)
{
    return appID && appID != kCFPreferencesCurrentApplication &&
           CFGetTypeID(appID) == CFStringGetTypeID() &&
           CFStringHasPrefix(appID, CFSTR("jp.daichi.synth1"));
}

static CFPropertyListRef fake_CFPreferencesCopyAppValue(CFStringRef key, CFStringRef appID)
{
    CFPropertyListRef value;
    if (!is_synth1_domain(appID)) return CFPreferencesCopyAppValue(key, appID);
    pthread_mutex_lock(&g_lock);
    value = CFDictionaryGetValue(g_prefs, key);
    if (value) CFRetain(value);
    pthread_mutex_unlock(&g_lock);
    return value;
}

static void fake_CFPreferencesSetAppValue(CFStringRef key, CFPropertyListRef value, CFStringRef appID)
{
    if (!is_synth1_domain(appID)) {
        CFPreferencesSetAppValue(key, value, appID);
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (value) CFDictionarySetValue(g_prefs, key, value);
    else CFDictionaryRemoveValue(g_prefs, key);
    pthread_mutex_unlock(&g_lock);
}

static Boolean fake_CFPreferencesAppSynchronize(CFStringRef appID)
{
    if (!is_synth1_domain(appID)) return CFPreferencesAppSynchronize(appID);
    return true;
}

static OSErr fake_FSFindFolder(FSVolumeRefNum domain, OSType type, Boolean create, FSRef* ref)
{
    const char* name;
    char path[2048];
    if (domain == kUserDomain && type == kDocumentsFolderType) name = "Documents";
    else if (domain == kUserDomain && type == kTemporaryFolderType) name = "tmp";
    else return FSFindFolder(domain, type, create, ref);

    snprintf(path, sizeof path, "%s/%s", g_home, name);
    if (create) mkdir(path, 0755);
    return FSPathMakeRef((const UInt8*)path, ref, NULL) == noErr ? noErr : fnfErr;
}

#define INTERPOSE(replacement, original) \
    { (const void*)(unsigned long)&replacement, (const void*)(unsigned long)&original }

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void* replacement; const void* original; } kInterpose[] = {
    INTERPOSE(fake_CFPreferencesCopyAppValue, CFPreferencesCopyAppValue),
    INTERPOSE(fake_CFPreferencesSetAppValue, CFPreferencesSetAppValue),
    INTERPOSE(fake_CFPreferencesAppSynchronize, CFPreferencesAppSynchronize),
    INTERPOSE(fake_FSFindFolder, FSFindFolder),
};
