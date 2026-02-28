#!/usr/bin/env bash
# Build Usefulity module for Move Anything (ARM64)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Usefulity Module Build (via Docker) ==="
    echo ""
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    echo ""
    echo "=== Done ==="
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Usefulity Module ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build
mkdir -p dist/usefulity

echo "Compiling DSP plugin..."
${CROSS_PREFIX}gcc -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/usefulity.c \
    -o build/usefulity.so \
    -Isrc/dsp \
    -lm

echo "Packaging..."
cat src/module.json > dist/usefulity/module.json
[ -f src/help.json ] && cat src/help.json > dist/usefulity/help.json
cat build/usefulity.so > dist/usefulity/usefulity.so
chmod +x dist/usefulity/usefulity.so

cd dist
tar -czvf usefulity-module.tar.gz usefulity/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/usefulity/"
echo "Tarball: dist/usefulity-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
