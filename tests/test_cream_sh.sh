#!/usr/bin/env bash
# test_cream_sh.sh - integration test for the cream.sh launch script
# (issues #51, #62, #71, Steam Frame architecture selection):
#   - works from any working directory (#62)
#   - mangohud mode exports MANGOHUD/MANGOHUD_DLSYM (#51)
#   - preloads only the architecture-matching creamlinux lib
#   - selects aarch64/x86-64 builds by the game's ELF e_machine
#   - copies the libs into a space-free private temp dir and cleans up
#
# Usage: bash test_cream_sh.sh <package-dir>
set -u

PACKAGE_DIR="${1:-}"
if [ -z "$PACKAGE_DIR" ] || [ ! -f "$PACKAGE_DIR/cream.sh" ]; then
    echo "usage: $0 <package-dir-with-cream.sh>" >&2
    exit 2
fi

failures=0
WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT

# write_elf <path> <elf-class 1|2> <e_machine little-endian hex, e.g. 3e00>
write_elf() {
    local path="$1" class="$2" machine="$3"
    printf '\177ELF' > "$path"
    printf "\\$(printf '%03o' "$class")\001\001\000\000\000\000\000\000\000\000\000" >> "$path"
    printf '\002\000' >> "$path"
    printf "\\$(printf '%03o' $((16#${machine:0:2})))\\$(printf '%03o' $((16#${machine:2:2})))" >> "$path"
}

# Fake game: a directory WITH SPACES in its name (an LD_PRELOAD hazard).
GAME="$WORK/My Game With Spaces"
mkdir -p "$GAME/binaries" "$GAME/aarch64"
write_elf "$GAME/binaries/libsteam_api.so" 2 3e00   # ELFCLASS64, x86-64
printf '[config]\nunlockall = true\n' > "$GAME/cream_api.ini"
cp "$PACKAGE_DIR/cream.sh" "$GAME/cream.sh"
chmod +x "$GAME/cream.sh"
# Dummy creamlinux libs: cream.sh only copies them, they are never loaded.
write_elf "$GAME/lib64Creamlinux.so" 2 3e00
write_elf "$GAME/lib32Creamlinux.so" 1 0300
write_elf "$GAME/aarch64/lib64Creamlinux.so" 2 b700

# run_game <workdir> <outfile> [extra args...] -> runs cream.sh + a shell
# that dumps $LD_PRELOAD (and $MANGOHUD|$MANGOHUD_DLSYM) into <outfile>
run_game() {
    local wd="$1" out="$2"; shift 2
    rm -f "$WORK/selected_lib"
    ( cd "$wd" && sh ./cream.sh "$@" /bin/sh -c \
        'for l in $LD_PRELOAD; do case "$l" in *Creamlinux.so) cp "$l" "$2";; esac; done; printf "%s\n%s" "$LD_PRELOAD" "$MANGOHUD|$MANGOHUD_DLSYM" > "$1"' \
        _ "$out" "$WORK/selected_lib" ) >/dev/null 2>&1
}

# Fake mangohud wrapper (like /usr/bin/mangohud): cream.sh keeps it as the
# first argument of the launched command, so it must exist in PATH.
FAKEBIN="$WORK/bin"
mkdir -p "$FAKEBIN"
cat > "$FAKEBIN/mangohud" <<'EOF'
#!/bin/sh
exec "$@"
EOF
chmod +x "$FAKEBIN/mangohud"
PATH="$FAKEBIN:$PATH"
export PATH

check() { # check <desc> <command...>
    local desc="$1"; shift
    if "$@"; then
        echo "ok: $desc"
    else
        echo "FAIL: $desc"
        failures=$((failures + 1))
    fi
}

# --- 1. x86-64 game: lib64Creamlinux preloaded, no lib32, no spaces ----
run_game "$GAME" "$WORK/preload64.txt"
check "64-bit: cream.sh exits 0" test -s "$WORK/preload64.txt"
check "64-bit: lib64Creamlinux preloaded" \
    grep -q "lib64Creamlinux.so" "$WORK/preload64.txt"
check "64-bit: lib32Creamlinux NOT preloaded" \
    bash -c '! grep -q "lib32Creamlinux.so" "$1"' _ "$WORK/preload64.txt"
check "64-bit: libsteam_api preloaded" \
    grep -q "libsteam_api.so" "$WORK/preload64.txt"
