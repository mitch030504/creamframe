// Export hooks: dlsym interposition, SteamAPI_Init, SteamInternal_* interface
// creation and the flat API hooks. This file is the "front door" of the .so:
// everything here is either an exported symbol games resolve through
// LD_PRELOAD or a helper for those hooks.
//
// On Android the same source is built as a libsteam_api.so proxy. bionic has
// no symbol versioning and no dlvsym(), so the glibc dlsym interposition is
// compiled out; the proxy loads the renamed original (libsteam_api_orig.so)
// and resolves every non-hooked symbol through it.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "interface_versions.h"
#include "spdlog/spdlog.h"
#include "steam_hooks.h"

#include "ext/steam/isteamapps.h"
#include "ext/steam/isteamclient.h"
#include "ext/steam/isteamclient020.h"
#include "ext/steam/isteamuser.h"
#include "ext/steam/isteamuser021.h"
#include "ext/steam/steam_api_common.h"

#ifdef __ANDROID__
#include <android/log.h>
#include "spdlog/sinks/android_sink.h"
#endif

using namespace std;

#if defined(__GNUC__)
#define CREAM_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define CREAM_EXPORT extern "C"
#endif

// SteamAPI_InitEx's result type lives in steam_api.h, but including that header
// would define an inline SteamAPI_Init() and collide with our exported hook.
enum ESteamAPIInitResult {
    k_ESteamAPIInitResult_OK = 0,
    k_ESteamAPIInitResult_FailedGeneric = 1,
    k_ESteamAPIInitResult_NoSteamClient = 2,
    k_ESteamAPIInitResult_VersionMismatch = 3,
};

static void* (*real_dlsym)(void *handle, const char *name) = nullptr;

#ifdef __ANDROID__
static void* orig_handle = nullptr;
static std::once_flag android_init_once;
#endif

static std::once_flag config_once;

static void ensure_orig_loaded();
static void ensure_realdlsym();

// Resolve a symbol from the real libsteam_api. On native Linux this is
// dlsym(RTLD_NEXT); on Android it is dlsym() on the renamed original, which
// the proxy lists in DT_NEEDED.
static void* cream_resolve(const char* name) {
#ifdef __ANDROID__
    ensure_orig_loaded();
    if (orig_handle == nullptr) {
        return nullptr;
    }
    return dlsym(orig_handle, name);
#else
    ensure_realdlsym();
    if (real_dlsym == nullptr) {
        return nullptr;
    }
    return real_dlsym(RTLD_NEXT, name);
#endif
}

static void ensure_realdlsym() {
    if (real_dlsym != nullptr) {
        return;
    }
#ifdef __ANDROID__
    // bionic does not use symbol versions. This library deliberately does not
    // export its own dlsym(), so RTLD_DEFAULT resolves libdl's implementation.
    *(void **)(&real_dlsym) = dlsym(RTLD_DEFAULT, "dlsym");
#elif defined(__aarch64__)
    // aarch64 glibc introduced dlsym in GLIBC_2.17; newer glibc keeps that
    // version for ABI compatibility. Try the common fallbacks as well.
    *(void **)(&real_dlsym) = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    if (real_dlsym == nullptr)
        *(void **)(&real_dlsym) = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
#elif defined(__arm__)
    *(void **)(&real_dlsym) = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.4");
    if (real_dlsym == nullptr)
        *(void **)(&real_dlsym) = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
#else
    // The x86-64 ABI exports dlsym as GLIBC_2.2.5, but the i386 ABI
    // uses GLIBC_2.0 (and GLIBC_2.34 on glibc >= 2.34). Trying only the
    // 64-bit version string left real_dlsym NULL in 32-bit builds, so
    // ANY dlsym() call in a 32-bit process with creamlinux preloaded
    // (e.g. the 32-bit Paradox launcher 'dowser') crashed with a NULL
    // call - showing up as "Killed"/SIGSEGV (issues #51, #56).
    *(void **)(&real_dlsym) = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    if (real_dlsym == nullptr)
        *(void **)(&real_dlsym) = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
    if (real_dlsym == nullptr)
        *(void **)(&real_dlsym) = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
#endif
}

