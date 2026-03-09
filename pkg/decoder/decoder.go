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

package decoder

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"image"
	"image/png"
	"io"
	"os"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/makiuchi-d/gozxing"
	gozxingqr "github.com/makiuchi-d/gozxing/qrcode"

	"github.com/jboero/vhs-codec/pkg/config"
)

type Decoder struct {
	cfg config.DecoderConfig

	mu          sync.Mutex
	chunks      map[uint32][]byte
	totalChunks uint32
	fileSize    int
	fileCRC     uint32
	seenFrames  int
	errorFrames int
	dupeFrames  int
}

func New(cfg config.DecoderConfig) *Decoder {
	return &Decoder{cfg: cfg, chunks: make(map[uint32][]byte)}
}

func (d *Decoder) DecodeFromFile(videoPath string) ([]byte, error) {
	fmt.Printf("Decoding from file: %s\n", videoPath)

	cmd := exec.Command("ffmpeg",
		"-i", videoPath,
		"-f", "image2pipe",
		"-vcodec", "png",
		"pipe:1",
	)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("ffmpeg stdout pipe: %w", err)
	}
	cmd.Stderr = os.Stderr

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("starting ffmpeg: %w", err)
	}

	if err := d.processFrameStream(stdout); err != nil {
		cmd.Process.Kill()
		return nil, err
	}

	if err := cmd.Wait(); err != nil {
		fmt.Printf("ffmpeg exit: %v (usually OK)\n", err)
	}

	return d.reassemble()
}

func (d *Decoder) DecodeFromDevice() ([]byte, error) {
	if d.cfg.VideoDevice == "" {
		return nil, fmt.Errorf("no video device specified")
	}

	fmt.Printf("Capturing from device: %s\n", d.cfg.VideoDevice)
	fmt.Println("Press Ctrl+C to stop capture and reassemble...")

	timeout := d.cfg.Timeout
	if timeout == 0 {
		timeout = 4 * time.Hour
	}

	cmd := exec.Command("ffmpeg",
		"-f", "v4l2",
		"-framerate", "29.97",
		"-video_size", "720x480",
		"-i", d.cfg.VideoDevice,
		"-f", "image2pipe",
		"-vcodec", "png",
		"-t", fmt.Sprintf("%.0f", timeout.Seconds()),
		"pipe:1",
	)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("ffmpeg stdout: %w", err)
	}
	cmd.Stderr = os.Stderr

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("starting ffmpeg: %w", err)
	}

	if err := d.processFrameStream(stdout); err != nil {
		cmd.Process.Kill()
		return nil, err
	}

	cmd.Wait()
	return d.reassemble()
}

func (d *Decoder) processFrameStream(r io.Reader) error {
	reader := gozxingqr.NewQRCodeReader()

	buf := make([]byte, 0, 1024*1024)
	tmp := make([]byte, 32768)

	for {
		n, err := r.Read(tmp)
		if n > 0 {
			buf = append(buf, tmp[:n]...)
		}

		for {
			img, consumed, decErr := tryDecodePNG(buf)
			if decErr != nil || img == nil {
				break
			}
			buf = buf[consumed:]

			d.seenFrames++
			qrData, qrErr := decodeQRFromImage(reader, img)
			if qrErr != nil {
				d.errorFrames++
				continue
			}

			rawData, b64Err := base64.StdEncoding.DecodeString(string(qrData))
			if b64Err != nil {
				d.errorFrames++
				continue
			}

			d.processQRData(rawData)

			if d.isComplete() {
				fmt.Printf("\nAll %d chunks received!\n", d.totalChunks)
				return nil
			}

			if d.seenFrames%100 == 0 {
				d.printProgress()
			}
		}

		if err == io.EOF {
			break
		}
		if err != nil {
			return fmt.Errorf("reading frames: %w", err)
		}
	}

	d.printProgress()
	return nil
}

func tryDecodePNG(buf []byte) (image.Image, int, error) {
	if len(buf) < 8 {
		return nil, 0, nil
	}

	pngSig := []byte{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}
	if !bytes.HasPrefix(buf, pngSig) {
		idx := bytes.Index(buf[1:], pngSig)
		if idx < 0 {
			return nil, 0, nil
		}
		return nil, idx + 1, fmt.Errorf("skipped %d bytes", idx+1)
	}

	r := bytes.NewReader(buf)
	img, err := png.Decode(r)
	if err != nil {
		return nil, 0, nil
	}

	consumed := int(r.Size()) - r.Len()
	return img, consumed, nil
}

func decodeQRFromImage(reader gozxing.Reader, img image.Image) ([]byte, error) {
	bmp, err := gozxing.NewBinaryBitmapFromImage(img)
	if err != nil {
		return nil, err
	}

	result, err := reader.Decode(bmp, nil)
	if err != nil {
		return nil, err
	}

	return []byte(result.GetText()), nil
}