check "64-bit: preload does not contain the spacey game dir" \
    bash -c '! grep -q "My Game With Spaces" "$1"' _ "$WORK/preload64.txt"
check "64-bit: mangohud vars unset" \
    grep -q "^|$" "$WORK/preload64.txt"

# --- 2. i386 game: lib32Creamlinux preloaded instead -----------------
write_elf "$GAME/binaries/libsteam_api.so" 1 0300   # ELFCLASS32, i386
run_game "$GAME" "$WORK/preload32.txt"
check "32-bit: lib32Creamlinux preloaded" \
    grep -q "lib32Creamlinux.so" "$WORK/preload32.txt"
check "32-bit: lib64Creamlinux NOT preloaded" \
    bash -c '! grep -q "lib64Creamlinux.so" "$1"' _ "$WORK/preload32.txt"

# --- 3. aarch64 game on an x86 host: picks aarch64/lib64Creamlinux -----
write_elf "$GAME/binaries/libsteam_api.so" 2 b700   # ELFCLASS64, aarch64
run_game "$GAME" "$WORK/preload_arm.txt"
check "aarch64: selected cream lib has aarch64 e_machine" \
    test "$(od -An -t x1 -j 18 -N 2 "$WORK/selected_lib" | tr -d ' \n')" = b700

# --- 4. mangohud mode ----------------------------------------------------
write_elf "$GAME/binaries/libsteam_api.so" 2 3e00
run_game "$GAME" "$WORK/mango.txt" mangohud
check "mangohud: MANGOHUD and MANGOHUD_DLSYM set" \
    grep -q "^1|1$" "$WORK/mango.txt"

# --- 5. CWD independence (#62): launch the script by absolute path ----
( cd "$WORK" && sh "$GAME/cream.sh" /bin/sh -c \
    'printf "%s\n" "$LD_PRELOAD" > "$1"' _ "$WORK/preload_cwd.txt" \
) >/dev/null 2>&1
check "cwd: works when launched from another directory" \
    grep -q "lib64Creamlinux.so" "$WORK/preload_cwd.txt"

# --- 6. temp dir cleanup --------------------------------------------------
BEFORE=$(ls -d /tmp/creamlinux.* 2>/dev/null | wc -l)
run_game "$GAME" "$WORK/preload_cl.txt"
AFTER=$(ls -d /tmp/creamlinux.* 2>/dev/null | wc -l)
check "cleanup: no leftover /tmp/creamlinux.* dirs" \
    test "$BEFORE" -eq "$AFTER"

# --- 7. missing libsteam_api.so -> clear error ---------------------------
EMPTY="$WORK/Empty Game"
mkdir -p "$EMPTY"
cp "$PACKAGE_DIR/cream.sh" "$EMPTY/cream.sh"
write_elf "$EMPTY/lib64Creamlinux.so" 2 3e00
write_elf "$EMPTY/lib32Creamlinux.so" 1 0300
( cd "$EMPTY" && sh ./cream.sh /bin/true ) >"$WORK/err.txt" 2>&1
check "missing libsteam_api.so: nonzero exit" test $? -ne 0
check "missing libsteam_api.so: error message" \
    grep -q "libsteam_api.so not found" "$WORK/err.txt"

# --- 8. wrong architecture -> clear error --------------------------------
MISMATCH="$WORK/Mismatch Game"
mkdir -p "$MISMATCH/binaries"
cp "$PACKAGE_DIR/cream.sh" "$MISMATCH/cream.sh"
write_elf "$MISMATCH/binaries/libsteam_api.so" 2 b700   # aarch64 game
write_elf "$MISMATCH/lib64Creamlinux.so" 2 3e00         # x86-64 lib only
printf '[config]\nunlockall = true\n' > "$MISMATCH/cream_api.ini"
( cd "$MISMATCH" && sh ./cream.sh /bin/true ) >"$WORK/mismatch.txt" 2>&1
check "mismatch: nonzero exit" test $? -ne 0
check "mismatch: clear architecture error" \
    grep -q "no creamlinux library matches the game architecture" "$WORK/mismatch.txt"

if [ "$failures" -eq 0 ]; then
    echo "cream.sh OK"
    exit 0
fi
echo "$failures cream.sh check(s) failed" >&2
exit 1
