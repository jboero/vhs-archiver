#!/bin/bash
set -e
echo "VHS-Codec Build Script (Fedora)"
sudo dnf install -y golang gcc-c++ cmake ffmpeg ffmpeg-free-devel v4l-utils \
    qrencode-devel zbar-devel qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtcharts-devel 2>/dev/null || true
cd "$(dirname "$0")/.."
go mod tidy
go build -o vhs-codec ./cmd/vhscodec
echo "Built: ./vhs-codec"
cd ui && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel
echo "Built: ./ui/build/vhs-codec-ui"