static void ensure_orig_loaded() {
#ifdef __ANDROID__
    if (orig_handle != nullptr) {
        return;
    }
    orig_handle = dlopen("libsteam_api_orig.so", RTLD_NOW | RTLD_GLOBAL);
    if (orig_handle == nullptr) {
        const std::string path = own_library_dir() + "/libsteam_api_orig.so";
        orig_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    }
    if (orig_handle == nullptr) {
        spdlog::error("creamlinux: cannot load libsteam_api_orig.so: {}",
                      dlerror() != nullptr ? dlerror() : "unknown error");
    }
#else
    ensure_realdlsym();
#endif
}

// Load the config once. Called by every init entry point and lazily by the DLC
// hooks, because modern SDKs call SteamInternal_SteamAPI_Init()/SteamAPI_InitFlat()
// rather than the legacy exported SteamAPI_Init().
static void ensure_config_loaded() {
    if (config_is_loaded()) {
        return;
    }
    std::call_once(config_once, [] {
        if (config_is_loaded()) {
            return;
        }
        const std::string path = default_config_path();
        if (config_file_exists(path)) {
            load_config(path);
            return;
        }
        if (load_config_from_library_trailer()) {
            return;
        }
        spdlog::warn("creamlinux: config file not found at {}", path);
    });
}

#ifdef __ANDROID__
__attribute__((constructor)) static void cream_android_constructor() {
    auto sink = std::make_shared<spdlog::sinks::android_sink_mt>("creamlinux");
    auto logger = std::make_shared<spdlog::logger>("creamlinux", sink);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("%v");
    spdlog::set_level(spdlog::level::info);
    ensure_orig_loaded();
}
#endif

CREAM_EXPORT void* CreateInterface(const char *pName, int *pReturnCode) {
    void *S_CALLTYPE (*real)(const char *pName, int *pReturnCode);
    *(void**)(&real) = cream_resolve("CreateInterface");
    spdlog::info("CreateInterface called pszVersion: {}", pName);
    if (real == nullptr) {
        return nullptr;
    }
    return real(pName, pReturnCode);
}

// ---- Flat API hooks (steam_api_flat.h) ----
// Some games (e.g. Unity titles) call SteamAPI_ISteamApps_* and SteamAPI_ISteamUser_*
// directly instead of going through the C++ interface accessors. Since creamlinux is
// LD_PRELOADed before libsteam_api.so, these symbols interpose the real ones.
// NOTE: the real flat API passes `self` as the first argument; it must stay in the
// signature or 32-bit builds will read the wrong stack slot.

CREAM_EXPORT int SteamAPI_ISteamApps_GetDLCCount(ISteamApps* self) {
    ensure_config_loaded();
    spdlog::info("SteamAPI_ISteamApps_GetDLCCount called (flat API)");
    if (is_unlockall()) {
        return self->GetDLCCount();
    }
    return (int)dlcs.size();
}

CREAM_EXPORT bool SteamAPI_ISteamApps_BIsDlcInstalled(ISteamApps* self, AppId_t appID) {
    ensure_config_loaded();
    spdlog::info("SteamAPI_ISteamApps_BIsDlcInstalled called (flat API) appID {}", appID);
    if (is_unlockall()) {
        spdlog::info("SteamAPI_ISteamApps_BIsDlcInstalled unlockall: unlocked {}", appID);
        return true;
    }
    if (is_dlc_unlocked(appID)) {
        spdlog::info("SteamAPI_ISteamApps_BIsDlcInstalled unlocked {}", appID);
        return true;
    }
    return false;
}

CREAM_EXPORT bool SteamAPI_ISteamApps_BIsSubscribedApp(ISteamApps* self, AppId_t appID) {
    ensure_config_loaded();
    spdlog::info("SteamAPI_ISteamApps_BIsSubscribedApp called (flat API) appID {}", appID);
    if (ini["methods"]["disable_steamapps_issubscribedapp"] == "true") {
        return self->BIsSubscribedApp(appID);
    }
    if (is_unlockall()) {
        spdlog::info("SteamAPI_ISteamApps_BIsSubscribedApp unlockall: unlocked {}", appID);
        return true;
    }
    if (is_dlc_unlocked(appID)) {
        spdlog::info("SteamAPI_ISteamApps_BIsSubscribedApp unlocked {}", appID);
        return true;
    }
    return false;
}

