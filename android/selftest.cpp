// Android smoke test for the creamlinux libsteam_api.so proxy.
//
// It loads the proxy on a real Android/bionic runtime, which exercises the
// DT_NEEDED forwarding to libsteam_api_orig.so, the proxy constructor, the
// embedded/next-to-the-.so config reader and the flat DLC hook. It does not
// need a running Steam client because list-based DLC answers do not touch the
// real ISteamApps object.
//
// Build: android/build-selftest.sh
// Run inside a Lepton container with libsteam_api.so, libsteam_api_orig.so
// and cream_api.ini in the same directory.

#include <cstdio>
#include <dlfcn.h>

int main(int argc, char** argv) {
    const char* name = argc > 1 ? argv[1] : "libsteam_api.so";
    void* handle = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        std::printf("FAIL: dlopen(%s): %s\n", name, dlerror());
        return 1;
    }

    using CountFn = int (*)(void*);
    auto count = reinterpret_cast<CountFn>(
        dlsym(handle, "SteamAPI_ISteamApps_GetDLCCount"));
    if (count == nullptr) {
        std::printf("FAIL: proxy does not export GetDLCCount\n");
        return 1;
    }

    int dlc_count = count(nullptr);
    std::printf("proxy loaded, GetDLCCount=%d\n", dlc_count);
    if (dlc_count <= 0) {
        std::printf("FAIL: expected a positive DLC count from cream_api.ini\n");
        return 2;
    }
    return 0;
}
