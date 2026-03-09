// VHS-Codec: Digital data storage on VHS tape
// Copyright (C) 2025 John Boero
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

package encoder

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"os/exec"
	"time"

	qrcode "github.com/skip2/go-qrcode"

	"github.com/johnny/vhs-codec/pkg/config"
)

// Encoder handles chunking input data and generating QR video frames
type Encoder struct {
	cfg    config.EncoderConfig
	seqNum uint32
}

func New(cfg config.EncoderConfig) *Encoder {
	return &Encoder{cfg: cfg}
}

// maxPayload returns the usable bytes per QR frame after header.
// Since we base64-encode binary data for QR compatibility, the QR capacity
// in characters is divided by the base64 expansion ratio (4/3) to get
// the raw binary capacity.
func (e *Encoder) maxPayload() int {
	qrCap := config.QRCapacity(e.cfg.QRVersion, e.cfg.ECLevel)
	// base64 of (header + payload) must fit in qrCap characters
	// base64 output length = ceil(input/3)*4, so max input = floor(qrCap/4)*3
	maxBinary := (qrCap / 4) * 3
	cap := maxBinary - config.FrameHeaderSize
	if cap < 1 {
		cap = 1
	}
	return cap
}

// buildFrame creates a framed data chunk with header
func (e *Encoder) buildFrame(frameType config.FrameType, payload []byte, totalChunks uint32) []byte {
	hdr := config.FrameHeader{
		Magic:       [2]byte{'V', 'H'},
		Type:        frameType,
		SeqNum:      e.seqNum,
		TotalChunks: totalChunks,
		ChunkSize:   uint16(len(payload)),
		Checksum:    crc32.ChecksumIEEE(payload),
	}
	e.seqNum++

	buf := new(bytes.Buffer)
	binary.Write(buf, binary.BigEndian, hdr)
	buf.Write(payload)
	return buf.Bytes()
}

// renderQRFrame generates an NTSC-sized image with a centered QR code
func (e *Encoder) renderQRFrame(data []byte) (image.Image, error) {
	// Map our EC level to the library's
	ecMap := map[config.ECLevel]qrcode.RecoveryLevel{
		config.ECLevelL: qrcode.Low,
		config.ECLevelM: qrcode.Medium,
		config.ECLevelQ: qrcode.High,
		config.ECLevelH: qrcode.Highest,
	}

	// Encode binary data as base64 so the QR string encoder handles it cleanly
	encoded := base64.StdEncoding.EncodeToString(data)

	// Use Encode() to get PNG bytes directly — this is the most reliable
	// code path in go-qrcode. We request a size that fits within NTSC height
	// with some margin for the quiet zone.
	targetSize := e.cfg.Resolution[1] - 40 // leave 20px margin top+bottom
	if targetSize < 100 {
		targetSize = 100
	}

	pngBytes, err := qrcode.Encode(encoded, ecMap[e.cfg.ECLevel], targetSize)
	if err != nil {
		return nil, fmt.Errorf("QR encode failed (payload %d bytes, base64 %d chars): %w",
			len(data), len(encoded), err)
	}

	// Decode the PNG back to an image
	qrImg, err := png.Decode(bytes.NewReader(pngBytes))
	if err != nil {
		return nil, fmt.Errorf("QR PNG decode failed: %w", err)
	}

	imgBounds := qrImg.Bounds()
	actualW := imgBounds.Dx()
	actualH := imgBounds.Dy()

	if actualW == 0 || actualH == 0 {
		return nil, fmt.Errorf("QR image generation produced 0x0 image (payload %d bytes, base64 %d chars)",
			len(data), len(encoded))
	}

	// Create NTSC frame with black background
	w, h := e.cfg.Resolution[0], e.cfg.Resolution[1]
	frame := image.NewRGBA(image.Rect(0, 0, w, h))

	// Fill black using draw for efficiency
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			frame.SetRGBA(x, y, color.RGBA{0, 0, 0, 255})
		}
	}

	// Center the QR code in the frame
	offsetX := (w - actualW) / 2
	offsetY := (h - actualH) / 2

	// Copy QR into frame
	for y := imgBounds.Min.Y; y < imgBounds.Max.Y; y++ {
		for x := imgBounds.Min.X; x < imgBounds.Max.X; x++ {
			r, g, b, a := qrImg.At(x, y).RGBA()
			dx := offsetX + (x - imgBounds.Min.X)
			dy := offsetY + (y - imgBounds.Min.Y)
			if dx >= 0 && dx < w && dy >= 0 && dy < h {
				frame.SetRGBA(dx, dy, color.RGBA{
					uint8(r >> 8), uint8(g >> 8), uint8(b >> 8), uint8(a >> 8),
				})
			}
		}
	}

	return frame, nil
}