CREAM_EXPORT bool SteamAPI_ISteamApps_BGetDLCDataByIndex(ISteamApps* self, int iDLC, AppId_t* pAppID, bool* pbAvailable, char* pchName, int cchNameBufferSize) {
    ensure_config_loaded();
    spdlog::info("SteamAPI_ISteamApps_BGetDLCDataByIndex called (flat API)");
    if (is_unlockall()) {
        return self->BGetDLCDataByIndex(iDLC, pAppID, pbAvailable, pchName, cchNameBufferSize);
    }
    if ((size_t)iDLC >= dlcs.size()) {
        return false;
    }
    if (pAppID == nullptr || pbAvailable == nullptr || pchName == nullptr || cchNameBufferSize <= 0) {
        return false;
    }

    *pAppID = std::get<0>(dlcs[iDLC]);
    *pbAvailable = true;

    const char* name = std::get<1>(dlcs[iDLC]).c_str();
    size_t slen = std::min((size_t)cchNameBufferSize - 1, std::get<1>(dlcs[iDLC]).size());
    memcpy((void*)pchName, (void*)name, slen);
    *(pchName + slen) = 0x0;

    return true;
}

CREAM_EXPORT EUserHasLicenseForAppResult SteamAPI_ISteamUser_UserHasLicenseForApp(ISteamUser* self, uint64 steamID, AppId_t appID) {
    ensure_config_loaded();
    spdlog::info("SteamAPI_ISteamUser_UserHasLicenseForApp called (flat API) appID {}", appID);
    if (is_unlockall() || is_dlc_unlocked(appID)) {
        spdlog::info("SteamAPI_ISteamUser_UserHasLicenseForApp result: owned");
        return (EUserHasLicenseForAppResult)0;
    }
    spdlog::info("SteamAPI_ISteamUser_UserHasLicenseForApp result: not owned");
    return (EUserHasLicenseForAppResult)2;
}

CREAM_EXPORT void* S_CALLTYPE SteamInternal_FindOrCreateUserInterface(HSteamUser hSteamUser, const char *pszVersion) {
    ensure_config_loaded();
    void* S_CALLTYPE (*real)(HSteamUser hSteamUser, const char *pszVersion);
    *(void**)(&real) = cream_resolve("SteamInternal_FindOrCreateUserInterface");
    spdlog::info("SteamInternal_FindOrCreateUserInterface called pszVersion: {}", pszVersion);
    if (real == nullptr) {
        return nullptr;
    }
    // Steamapps Interface call is hooked here (v008 and v009 share a vtable prefix, one wrapper is enough)
    if (strstr(pszVersion, STEAMAPPS_INTERFACE_VERSION_N008) == pszVersion ||
        strstr(pszVersion, STEAMAPPS_INTERFACE_VERSION_N009) == pszVersion) {
        ISteamApps* val = (ISteamApps*)real(hSteamUser, pszVersion);
        spdlog::info("SteamInternal_FindOrCreateUserInterface hooked ISteamApps {}", pszVersion);
        return Hookey_SteamApps(val);
    }

    // SteamUser v022/v023 (GetAuthTicketForWebApi shifted the vtable)
    if (strstr(pszVersion, STEAMUSER_INTERFACE_VERSION_022) == pszVersion ||
        strstr(pszVersion, STEAMUSER_INTERFACE_VERSION_023) == pszVersion) {
        ISteamUser* val = (ISteamUser*)real(hSteamUser, pszVersion);
        spdlog::info("SteamInternal_FindOrCreateUserInterface ISteamUser hook {}", pszVersion);
        return Hookey_SteamUser23(val);
    }
    // SteamUser v020/v021
    if (strstr(pszVersion, STEAMUSER_INTERFACE_VERSION_020) == pszVersion ||
        strstr(pszVersion, STEAMUSER_INTERFACE_VERSION_021) == pszVersion) {
        ISteamUser021* val = (ISteamUser021*)real(hSteamUser, pszVersion);
        spdlog::info("SteamInternal_FindOrCreateUserInterface ISteamUser(legacy) hook {}", pszVersion);
        return Hookey_SteamUser21(val);
    }
    auto val = real(hSteamUser, pszVersion);
    return val;
}

