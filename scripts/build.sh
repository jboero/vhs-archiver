#!/bin/bash
# VHS-Codec: Fedora setup and build script
set -e

echo "╔══════════════════════════════════════════╗"
echo "║     VHS-Codec Build Script (Fedora)      ║"
echo "╚══════════════════════════════════════════╝"

# Install dependencies
echo ""
echo "▸ Installing dependencies..."
sudo dnf install -y \
    golang gcc-c++ cmake \
    ffmpeg ffmpeg-free-devel v4l-utils \
    qrencode-devel zbar-devel \
    qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtcharts-devel \
    2>/dev/null || true

# Build Go CLI
echo ""
echo "▸ Building Go CLI..."
cd "$(dirname "$0")/.."
go mod tidy
go build -o vhs-codec ./cmd/vhscodec
echo "  ✓ Built: ./vhs-codec"

# Build Qt6 UI
echo ""
echo "▸ Building Qt6 UI..."
cd ui
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
echo "  ✓ Built: ./ui/build/vhs-codec-ui"

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║              Build Complete!             ║"
echo "╠══════════════════════════════════════════╣"
echo "║  CLI:  ./vhs-codec --help               ║"
echo "║  GUI:  ./ui/build/vhs-codec-ui          ║"
echo "╚══════════════════════════════════════════╝"
echo ""
echo "Quick test:"
echo "  ./vhs-codec estimate --qr-version 25 --ec-level M --fps 10"
echo ""
echo "List your USB capture devices:"
echo "  v4l2-ctl --list-devices"