// renderSyncFrame generates a sync/calibration frame with known patterns
func (e *Encoder) renderSyncFrame(seqNum uint32) (image.Image, error) {
	syncData := fmt.Sprintf("VH-SYNC:%08d:%d", seqNum, time.Now().UnixMilli())
	return e.renderQRFrame([]byte(syncData))
}

// EncodeToFile encodes input data to a raw video file (for testing/preview)
func (e *Encoder) EncodeToFile(input io.Reader, outputPath string) error {
	data, err := io.ReadAll(input)
	if err != nil {
		return fmt.Errorf("reading input: %w", err)
	}

	payload := e.maxPayload()
	totalChunks := uint32((len(data) + payload - 1) / payload)

	fmt.Printf("Input: %d bytes → %d chunks of %d bytes
", len(data), totalChunks, payload)
	fmt.Printf("Settings: QR v%d, EC-%s, %dpx modules, %d gray levels
",
		e.cfg.QRVersion, e.cfg.ECLevel, e.cfg.ModulePixels, e.cfg.GrayLevels)

	est := config.EstimateThroughput(e.cfg)
	fmt.Println(est)

	// Write frames to a temp directory, then assemble with ffmpeg.
	// This is more reliable than piping PNGs, which can be fragile.
	tmpDir, err := os.MkdirTemp("", "vhs-codec-frames-")
	if err != nil {
		return fmt.Errorf("creating temp dir: %w", err)
	}
	defer os.RemoveAll(tmpDir)

	frameNum := 0

	writeFrameFile := func(frameData []byte) error {
		img, err := e.renderQRFrame(frameData)
		if err != nil {
			return err
		}
		path := fmt.Sprintf("%s/frame_%08d.png", tmpDir, frameNum)
		f, err := os.Create(path)
		if err != nil {
			return err
		}
		defer f.Close()
		if err := png.Encode(f, img); err != nil {
			return fmt.Errorf("png encode frame %d: %w", frameNum, err)
		}
		frameNum++
		return nil
	}

	// Generate header frame with file metadata
	headerPayload := fmt.Sprintf("VH-HDR:size=%d:chunks=%d:crc=%08x",
		len(data), totalChunks, crc32.ChecksumIEEE(data))
	headerFrame := e.buildFrame(config.FrameTypeHeader, []byte(headerPayload), totalChunks)
	if err := writeFrameFile(headerFrame); err != nil {
		return err
	}

	// Write initial sync frames
	for i := 0; i < 5; i++ {
		syncFrame := e.buildFrame(config.FrameTypeSync, []byte("SYNC-INIT"), 0)
		if err := writeFrameFile(syncFrame); err != nil {
			return err
		}
	}

	// Encode data chunks
	for i := uint32(0); i < totalChunks; i++ {
		start := int(i) * payload
		end := start + payload
		if end > len(data) {
			end = len(data)
		}

		chunk := data[start:end]
		frame := e.buildFrame(config.FrameTypeData, chunk, totalChunks)

		if err := writeFrameFile(frame); err != nil {
			return fmt.Errorf("writing frame %d: %w", i, err)
		}

		// Periodic sync frames
		if e.cfg.SyncEveryN > 0 && (i+1)%uint32(e.cfg.SyncEveryN) == 0 {
			syncFrame := e.buildFrame(config.FrameTypeSync, []byte(fmt.Sprintf("SYNC:%d", i)), 0)
			if err := writeFrameFile(syncFrame); err != nil {
				return err
			}
		}

		if (i+1)%100 == 0 || i == totalChunks-1 {
			fmt.Printf("Encoded %d/%d frames (%.1f%%)", i+1, totalChunks,
				float64(i+1)/float64(totalChunks)*100)
		}
	}

	// EOF frame
	eofFrame := e.buildFrame(config.FrameTypeEOF, []byte("VH-EOF"), 0)
	if err := writeFrameFile(eofFrame); err != nil {
		return err
	}

	fmt.Printf("
%d frames written, assembling video...
", frameNum)

	// Assemble frames into video with ffmpeg
	fps := fmt.Sprintf("%.2f", e.cfg.DataFPS)
	cmd := exec.Command("ffmpeg",
		"-y",
		"-framerate", fps,
		"-i", fmt.Sprintf("%s/frame_%%08d.png", tmpDir),
		"-c:v", "libx264",
		"-preset", "ultrafast",
		"-crf", "0",
		"-pix_fmt", "yuv444p", // preserve sharp edges for QR codes
		"-r", fps,
		outputPath,
	)

	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr

	if err := cmd.Run(); err != nil {
		return fmt.Errorf("ffmpeg assembly failed: %w", err)
	}

	fmt.Println("Done.")
	return nil
}

// EncodeToDevice streams encoded video to a V4L2 output device
func (e *Encoder) EncodeToDevice(input io.Reader) error {
	if e.cfg.VideoDevice == "" {
		return fmt.Errorf("no video device specified")
	}

	data, err := io.ReadAll(input)
	if err != nil {
		return fmt.Errorf("reading input: %w", err)
	}

	payload := e.maxPayload()
	totalChunks := uint32((len(data) + payload - 1) / payload)

	est := config.EstimateThroughput(e.cfg)
	fmt.Printf("Encoding %d bytes → %d chunks
%s
", len(data), totalChunks, est)

	// ffmpeg piping raw frames to V4L2 output device
	w, h := e.cfg.Resolution[0], e.cfg.Resolution[1]
	fps := fmt.Sprintf("%.2f", NTSCFrameRate) // always output at NTSC rate

	cmd := exec.Command("ffmpeg",
		"-y",
		"-f", "image2pipe",
		"-framerate", fmt.Sprintf("%.2f", e.cfg.DataFPS),
		"-i", "pipe:0",
		"-f", "v4l2",
		"-pix_fmt", NTSCPixelFmt,
		"-s", fmt.Sprintf("%dx%d", w, h),
		"-r", fps,
		e.cfg.VideoDevice,
	)

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return fmt.Errorf("ffmpeg stdin: %w", err)
	}

	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("starting ffmpeg: %w", err)
	}

	// Same encoding loop as EncodeToFile
	headerPayload := fmt.Sprintf("VH-HDR:size=%d:chunks=%d:crc=%08x",
		len(data), totalChunks, crc32.ChecksumIEEE(data))
	e.writeFrame(stdin, e.buildFrame(config.FrameTypeHeader, []byte(headerPayload), totalChunks))

	for i := 0; i < 5; i++ {
		e.writeFrame(stdin, e.buildFrame(config.FrameTypeSync, []byte("SYNC-INIT"), 0))
	}

	frameDuration := time.Duration(float64(time.Second) / e.cfg.DataFPS)

	for i := uint32(0); i < totalChunks; i++ {
		start := time.Now()

		s := int(i) * payload
		end := s + payload
		if end > len(data) {
			end = len(data)
		}

		frame := e.buildFrame(config.FrameTypeData, data[s:end], totalChunks)
		if err := e.writeFrame(stdin, frame); err != nil {
			return fmt.Errorf("frame %d: %w", i, err)
		}

		if e.cfg.SyncEveryN > 0 && (i+1)%uint32(e.cfg.SyncEveryN) == 0 {
			e.writeFrame(stdin, e.buildFrame(config.FrameTypeSync,
				[]byte(fmt.Sprintf("SYNC:%d", i)), 0))
		}

		// Pace output to match desired data FPS
		elapsed := time.Since(start)
		if elapsed < frameDuration {
			time.Sleep(frameDuration - elapsed)
		}

		if (i+1)%50 == 0 {
			fmt.Printf("Streaming %d/%d (%.1f%%)", i+1, totalChunks,
				float64(i+1)/float64(totalChunks)*100)
		}
	}

	e.writeFrame(stdin, e.buildFrame(config.FrameTypeEOF, []byte("VH-EOF"), 0))
	stdin.Close()
	fmt.Println("
Done.")
	return cmd.Wait()
}

// writeFrame renders a data payload as a QR code and writes the PNG to the pipe
func (e *Encoder) writeFrame(w io.Writer, data []byte) error {
	img, err := e.renderQRFrame(data)
	if err != nil {
		return err
	}

	return png.Encode(w, img)
}

// NTSCFrameRate and pixel format constants for device output
const (
	NTSCFrameRate = 29.97
	NTSCPixelFmt  = "uyvy422"
)