CREAM_EXPORT void* S_CALLTYPE SteamInternal_CreateInterface(const char *pszVersion) {
    ensure_config_loaded();
    void* S_CALLTYPE (*real)(const char *pszVersion);
    *(void**)(&real) = cream_resolve("SteamInternal_CreateInterface");
    spdlog::info("SteamInternal_CreateInterface called pszVersion: {}", pszVersion);
    if (real == nullptr) {
        return nullptr;
    }

    // Steamapps Interface call is hooked here (v008 and v009 share a vtable prefix, one wrapper is enough)
    if (strstr(pszVersion, STEAMAPPS_INTERFACE_VERSION_N008) == pszVersion ||
        strstr(pszVersion, STEAMAPPS_INTERFACE_VERSION_N009) == pszVersion) {
        ISteamApps* val = (ISteamApps*)real(pszVersion);
        spdlog::info("SteamInternal_CreateInterface hooked ISteamApps {}", pszVersion);
        return Hookey_SteamApps(val);
    }

    // SteamUser v022/v023 (GetAuthTicketForWebApi shifted the vtable)
    if (strstr(pszVersion, STEAMUSER_INTERFACE_VERSION_022) == pszVersion ||
        strstr(pszVersion, STEAMUSER_INTERFACE_VERSION_023) == pszVersion) {
        ISteamUser* val = (ISteamUser*)real(pszVersion);
        spdlog::info("SteamInternal_CreateInterface ISteamUser hook {}", pszVersion);
        return Hookey_SteamUser23(val);
    }

    // SteamUser v020/v021
    if (strstr(pszVersion, STEAMUSER_INTERFACE_VERSION_020) == pszVersion ||
        strstr(pszVersion, STEAMUSER_INTERFACE_VERSION_021) == pszVersion) {
        ISteamUser021* val = (ISteamUser021*)real(pszVersion);
        spdlog::info("SteamInternal_CreateInterface ISteamUser(legacy) hook {}", pszVersion);
        return Hookey_SteamUser21(val);
    }

    // SteamClient v021/v022/v023
    if (strstr(pszVersion, STEAMCLIENT_INTERFACE_VERSION_021) == pszVersion ||
        strstr(pszVersion, STEAMCLIENT_INTERFACE_VERSION_022) == pszVersion ||
        strstr(pszVersion, STEAMCLIENT_INTERFACE_VERSION_023) == pszVersion) {
        ISteamClient* val = (ISteamClient*)real(pszVersion);
        spdlog::info("SteamInternal_CreateInterface ISteamClient hook {}", pszVersion);
        return Hookey_SteamClient23(val);
    }

    // SteamClient v017-v020 (legacy, vtable compatible)
    if (strstr(pszVersion, STEAMCLIENT_INTERFACE_VERSION_017) == pszVersion ||
        strstr(pszVersion, STEAMCLIENT_INTERFACE_VERSION_018) == pszVersion ||
        strstr(pszVersion, STEAMCLIENT_INTERFACE_VERSION_019) == pszVersion ||
        strstr(pszVersion, STEAMCLIENT_INTERFACE_VERSION_020) == pszVersion) {
        ISteamClient020* val = (ISteamClient020*)real(pszVersion);
        spdlog::info("SteamInternal_CreateInterface ISteamClient(legacy) hook {}", pszVersion);
        return Hookey_SteamClient20(val);
    }

    auto val = real(pszVersion);
    return val;
}

// for older games
// NOTE: the v009/v023 headers define inline accessors with these names; we export our
// own symbols instead so old binaries that link SteamApps()/SteamUser() directly get hooked.
// The SteamClient() accessor is opt-in via [methods] hook_steamclient_accessor = true
// because hooking it historically broke PAYDAY 2 (Paradox launcher 'Not owned').
CREAM_EXPORT ISteamClient *S_CALLTYPE SteamClient() {
    spdlog::info("SteamClient() called");
    void* S_CALLTYPE (*real)();
    *(void**)(&real) = cream_resolve("SteamClient");
    if (real == nullptr) {
        return nullptr;
    }
    ISteamClient* val = (ISteamClient*)real();
    if (ini["methods"]["hook_steamclient_accessor"] == "true") {
        spdlog::info("SteamClient() hooked (hook_steamclient_accessor=true)");
        return Hookey_SteamClient23(val);
    }
    return val;
}

