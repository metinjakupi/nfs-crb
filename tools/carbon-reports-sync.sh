#!/bin/bash
# carbon-reports-sync.sh - copies the players' problem reports that are not here yet from the relay host
# (sportsbook-server, /var/lib/carbon-relay-stage/reports; the droplet's folder has been empty since 2026-11-04)
# into logs/reports-all/. READ-ONLY there: it lists and copies, it never changes or deletes a report.
# logs/ is not in git: the reports hold player names.
#
#   bash tools/carbon-reports-sync.sh            only what is newer than the newest report here
#   SINCE=20261101 bash tools/carbon-reports-sync.sh   first run: everything from that date (default 20261104)
#
# Needs: the ssh key ~/.ssh/usersapi_vps_ed25519 and passwordless sudo on the host (the folder is private to the service).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(cd "$HERE/.." && pwd)/logs/reports-all"
KEY="${CARBON_REPORTS_KEY:-$HOME/.ssh/usersapi_vps_ed25519}"
HOST="${CARBON_REPORTS_HOST:-sportsbook-server}"
DIR=/var/lib/carbon-relay-stage/reports
mkdir -p "$OUT"
LAST=$(ls "$OUT" 2>/dev/null | grep -E '^[0-9]{8}-[0-9]{6}-' | sort | tail -1 || true)
if [ -z "$LAST" ]; then LAST="${SINCE:-20261104}"; fi
echo "newest here: $LAST"
BEFORE=$(ls "$OUT" | wc -l | tr -d ' ')
# names sort by time; ">" keeps the newest one out of the copy only if it is the same name
ssh -i "$KEY" -o IdentitiesOnly=yes -o ConnectTimeout=20 "$HOST" \
  "sudo sh -c 'cd $DIR && ls | awk -v n=\"$LAST\" \"\\\$0 > n\" | tar -T - -cf -'" | tar -xf - -C "$OUT"
AFTER=$(ls "$OUT" | wc -l | tr -d ' ')
echo "$((AFTER - BEFORE)) new report(s); $AFTER in $OUT"
