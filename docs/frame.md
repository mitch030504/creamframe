# Steam Frame support

Steam Frame runs SteamOS on ARM64. It can run:

- native Linux games built for aarch64,
- Android games through **Lepton** (the game ships as an APK),
- Windows games through Proton/FEX.

This fork covers native ARM64 with the normal `LD_PRELOAD` hooks, Android games
with a `libsteam_api.so` proxy in the APK, and Windows/Proton games with
SmokeAPI. FEX removes `LD_PRELOAD` before launching x86 Linux games, so native
x86 Linux games under FEX are not supported; use Proton + SmokeAPI for those.

## SSH access

Developer Mode exposes SSH, ADB and RDP:

1. On the Frame: **Steam Settings → System → Enable Developer Mode**.
2. Then **Settings → Developer → Set User Password**.
3. From your PC:

```sh
ssh steamos@frame
ssh-copy-id steamos@frame   # optional, for passwordless key auth
```

The default hostname is `frame`; rename it under **Settings → System →
Hostname** if you have more than one. `tools/frame.py` defaults to
`steamos@frame`; override with `--remote` or `CREAM_FRAME_REMOTE`.

## Host requirements

- Python 3.9+ (`tools/frame.py`, `tools/apk_patch.py`)
- `rsync`, `ssh`
- Java (`java`, `keytool`)
- Android SDK build-tools (`zipalign`, `apksigner`); set `ANDROID_HOME` or put
  them on `PATH`
- `zip`, `unzip`
- Android NDK r30 for `./build-android.sh` (defaults to `/opt/android-ndk`,
  override with `ANDROID_NDK_HOME`)

`python3 tools/frame.py doctor` checks all of these.

## Android games

Android games on Steam Frame are APKs. Lepton installs them into a container
and launches them; the game's APK contains `lib/arm64-v8a/libsteam_api.so`.

`tools/frame.py android patch <appid>`:

1. finds the installed APK in any Steam library (including SD cards),
2. fetches the game's DLC list from the Steam Store (or uses `--ini`),
3. downloads the APK and patches it locally:
   - `libsteam_api.so` becomes the creamlinux proxy,
   - the original is shipped as `libsteam_api_orig.so` and the proxy forwards
     every non-hooked Steamworks symbol to it,
   - the per-game `cream_api.ini` is appended to the proxy as a trailer,
4. backs up the original on the Frame as `<apk>.cream-backup`,
5. uploads the patched APK atomically and writes `<apk>.cream.json` with the
   original and patched hashes.

Example:

```sh
python3 tools/frame.py scan
python3 tools/frame.py android patch 1408230
python3 tools/frame.py android status 1408230
python3 tools/frame.py android revert 1408230
```

### Updates

- **Steam updates the game APK normally.** The update replaces the patched APK
  with the new original. Run `android patch` again after an update; `status`
  reports `APK changed since the patch`.
- **Lepton updates normally.** The fork never edits Lepton's files.
- The patched APK is re-signed with a key stored at
  `~/.config/creamframe/android.keystore` (mode 0600). Keep it stable: patched
  versions signed with the same key update each other cleanly.
- The first patch changes the app signature. Lepton may re-bake the container;
  local app data can be reset. Back up saves or rely on Steam Cloud first.
- Steam's **Verify integrity** restores the original APK; that is also a clean
  uninstall of the patch.

### Why not patch Lepton or the installed native directory?

Lepton's baked app directory (`compatdata/<appid>/baked/app_overlay`) is
temporary: it is removed on normal container teardown. Patching the APK is the
only location that survives that lifecycle while still letting Steam own the
game files. The original APK is always backed up.

The proxy is a real Android shared library named `libsteam_api.so`; the original
is renamed to `libsteam_api_orig.so`. It is built with bionic/NDK, statically
links libc++, exports only the creamlinux hooks, and resolves non-hooked symbols
through `DT_NEEDED: libsteam_api_orig.so`. The DLC list is appended as:

```
[cream_api.ini bytes]["CREAMCFG"][uint32 little-endian length]
```

The ELF loader ignores the trailing data. This avoids adding a non-`.so` file to
`lib/arm64-v8a`, which Android does not extract.

## Native ARM64 games

```sh
./build.sh                                # on the Frame, or use the CI artifact
python3 tools/frame.py native install 123456
```

Set the game's launch options to:

```sh
sh ./cream.sh %command%
```

`cream.sh` reads the game's `libsteam_api.so` ELF `e_machine` and picks
`aarch64/lib64Creamlinux.so`, the flat x86-64 library, or the 32-bit library.
On an aarch64 host with an x86-64 game it prints a warning because FEX strips
`LD_PRELOAD`.

## Proton/Windows games

```sh
python3 tools/frame.py proton install 620980 --smokeapi-mode proxy
python3 tools/frame.py proton uninstall 620980
```

`proxy` mode replaces the game's `steam_api64.dll` with SmokeAPI and keeps the
original as `steam_api64_o.dll`. This is the most reliable mode for Unity VR
games such as Beat Saber. The per-game `cream_api.ini` is copied to the game
root. Game updates overwrite the Steam DLL; re-run the command after an update.

## Logs

For Android games:

```sh
python3 tools/frame.py logs 1408230
# or manually:
ssh steamos@frame
/home/steamos/.local/share/Steam/steamapps/common/Lepton/lepton logcat steamlaunch-1408230
```

Look for `creamlinux:` lines. A patched game logs the config load, the Steamworks
init, and `BIsDlcInstalled unlocked <id>` responses.

## Tested

- **Walkabout Mini Golf** (Android, 1408230): 30 DLCs, patched APK, proxy
  loaded, all 30 `BIsDlcInstalled` queries returned unlocked.
- **UNDERDOGS** (Android, 2441700): 4 DLC IDs; the test account could not
  launch the base game (`AppError_5`, license check), so the APK patch was
  reverted after the Android proxy self-test.
- **Beat Saber** (Proton, 620980): SmokeAPI proxy installed into
  `Beat Saber_Data/Plugins/x86_64`; launch was waiting on Steam's EULA dialog
  at the time of writing.

## Troubleshooting

- `doctor` cannot SSH: enable Developer Mode and set a password; try the IP
  address instead of `frame` if mDNS is blocked.
- `android patch` says the APK is already patched but has no backup: run
  Steam's **Verify integrity** first; the tool refuses to overwrite the only
  original.
- Game does not start after patching: `android revert <appid>`, launch once, and
  check the APK/signature state with `android status`.
- No `creamlinux` log lines: confirm the game is the Android/Lepton variant
  (`frame.py scan` prints `android`) and that Lepton re-baked the patched APK.
- DLC still locked: the game may check DLC in another way, or the DLC content
  files may not be present. Cream does not download DLC content.

## Legal / safety

Creamlinux changes client-side entitlement checks. It does not download game
content. Use it only with games you own; DLC checks may be covered by the Steam
Subscriber Agreement and local law. Re-signing an APK changes its signature;
some games or anti-cheat systems may object. Do not use it in competitive or
anti-cheat titles.
