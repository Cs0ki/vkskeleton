#!/usr/bin/env bash
# Play Sven Co-op with the experimental Vulkan renderer (build/vkskeleton.so).
#
#   ./play_vulkan.sh [extra game args...]        e.g. ./play_vulkan.sh +map hl_t00
#
# Environment:
#   SVEN_DIR                 game folder, if not in a standard Steam location
#   VKSKELETON_PRESENT_MODE  mailbox | immediate | fifo  (default: best available)
#   VKSKELETON_DEBUG=1       extra one-shot logging
#
# While the game runs, svencoop.sh is hooked to preload the renderer, and the
# Options -> Video "Renderer" entry is relabelled "Vulkan".  Both are undone
# when the game exits (or on Ctrl-C); a hard-killed run is repaired on the
# next start.
#
# WARNING: the renderer hooks OpenGL with LD_PRELOAD.  Joining VAC-secured
# servers with it loaded can get the account VAC-banned, and such bans cannot
# be appealed.  Use it offline / on servers you trust, at your own risk.
set -uo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
SRC="$REPO/build/vkskeleton.so"
TMP="${TMPDIR:-/tmp}"
LIB="$TMP/vkskeleton-play.so"
STDOUT_LOG="$TMP/vk-play-stdout.log"
VK_LOG="$TMP/vkplay.log"

find_game_dir() {
    local d
    for d in "${SVEN_DIR:-}" \
             "$HOME/.steam/steam/steamapps/common/Sven Co-op" \
             "$HOME/.steam/debian-installation/steamapps/common/Sven Co-op" \
             "$HOME/.local/share/Steam/steamapps/common/Sven Co-op"; do
        if [[ -n "$d" && -x "$d/svencoop_linux" && -f "$d/svencoop.sh" ]]; then
            (cd "$d" && pwd)
            return 0
        fi
    done
    return 1
}

GAMEDIR="$(find_game_dir)" || {
    echo "Sven Co-op not found; set SVEN_DIR to the game folder" >&2
    exit 1
}
LAUNCHER="$GAMEDIR/svencoop.sh"
ORIG="$GAMEDIR/svencoop.sh.vkskeleton-orig"

[[ -f "$SRC" ]] || { echo "missing $SRC -- run ./build.sh first" >&2; exit 1; }
if pgrep -x svencoop_linux >/dev/null; then
    echo "Sven Co-op is already running; quit it first" >&2
    exit 1
fi

# ---- launcher hook: keep a pristine copy, restore it afterwards ----------
if grep -q VKSKELETON_ENABLE "$LAUNCHER"; then
    if [[ -f "$ORIG" ]]; then           # left over from a hard-killed run
        cp "$ORIG" "$LAUNCHER"
    else
        echo "svencoop.sh is hooked but $ORIG is missing;" \
             "verify the game files in Steam" >&2
        exit 1
    fi
fi
cp "$LAUNCHER" "$ORIG"

# ---- "Vulkan" in Options -> Video -> Renderer ------------------------------
# A copy of the stock gameui_english.txt with only #GameUI_OpenGL changed goes
# into svencoop_addon/resource/ (which overrides svencoop/).  A file the user
# put there is never touched; ours are listed in a sidecar marker.
UI_SRC="$GAMEDIR/svencoop/resource/gameui_english.txt"
UI_DIR="$GAMEDIR/svencoop_addon/resource"
UI_DST="$UI_DIR/gameui_english.txt"
UI_MARK="$UI_DIR/.vkskeleton-temporary"
remove_our_overrides() {
    local f
    [[ -f "$UI_MARK" ]] || return 0
    while IFS= read -r f; do
        [[ -n "$f" && -f "$UI_DIR/$f" ]] && rm -f "$UI_DIR/$f"
    done < "$UI_MARK"
    rm -f "$UI_MARK"
    rmdir --ignore-fail-on-non-empty "$UI_DIR" 2>/dev/null
}
remove_our_overrides
if [[ -f "$UI_SRC" && ! -e "$UI_DST" ]]; then
    mkdir -p "$UI_DIR"
    if python3 -I "$REPO/tools/make_vk_gameui.py" "$UI_SRC" "$UI_DST"; then
        echo "gameui_english.txt" > "$UI_MARK"
    fi
fi

restore() {
    if [[ -f "$ORIG" ]]; then
        cp "$ORIG" "$LAUNCHER" && rm -f "$ORIG"
    fi
    remove_our_overrides
}
trap restore EXIT
trap 'pkill -TERM -x svencoop_linux; exit 130' INT TERM

# ---- hook svencoop.sh: preload the renderer --------------------------------
# Environment variables given to `steam -applaunch` do not reliably reach the
# game, so they are exported from the game's own launcher script instead.
cp "$SRC" "$LIB"
python3 -I - "$LAUNCHER" "$LIB" "$STDOUT_LOG" "$VK_LOG" <<'PY'
import os
import re
import sys
from pathlib import Path

launcher, lib, out, log = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
text = launcher.read_text()
hook = ("export VKSKELETON_ENABLE=1\nexport VKSKELETON_GEOMETRY=1\n"
        f"export VKSKELETON_LOG={log}\nexport VKSKELETON_LOG_PID=1\n"
        f"export LD_PRELOAD={lib}\n\n")
mode = os.environ.get("VKSKELETON_PRESENT_MODE", "")
if re.fullmatch(r"mailbox|immediate|fifo", mode):
    hook = f"export VKSKELETON_PRESENT_MODE={mode}\n" + hook
if os.environ.get("VKSKELETON_DEBUG", "") == "1":
    hook = "export VKSKELETON_DEBUG=1\n" + hook
anchor = "STATUS=42\nwhile"
launch = '${GAMEROOT}/${GAMEEXE}" ${GAMEPARAMS}\n'
if anchor not in text or launch not in text:
    sys.exit("svencoop.sh layout not recognised; not hooking it")
text = text.replace(anchor, hook + anchor, 1)
text = text.replace(launch, launch[:-1] + f" > {out} 2>&1\n", 1)
launcher.write_text(text)
PY
[[ $? -eq 0 ]] || exit 1

rm -f "$VK_LOG".* "$STDOUT_LOG"
steam -applaunch 225840 -nosteamoverlay "$@" >/dev/null 2>&1 &

echo "waiting for the game to start..."
for _ in $(seq 1 1200); do
    pgrep -x svencoop_linux >/dev/null && break
    sleep 0.1
done
pgrep -x svencoop_linux >/dev/null || { echo "game did not start" >&2; exit 1; }
echo "game running (Vulkan log: $VK_LOG.<pid>); quit the game to finish"

while pgrep -x svencoop_linux >/dev/null; do
    sleep 1
done

restore
trap - EXIT
echo "game exited; launcher restored"