func (d *Decoder) processQRData(data []byte) {
	if len(data) < config.FrameHeaderSize {
		d.errorFrames++
		return
	}

	var hdr config.FrameHeader
	r := bytes.NewReader(data)
	if err := binary.Read(r, binary.BigEndian, &hdr); err != nil {
		d.errorFrames++
		return
	}

	if hdr.Magic != [2]byte{'V', 'H'} {
		d.errorFrames++
		return
	}

	payload := data[config.FrameHeaderSize:]
	if len(payload) < int(hdr.ChunkSize) {
		d.errorFrames++
		return
	}
	payload = payload[:hdr.ChunkSize]

	if crc32.ChecksumIEEE(payload) != hdr.Checksum {
		d.errorFrames++
		return
	}

	switch hdr.Type {
	case config.FrameTypeHeader:
		d.parseHeaderFrame(payload)
	case config.FrameTypeData:
		d.mu.Lock()
		if _, exists := d.chunks[hdr.SeqNum]; exists {
			d.dupeFrames++
		} else {
			d.chunks[hdr.SeqNum] = make([]byte, len(payload))
			copy(d.chunks[hdr.SeqNum], payload)
		}
		if hdr.TotalChunks > 0 {
			d.totalChunks = hdr.TotalChunks
		}
		d.mu.Unlock()
	case config.FrameTypeSync:
		// sync frames help alignment
	case config.FrameTypeEOF:
		fmt.Println("\nReceived EOF frame")
	}
}

func (d *Decoder) parseHeaderFrame(payload []byte) {
	s := string(payload)
	if !strings.HasPrefix(s, "VH-HDR:") {
		return
	}

	parts := strings.Split(s[7:], ":")
	for _, p := range parts {
		kv := strings.SplitN(p, "=", 2)
		if len(kv) != 2 {
			continue
		}
		switch kv[0] {
		case "size":
			d.fileSize, _ = strconv.Atoi(kv[1])
		case "chunks":
			v, _ := strconv.Atoi(kv[1])
			d.totalChunks = uint32(v)
		case "crc":
			v, _ := strconv.ParseUint(kv[1], 16, 32)
			d.fileCRC = uint32(v)
		}
	}
	fmt.Printf("Header: file size=%d, chunks=%d, CRC=%08x\n",
		d.fileSize, d.totalChunks, d.fileCRC)
}

func (d *Decoder) isComplete() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.totalChunks == 0 {
		return false
	}
	return uint32(len(d.chunks)) >= d.totalChunks
}

func (d *Decoder) reassemble() ([]byte, error) {
	d.mu.Lock()
	defer d.mu.Unlock()

	if d.totalChunks == 0 {
		return nil, fmt.Errorf("never received header frame with total chunk count")
	}

	type seqChunk struct {
		seq  uint32
		data []byte
	}

	sorted := make([]seqChunk, 0, len(d.chunks))
	for seq, data := range d.chunks {
		sorted = append(sorted, seqChunk{seq, data})
	}
	sort.Slice(sorted, func(i, j int) bool {
		return sorted[i].seq < sorted[j].seq
	})

	if uint32(len(sorted)) < d.totalChunks {
		fmt.Printf("WARNING: Only received %d/%d chunks (%.1f%%)\n",
			len(sorted), d.totalChunks,
			float64(len(sorted))/float64(d.totalChunks)*100)
	}

	var result bytes.Buffer
	for _, sc := range sorted {
		result.Write(sc.data)
	}

	data := result.Bytes()

	if d.fileSize > 0 && len(data) >= d.fileSize {
		data = data[:d.fileSize]
	}

	if d.fileCRC > 0 {
		actual := crc32.ChecksumIEEE(data)
		if actual == d.fileCRC {
			fmt.Println("CRC32 verification: PASS")
		} else {
			fmt.Printf("CRC32 verification: FAIL (expected %08x, got %08x)\n",
				d.fileCRC, actual)
		}
	}

	return data, nil
}

func (d *Decoder) printProgress() {
	d.mu.Lock()
	defer d.mu.Unlock()

	total := "?"
	pct := ""
	if d.totalChunks > 0 {
		total = fmt.Sprintf("%d", d.totalChunks)
		pct = fmt.Sprintf(" (%.1f%%)", float64(len(d.chunks))/float64(d.totalChunks)*100)
	}

	fmt.Printf("\rFrames: %d seen, %d decoded, %d errors, %d dupes | Chunks: %d/%s%s",
		d.seenFrames, len(d.chunks), d.errorFrames, d.dupeFrames,
		len(d.chunks), total, pct)
}

func (d *Decoder) Stats() map[string]interface{} {
	d.mu.Lock()
	defer d.mu.Unlock()

	return map[string]interface{}{
		"frames_seen":    d.seenFrames,
		"frames_decoded": len(d.chunks),
		"frames_errors":  d.errorFrames,
		"frames_dupes":   d.dupeFrames,
		"total_chunks":   d.totalChunks,
		"chunks_received": len(d.chunks),
		"file_size":      d.fileSize,
	}
}
