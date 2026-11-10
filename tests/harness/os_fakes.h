/**
 * @file os_fakes.h
 * @brief Test doubles for the operating-system services the engine uses for settings and folders.
 *
 * In the test build only, scripts/legacy/run_unit_tests.sh compiles the engine files that call these services
 * with -D redirects (e.g. -DCFPreferencesSetAppValue=t1_CFPreferencesSetAppValue), so the calls
 * land in os_fakes.cpp instead of the real system:
 * - CFPreferencesCopyAppValue / CFPreferencesSetAppValue / CFPreferencesAppSynchronize use an
 *   in-memory preference store, so the user's real Synth1 settings are never read or written;
 * - FSFindFolder resolves folders (Documents, the temporary folder, ...) inside a fake home folder
 *   created under /tmp for the test run and removed at exit.
 * The release build is not affected: it never sees these flags.
 */
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <string>

namespace t1fake {

/**
 * @brief Clears the in-memory preference store and the call counters.
 */
void resetPrefs();

/**
 * @brief Stores a string preference as if the user had set it.
 *
 * @param appID  Preference domain, e.g. "jp.daichi.synth1".
 * @param key    Preference key.
 * @param value  String value; NULL removes the key.
 */
void setPrefString(const char* appID, const char* key, const char* value);

/**
 * @brief Reads a stored preference as a string.
 *
 * @param appID  Preference domain.
 * @param key    Preference key.
 * @return The value converted to a string (numbers are formatted), or "" if the key is not set.
 */
std::string getPrefString(const char* appID, const char* key);

/**
 * @brief Tells whether a preference key is set.
 *
 * @param appID  Preference domain.
 * @param key    Preference key.
 * @return true if the key has a value.
 */
bool hasPref(const char* appID, const char* key);

/**
 * @brief Number of CFPreferencesSetAppValue calls since the last resetPrefs().
 */
extern int setCalls;

/**
 * @brief Number of CFPreferencesAppSynchronize calls since the last resetPrefs().
 */
extern int syncCalls;

/**
 * @brief Gets the fake home folder (created on first use under /tmp, removed at exit).
 *
 * @return Absolute path without a trailing slash.
 */
std::string home();

/**
 * @brief Gets the folder FSFindFolder returns for a folder type, without creating it.
 *
 * @param folderType  Folder type constant, e.g. kDocumentsFolderType.
 * @return Absolute path inside home().
 */
std::string folderPath(OSType folderType);

/**
 * @brief Removes everything inside the fake home folder (the folder itself stays).
 */
void resetHome();

}  // namespace t1fake