// Newer SDKs create interfaces through SteamInternal_ContextInit; hook it so
// games that resolve it via dlsym get a consistent path. The real call is
// delegated unchanged - FindOrCreateUserInterface is already hooked.
CREAM_EXPORT void* S_CALLTYPE SteamInternal_ContextInit(void *pContextInitData) {
    void* S_CALLTYPE (*real)(void *pContextInitData);
    *(void**)(&real) = cream_resolve("SteamInternal_ContextInit");
    spdlog::info("SteamInternal_ContextInit called");
    if (real == nullptr) {
        return nullptr;
    }
    return real(pContextInitData);
}

// for older games
// NOTE: the v009/v023 headers define inline accessors with these names; we export our
// own symbols instead so old binaries that link SteamApps()/SteamUser() directly get hooked.
CREAM_EXPORT ISteamApps *S_CALLTYPE SteamApps() {
    spdlog::info("SteamApps() called");

    //get isteamapps
    void* S_CALLTYPE (*real)();
    *(void**)(&real) = cream_resolve("SteamApps");
    if (real == nullptr) {
        return nullptr;
    }
    ISteamApps* val = (ISteamApps*)real();
    //return val;
    return Hookey_SteamApps(val);
}
// for older games
CREAM_EXPORT ISteamUser *S_CALLTYPE SteamUser() {
    spdlog::info("SteamUser() called");

    //get isteamuser
    void* S_CALLTYPE (*real)();
    *(void**)(&real) = cream_resolve("SteamUser");
    if (real == nullptr) {
        return nullptr;
    }
    ISteamUser* val = (ISteamUser*)real();
    //return val;
    // legacy accessor: games on old SDKs expect the SteamUser020/021 vtable
    return (ISteamUser*)Hookey_SteamUser21((ISteamUser021*)val);
}

// Used by SteamAPI_Init() to list loaded libraries when dlsym resolution fails.
static int printdliter(struct dl_phdr_info *info, size_t size, void *data) {
  printf("%s\n", info->dlpi_name);
  return 0;
}

CREAM_EXPORT bool S_CALLTYPE SteamAPI_Init() {
    ensure_config_loaded();
    bool (*real)();
    *(void**)(&real) = cream_resolve("SteamAPI_Init");
    if (real == nullptr) {
        spdlog::error("SteamAPI_Init: real symbol not found");
        return false;
    }
    spdlog::info("SteamAPI_Init called in PID {0}", getpid());

    spdlog::info("Calling real SteamAPI_Init");
    auto retval = real();
    spdlog::info("SteamAPI_Init returned {0}", retval);

    return retval;
}

// Modern SDKs: SteamAPI_Init()/SteamAPI_InitEx() are inline in steam_api.h and
// call this exported symbol. Older SDK builds call the exported SteamAPI_Init()
// above. Hooking both covers old and new games on native Linux and Android.
CREAM_EXPORT ESteamAPIInitResult S_CALLTYPE SteamInternal_SteamAPI_Init(const char *pszInternalCheckInterfaceVersions, SteamErrMsg *pOutErrMsg) {
    ensure_config_loaded();
    ESteamAPIInitResult S_CALLTYPE (*real)(const char *, SteamErrMsg *);
    *(void**)(&real) = cream_resolve("SteamInternal_SteamAPI_Init");
    if (real == nullptr) {
        spdlog::error("SteamInternal_SteamAPI_Init: real symbol not found");
        return k_ESteamAPIInitResult_FailedGeneric;
    }
    spdlog::info("SteamInternal_SteamAPI_Init called");
    auto result = real(pszInternalCheckInterfaceVersions, pOutErrMsg);
    spdlog::info("SteamInternal_SteamAPI_Init returned {}", (int)result);
    return result;
}

CREAM_EXPORT ESteamAPIInitResult S_CALLTYPE SteamAPI_InitFlat(SteamErrMsg *pOutErrMsg) {
    ensure_config_loaded();
    ESteamAPIInitResult S_CALLTYPE (*real)(SteamErrMsg *);
    *(void**)(&real) = cream_resolve("SteamAPI_InitFlat");
    if (real == nullptr) {
        spdlog::error("SteamAPI_InitFlat: real symbol not found");
        return k_ESteamAPIInitResult_FailedGeneric;
    }
    spdlog::info("SteamAPI_InitFlat called");
    auto result = real(pOutErrMsg);
    spdlog::info("SteamAPI_InitFlat returned {}", (int)result);
    return result;
}

