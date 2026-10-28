#!/bin/sh
# Reproduce a brand-new player's first launch and capture what the game is doing.
#   tools/dev/first_launch_capture.sh [seconds before the capture, default 45]
# A fresh temporary profile (no saves, no shader warm-up list) and the app's Metal shader cache moved
# aside, so shaders compile cold as on a first launch. After the wait it records 5 s of every thread's
# call stacks with macOS's `sample`. Quit the game (Cmd+Q) when you are done; the shader cache is then
# put back and your real profile is never touched. Results: ~/nfsc-mac/first-launch/
set -eu
APP="/Applications/Need for Speed Carbon.app"
WAIT="${1:-45}"
OUT="$HOME/nfsc-mac/first-launch"
CACHE="$(getconf DARWIN_USER_CACHE_DIR)io.github.nfsc-native-mac"
rm -rf "$OUT"; mkdir -p "$OUT"
PROFILE="$(mktemp -d)/NFS Carbon"
[ -d "$CACHE" ] && mv "$CACHE" "$CACHE.aside"
restore() { rm -rf "$CACHE"; [ -d "$CACHE.aside" ] && mv "$CACHE.aside" "$CACHE"; true; }
trap restore EXIT
echo "Starting the game with a fresh profile and a cold shader cache."
RECOMP_PROFILE_DIR="$PROFILE" "$APP/Contents/MacOS/Launch" &
sleep "$WAIT"
PID="$(pgrep -n CarbonRecomp || true)"
if [ -n "$PID" ]; then
    echo "Capturing what the game is doing (5 s)..."
    sample "$PID" 5 -file "$OUT/sample.txt" >/dev/null 2>&1 || echo "sample failed"
    echo "Captured. Note whether the screen was black, then quit the game (Cmd+Q)."
else
    echo "The game is not running any more (crashed or quit early)."
fi
wait || true
cp "$PROFILE/Logs/latest.log" "$OUT/game.log" 2>/dev/null || cp "$PROFILE"/Logs/2*.log "$OUT/" 2>/dev/null || true
echo "Done. Files in $OUT"
