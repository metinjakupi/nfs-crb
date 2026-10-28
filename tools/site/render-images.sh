#!/bin/bash
# render-images.sh - draws the site's PNG images from the SVGs in tools/site/art/: og.png (1200x630,
# the link preview), apple-touch-icon.png (180x180) and favicon-32.png. favicon.svg is served as it
# is. Uses this Mac's Chrome, headless, with a throwaway profile (the SVGs' fonts are macOS ones).
# Run it again after changing an SVG; tools/carbon-site-deploy.sh uploads the PNGs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

shot() { # svg width height background out
  printf '<!doctype html><style>html,body{margin:0;background:%s}img{display:block;width:%spx;height:%spx}</style><img src="file://%s">' \
    "$4" "$2" "$3" "$1" > "$TMP/page.html"
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --user-data-dir="$TMP/profile" \
    --force-device-scale-factor=1 --default-background-color=00000000 --window-size="$2,$3" \
    --screenshot="$5" "file://$TMP/page.html" >/dev/null 2>&1
  local w h
  w=$(sips -g pixelWidth "$5" | awk '/pixelWidth/ {print $2}'); h=$(sips -g pixelHeight "$5" | awk '/pixelHeight/ {print $2}')
  [ "$w" = "$2" ] && [ "$h" = "$3" ] || { echo "$5 came out ${w}x${h}, not $2x$3" >&2; exit 1; }
}

shot "$HERE/art/og.svg" 1200 630 "#070c16" "$HERE/og.png"
shot "$HERE/art/favicon.svg" 180 180 "#070c16" "$HERE/apple-touch-icon.png"
shot "$HERE/art/favicon.svg" 256 256 transparent "$TMP/icon.png"
sips -z 32 32 "$TMP/icon.png" --out "$HERE/favicon-32.png" >/dev/null
ls -l "$HERE/og.png" "$HERE/apple-touch-icon.png" "$HERE/favicon-32.png"
