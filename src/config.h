#pragma once

#include <string>
#include <tuple>
#include <vector>

#include "ext/ini.h"
#include "ext/steam/steamtypes.h"

// DLC list loaded from cream_api.ini ([dlc] section: "<appid> = <name>").
extern std::vector<std::tuple<int, std::string>> dlcs;
// Parsed cream_api.ini structure (also holds [methods]/[config] toggles).
extern mINI::INIStructure ini;

// Returns true if the given appID is present in the cream_api.ini [dlc] list.
bool is_dlc_unlocked(AppId_t appID);

// Returns true when [config] unlockall is enabled: every DLC the game knows
// about is treated as installed, no ID list required.
bool is_unlockall();

// Reads the ini file and fills the global dlcs vector.
void load_config(const std::string& creaminipath);

// Parses an in-memory ini (used by the Android proxy, where the config lives
// next to libsteam_api.so or is embedded in the library).
void load_config_string(const std::string& text,
                        const std::string& source = "embedded");

// Resolves a user-supplied CREAM_CONFIG_PATH (file or directory) to the ini
// file path: appends /cream_api.ini when the path is a directory.
std::string resolve_config_path(const std::string& path);

// The config path used when the game does not set CREAM_CONFIG_PATH.
// Native Linux: ./cream_api.ini (upstream behaviour).
// Android: cream_api.ini next to the loaded library, because an app's working
// directory is "/" and is not writable.
std::string default_config_path();

// True if path exists and is a regular file.
bool config_file_exists(const std::string& path);

// True after a config (including an empty one) has been loaded. Used to avoid
// reloading the ini from a different working directory after a test or game
// has already supplied one.
bool config_is_loaded();

// Directory containing the currently loaded creamlinux library.
std::string own_library_dir();

// Full path of the currently loaded creamlinux library.
std::string own_library_path();

// Reads a config appended to the end of the proxy .so as
//   [ini bytes]["CREAMCFG"][uint32 little-endian length]
// This is how the APK patcher gives each game its DLC list without adding a
// non-.so file to lib/arm64-v8a (Android only extracts native libraries from
// an APK).
bool load_config_from_library_trailer();
