#!/bin/bash
# Install Usefulity module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/usefulity" ]; then
    echo "Error: dist/usefulity not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Usefulity Module ==="

echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/audio_fx/usefulity"
scp -r dist/usefulity/* ableton@move.local:/data/UserData/move-anything/modules/audio_fx/usefulity/

echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/audio_fx/usefulity"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/audio_fx/usefulity/"
echo ""
echo "Restart Move Anything to load the new module."
