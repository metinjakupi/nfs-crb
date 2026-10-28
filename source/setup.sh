#!/bin/sh
# Setup kit: builds the native Mac app from YOUR OWN copy of Need for Speed: Carbon (PC, version 1.4).
# Usage: ./setup.sh "/path/to/Need for Speed - Carbon" [options]   (./setup.sh --help for the options)
set -eu
cd "$(dirname "$0")"
if [ "$(uname -m)" != arm64 ]; then echo "This port needs an Apple Silicon Mac."; exit 1; fi
if ! xcode-select -p >/dev/null 2>&1; then
    echo "The Xcode Command Line Tools are needed. Run:  xcode-select --install   then run this setup again."; exit 1
fi
# A Python 3.9+ that runs natively on Apple Silicon (the Command Line Tools include one). An Intel-only Python
# (for example an old Homebrew in /usr/local) runs under Rosetta, and everything it starts would too.
PYTHON=""
for p in python3.13 python3.12 python3.11 /usr/bin/python3 python3; do
    if command -v "$p" >/dev/null 2>&1 && \
            "$p" -c 'import platform, sys; sys.exit(sys.version_info < (3, 9) or platform.machine() != "arm64")' 2>/dev/null; then
        PYTHON=$p; break
    fi
done
if [ -z "$PYTHON" ]; then echo "No Apple Silicon Python 3.9 or later found (the Command Line Tools include one)."; exit 1; fi
if [ ! -f kit/tools/build.py ]; then echo "The kit folder is missing: clone the whole repository again." >&2; exit 1; fi
# Checking the game folder needs nothing from the internet: no environment, no downloads.
for a in "$@"; do
    if [ "$a" = "--check-only" ]; then
        if ! "$PYTHON" -c 'import tomllib' 2>/dev/null && [ -x .venv/bin/python ]; then PYTHON=.venv/bin/python; fi
        if "$PYTHON" -c 'import tomllib' 2>/dev/null || "$PYTHON" -c 'import tomli' 2>/dev/null; then
            exec "$PYTHON" tools/setup_kit/setup.py "$@"
        fi
    fi
done
# An environment made by an earlier run with an Intel Python is made again with this one.
if [ -x .venv/bin/python ] && ! .venv/bin/python -c 'import platform, sys; sys.exit(platform.machine() != "arm64")' 2>/dev/null; then
    echo "Making the Python environment again for Apple Silicon (an earlier run used an Intel Python)."; rm -rf .venv
fi
if [ ! -x .venv/bin/python ]; then "$PYTHON" -m venv .venv; fi
if ! .venv/bin/python -m pip install --quiet --disable-pip-version-check --retries 8 --timeout 30 \
        -r kit/requirements-dev.txt tomli; then
    echo
    echo "SETUP STOPPED: the setup could not download its build tools from the Python package index (pypi.org)."
    echo "Check the internet connection. A VPN, proxy or firewall app (for example Little Snitch or LuLu) may be"
    echo "blocking Python or the Setup app: allow them to connect, or pause it, and run the setup again."
    exit 1
fi
exec .venv/bin/python tools/setup_kit/setup.py "$@"
