// Link-time stub used only to make the Android proxy record
// DT_NEEDED: libsteam_api_orig.so. At runtime the real renamed original from
// the APK satisfies that dependency; this library is never shipped.
extern "C" void cream_steam_api_orig_link_stub() {}