CREAM_EXPORT bool S_CALLTYPE SteamAPI_InitSafe() {
    ensure_config_loaded();
    bool S_CALLTYPE (*real)();
    *(void**)(&real) = cream_resolve("SteamAPI_InitSafe");
    if (real == nullptr) {
        spdlog::error("SteamAPI_InitSafe: real symbol not found");
        return false;
    }
    spdlog::info("SteamAPI_InitSafe called");
    auto result = real();
    spdlog::info("SteamAPI_InitSafe returned {}", result);
    return result;
}

#ifndef __ANDROID__
//TODO: add a flag to allow users to disable the dlsym hooking method
CREAM_EXPORT void *dlsym(void *handle, const char *name)
{
    // filelog->info("modified dlsym called with {0}", name); // enable dlsym logging for quirky use cases (Proton, mono apps)

    ensure_realdlsym();

    /* my target binary is even asking for dlsym() via dlsym()... */
    if (!strcmp(name,"dlsym"))
        return (void*)dlsym;

    if (!strcmp(name, "SteamAPI_Init") && handle != RTLD_NEXT) {
        spdlog::info("returning custom impl for {0}", name);
        spdlog::info("custom: {0}, real: {1}", (void *)SteamAPI_Init, real_dlsym(RTLD_NEXT, "SteamAPI_Init"));
        return (void *)SteamAPI_Init;
    }
    if (!strcmp(name, "SteamAPI_InitFlat") && handle != RTLD_NEXT) {
        return (void *)SteamAPI_InitFlat;
    }
    if (!strcmp(name, "SteamAPI_InitSafe") && handle != RTLD_NEXT) {
        return (void *)SteamAPI_InitSafe;
    }
    if (!strcmp(name, "SteamInternal_SteamAPI_Init") && handle != RTLD_NEXT) {
        return (void *)SteamInternal_SteamAPI_Init;
    }
    if (!strcmp(name, "SteamInternal_CreateInterface") && handle != RTLD_NEXT) {
        spdlog::info("returning custom impl for {0}", name);
        spdlog::info("custom: {0}, real: {1}", (void *)SteamInternal_CreateInterface, real_dlsym(RTLD_NEXT, "SteamInternal_CreateInterface"));
        return (void *)SteamInternal_CreateInterface;
    }
    if (!strcmp(name, "SteamInternal_ContextInit") && handle != RTLD_NEXT) {
        spdlog::info("returning custom impl for {0}", name);
        return (void *)SteamInternal_ContextInit;
    }
    // enabling this makes some games (golf with your friends) start calling non-existing functions
    //TODO: add a setting to let users use this as a hooking method
    // if (!strcmp(name, "CreateInterface") && handle != RTLD_NEXT) {
    //     filelog->info("returning custom impl for {0}", name);
    //     filelog->info("custom: {0}, real: {1}", (void *)CreateInterface, real_dlsym(RTLD_NEXT, "CreateInterface"));
    //     return (void *)CreateInterface;
    // }

    // Flat API interposition (issue #60): Mono-based Steamworks.NET games
    // (e.g. Battletech) resolve the flat API via dlsym(handle) on the
    // already-loaded libsteam_api.so, which bypasses LD_PRELOAD symbol
    // interposition entirely. Hand out our DLC-related flat hooks for those
    // names so the unlock logic applies there too. RTLD_NEXT stays real.
    if (handle != RTLD_NEXT) {
        if (!strcmp(name, "SteamAPI_ISteamApps_GetDLCCount"))
            return (void *)SteamAPI_ISteamApps_GetDLCCount;
        if (!strcmp(name, "SteamAPI_ISteamApps_BIsDlcInstalled"))
            return (void *)SteamAPI_ISteamApps_BIsDlcInstalled;
        if (!strcmp(name, "SteamAPI_ISteamApps_BIsSubscribedApp"))
            return (void *)SteamAPI_ISteamApps_BIsSubscribedApp;
        if (!strcmp(name, "SteamAPI_ISteamApps_BGetDLCDataByIndex"))
            return (void *)SteamAPI_ISteamApps_BGetDLCDataByIndex;
        if (!strcmp(name, "SteamAPI_ISteamUser_UserHasLicenseForApp"))
            return (void *)SteamAPI_ISteamUser_UserHasLicenseForApp;
    }

    return real_dlsym(handle,name);
}
#endif
