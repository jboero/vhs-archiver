# VHS-Archiver: Digital Data Storage on VHS Tape

A proof-of-concept tool for encoding arbitrary digital data onto VHS tapes via
composite video (and optionally hi-fi audio), and decoding it back. Built for a
blog post exploring the practical limits of VHS as a cheap archival medium 
saving VHS tapes from landfill and disposal.

![encode](https://github.com/user-attachments/assets/af5d77eb-f6a3-492c-8b05-98bf80d28fe5)
![capacity](https://github.com/user-attachments/assets/46a10548-74c1-4a18-882b-887967be8cc9)

![danger](https://github.com/user-attachments/assets/d7287587-0d0e-4db9-b5ef-ca564c6ebeb5)

Beware this is indeed vibe coded with Claude Opus v4.6. Use at your own risk.

## Architecture

```
┌──────────────┐     Composite Out       ┌─────────┐
│  vhs-codec   │ ──────────────────────► │   VCR   │ ──► VHS Tape
│  (encoder)   │     USB to RCA Out      │ Record  │
└──────────────┘                         └─────────┘

┌─────────┐     Composite In        ┌──────────────┐
│   VCR   │ ──────────────────────► │  vhs-codec   │ ──► Restored File
│ Playback│     USB Capture Dev     │  (decoder)   │
└─────────┘                         └──────────────┘
```

The encoder generates a raw video stream (and optional audio stream) of QR code
frames containing chunked, error-corrected data. The decoder captures playback
video, detects and decodes QR frames, reassembles chunks, and verifies integrity.

## Prerequisites (Fedora)

```bash
# Core build tools
sudo dnf install golang gcc-c++ cmake

# Video I/O
sudo dnf install ffmpeg ffmpeg-devel v4l-utils

# QR code support
sudo dnf install qrencode-devel zbar-devel

# Qt6 frontend (optional)
sudo dnf install qt6-qtbase-devel qt6-qtmultimedia-devel qt6-qtcharts-devel

# Go dependencies
cd vhs-codec && go mod tidy
```

## Build

```bash
# CLI only
go build -o vhs-codec ./cmd/vhscodec

# Qt6 frontend
cd ui && cmake -B build && cmake --build build
```

## Usage

![usage](https://github.com/user-attachments/assets/31752837-2d97-4bc0-bc7a-d4ad77f2152b)

### Encode a file to VHS

```bash
# List available video devices
./vhs-codec devices

# Encode with default settings (Version 25 QR, EC-M, 10 fps, B&W)
./vhs-codec encode --input myfile.tar.gz --device /dev/video1

# Maximum density (aggressive, for blog testing)
./vhs-codec encode --input myfile.tar.gz --device /dev/video1 \
    --qr-version 40 --ec-level L --fps 29.97 --gray-levels 4 \
    --audio --audio-device hw:1,0

# Conservative archival settings
./vhs-codec encode --input myfile.tar.gz --device /dev/video1 \
    --qr-version 15 --ec-level H --fps 8 --fec-ratio 0.5
```

### Decode from VHS playback

```bash
# Decode with auto-detection
./vhs-codec decode --device /dev/video0 --output restored.tar.gz

# Decode from a previously captured video file
./vhs-codec decode --file capture.avi --output restored.tar.gz
```

### Calibration test

```bash
# Run calibration sweep to find optimal settings for your hardware
./vhs-codec calibrate --device-in /dev/video0 --device-out /dev/video1 \
    --sweep-versions 10,15,20,25,30,35,40 \
    --sweep-gray 2,4 \
    --sweep-fps 5,10,15,20,30
```

## Tunable Parameters

| Parameter       | Flag              | Range         | Default | Notes                            |
|-----------------|-------------------|---------------|---------|----------------------------------|
| QR Version      | `--qr-version`    | 1-40          | 25      | Higher = more data, harder decode|
| EC Level        | `--ec-level`      | L/M/Q/H       | M       | H for archival, L for max density|
| Data FPS        | `--fps`           | 1-29.97       | 10      | Unique QR frames per second      |
| Gray Levels     | `--gray-levels`   | 2/4/8         | 2       | 2=B&W, 4=2bit, 8=3bit per module|
| FEC Ratio       | `--fec-ratio`     | 0.0-1.0       | 0.3     | App-level redundancy (fountain)  |
| Audio Channel   | `--audio`         | flag          | off     | Enable audio data channel        |
| Audio Bitrate   | `--audio-bitrate` | 1000-96000    | 44100   | Audio sample rate for data encode|
| Resolution      | `--resolution`    | 720x480       | 720x480 | NTSC composite frame size        |
| Module Pixels   | `--module-px`     | 2-8           | 4       | Pixels per QR module             |

## Data Flow

1. **Chunking**: Input file is split into chunks sized to fit one QR code
2. **FEC Encoding**: Fountain codes (LT/RaptorQ-style) generate redundant chunks
3. **Framing**: Each chunk gets a header (sequence #, total chunks, checksum)
4. **QR Generation**: Chunks encoded as QR codes at specified version/EC level
5. **Video Output**: QR frames rendered to composite out via ffmpeg/v4l2
6. **Audio Output** (optional): Parallel data stream via FSK on hi-fi audio tracks
7. **Sync Frames**: Periodic sync/calibration frames interleaved for alignment

Decoding reverses this pipeline, with frame deduplication and fountain code
reassembly tolerating missing/corrupt frames.

## Project Structure

```
vhs-codec/
├── cmd/vhscodec/       # CLI entry point
│   └── main.go
├── pkg/
│   ├── encoder/        # QR frame generation + video output
│   ├── decoder/        # Video capture + QR detection + reassembly
│   ├── audio/          # FSK audio data channel
│   ├── calibrate/      # Hardware calibration sweep
│   └── config/         # Shared types and configuration
├── ui/                 # Qt6/C++ frontend
│   ├── CMakeLists.txt
│   ├── main.cpp
│   └── mainwindow.cpp/h
├── scripts/            # Helper scripts
└── go.mod
```

## Authors

- **John Boero** — Viber, design, testing, and hardware integration
- **Claude** (Anthropic) — Code generation and architecture and really most of it

## License

Copyright (C) 2026 John Boero

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE) for details.

## Disclaimer

**THIS SOFTWARE IS EXPERIMENTAL.** The authors shall not be held liable for any
data loss, corruption, or damages arising from the use of this software. VHS
tape is an analog medium with inherent reliability limitations. Do not rely on
this tool as your sole backup strategy. **Use at your own risk.**
