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

package audio

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"math"
	"os"
	"os/exec"
)

// FSKConfig holds audio channel parameters
type FSKConfig struct {
	SampleRate int     // audio sample rate (default 44100)
	BaudRate   int     // symbols per second (default 1200)
	FreqMark   float64 // frequency for '1' bit (default 2400 Hz)
	FreqSpace  float64 // frequency for '0' bit (default 1200 Hz)
	Amplitude  float64 // signal amplitude 0.0-1.0 (default 0.8)
}

func DefaultFSKConfig() FSKConfig {
	return FSKConfig{
		SampleRate: 44100,
		BaudRate:   1200,
		FreqMark:   2400,
		FreqSpace:  1200,
		Amplitude:  0.8,
	}
}

// AudioEncoder generates FSK-modulated audio from data
type AudioEncoder struct {
	cfg FSKConfig
}

func NewEncoder(cfg FSKConfig) *AudioEncoder {
	return &AudioEncoder{cfg: cfg}
}

// Encode generates raw PCM S16LE samples for the given data
func (e *AudioEncoder) Encode(data []byte) []int16 {
	samplesPerBit := e.cfg.SampleRate / e.cfg.BaudRate
	totalBits := (len(data) + 6) * 8 // data + header + CRC
	samples := make([]int16, 0, totalBits*samplesPerBit)

	// Preamble: alternating tones for sync (64 bits)
	for i := 0; i < 64; i++ {
		freq := e.cfg.FreqMark
		if i%2 == 0 {
			freq = e.cfg.FreqSpace
		}
		samples = append(samples, e.generateTone(freq, samplesPerBit)...)
	}

	// Start-of-frame marker: 8 mark bits
	for i := 0; i < 8; i++ {
		samples = append(samples, e.generateTone(e.cfg.FreqMark, samplesPerBit)...)
	}

	// Length header (2 bytes, big-endian)
	lenBuf := make([]byte, 2)
	binary.BigEndian.PutUint16(lenBuf, uint16(len(data)))
	for _, b := range lenBuf {
		samples = append(samples, e.encodeByte(b, samplesPerBit)...)
	}

	// Data payload
	for _, b := range data {
		samples = append(samples, e.encodeByte(b, samplesPerBit)...)
	}

	// CRC32 trailer (4 bytes)
	crc := crc32.ChecksumIEEE(data)
	crcBuf := make([]byte, 4)
	binary.BigEndian.PutUint32(crcBuf, crc)
	for _, b := range crcBuf {
		samples = append(samples, e.encodeByte(b, samplesPerBit)...)
	}

	// End-of-frame: 8 space bits
	for i := 0; i < 8; i++ {
		samples = append(samples, e.generateTone(e.cfg.FreqSpace, samplesPerBit)...)
	}

	return samples
}

func (e *AudioEncoder) encodeByte(b byte, samplesPerBit int) []int16 {
	samples := make([]int16, 0, 8*samplesPerBit)
	for bit := 7; bit >= 0; bit-- {
		freq := e.cfg.FreqSpace
		if (b>>uint(bit))&1 == 1 {
			freq = e.cfg.FreqMark
		}
		samples = append(samples, e.generateTone(freq, samplesPerBit)...)
	}
	return samples
}

func (e *AudioEncoder) generateTone(freq float64, numSamples int) []int16 {
	samples := make([]int16, numSamples)
	for i := 0; i < numSamples; i++ {
		t := float64(i) / float64(e.cfg.SampleRate)
		val := e.cfg.Amplitude * math.Sin(2*math.Pi*freq*t)
		samples[i] = int16(val * 32767)
	}
	return samples
}

// EncodeToWAV writes the FSK-encoded data as a WAV file
func (e *AudioEncoder) EncodeToWAV(data []byte, outputPath string) error {
	samples := e.Encode(data)

	f, err := os.Create(outputPath)
	if err != nil {
		return err
	}
	defer f.Close()

	// Write WAV header
	dataSize := uint32(len(samples) * 2) // 16-bit samples
	fileSize := uint32(36 + dataSize)

	// RIFF header
	f.Write([]byte("RIFF"))
	binary.Write(f, binary.LittleEndian, fileSize)
	f.Write([]byte("WAVE"))

	// fmt chunk
	f.Write([]byte("fmt "))
	binary.Write(f, binary.LittleEndian, uint32(16)) // chunk size
	binary.Write(f, binary.LittleEndian, uint16(1))  // PCM
	binary.Write(f, binary.LittleEndian, uint16(1))  // mono
	binary.Write(f, binary.LittleEndian, uint32(e.cfg.SampleRate))
	binary.Write(f, binary.LittleEndian, uint32(e.cfg.SampleRate*2)) // byte rate
	binary.Write(f, binary.LittleEndian, uint16(2))                   // block align
	binary.Write(f, binary.LittleEndian, uint16(16))                  // bits per sample

	// data chunk
	f.Write([]byte("data"))
	binary.Write(f, binary.LittleEndian, dataSize)
	for _, s := range samples {
		binary.Write(f, binary.LittleEndian, s)
	}

	return nil
}

// AudioDecoder demodulates FSK audio back to data
type AudioDecoder struct {
	cfg FSKConfig
}

func NewDecoder(cfg FSKConfig) *AudioDecoder {
	return &AudioDecoder{cfg: cfg}
}

// Decode processes raw PCM S16LE samples and extracts data
func (d *AudioDecoder) Decode(samples []int16) ([]byte, error) {
	samplesPerBit := d.cfg.SampleRate / d.cfg.BaudRate

	// Simple Goertzel-based frequency detection per bit period
	bits := make([]byte, 0)

	for i := 0; i+samplesPerBit <= len(samples); i += samplesPerBit {
		window := samples[i : i+samplesPerBit]
		markPower := d.goertzel(window, d.cfg.FreqMark)
		spacePower := d.goertzel(window, d.cfg.FreqSpace)

		if markPower > spacePower {
			bits = append(bits, 1)
		} else {
			bits = append(bits, 0)
		}
	}

	// Find start-of-frame (8 consecutive 1-bits after preamble)
	frameStart := -1
	consecutive := 0
	for i, b := range bits {
		if b == 1 {
			consecutive++
			if consecutive >= 8 {
				frameStart = i + 1
				break
			}
		} else {
			consecutive = 0
		}
	}

	if frameStart < 0 {
		return nil, fmt.Errorf("no start-of-frame marker found")
	}

	// Decode bytes from bit stream
	rawBytes := d.bitsToBytes(bits[frameStart:])

	if len(rawBytes) < 6 { // 2 len + min 0 data + 4 crc
		return nil, fmt.Errorf("insufficient data decoded: %d bytes", len(rawBytes))
	}

	// Extract length
	dataLen := int(binary.BigEndian.Uint16(rawBytes[:2]))
	if dataLen+6 > len(rawBytes) {
		return nil, fmt.Errorf("data length %d exceeds decoded bytes %d", dataLen, len(rawBytes)-6)
	}

	data := rawBytes[2 : 2+dataLen]
	crcBytes := rawBytes[2+dataLen : 2+dataLen+4]
	expectedCRC := binary.BigEndian.Uint32(crcBytes)
	actualCRC := crc32.ChecksumIEEE(data)

	if actualCRC != expectedCRC {
		return data, fmt.Errorf("CRC mismatch: expected %08x got %08x", expectedCRC, actualCRC)
	}

	return data, nil
}

// goertzel computes the power at a specific frequency using Goertzel algorithm
func (d *AudioDecoder) goertzel(samples []int16, targetFreq float64) float64 {
	n := len(samples)
	k := int(0.5 + float64(n)*targetFreq/float64(d.cfg.SampleRate))
	w := 2.0 * math.Pi * float64(k) / float64(n)
	coeff := 2.0 * math.Cos(w)

	s0, s1, s2 := 0.0, 0.0, 0.0
	for _, sample := range samples {
		s0 = float64(sample)/32768.0 + coeff*s1 - s2
		s2 = s1
		s1 = s0
	}

	return s1*s1 + s2*s2 - coeff*s1*s2
}

func (d *AudioDecoder) bitsToBytes(bits []byte) []byte {
	result := make([]byte, 0, len(bits)/8)
	for i := 0; i+8 <= len(bits); i += 8 {
		var b byte
		for j := 0; j < 8; j++ {
			b = (b << 1) | bits[i+j]
		}
		result = append(result, b)
	}
	return result
}

// DecodeFromDevice captures audio from an ALSA device and decodes
func (d *AudioDecoder) DecodeFromDevice(device string, durationSec float64) ([]byte, error) {
	cmd := exec.Command("ffmpeg",
		"-f", "alsa",
		"-i", device,
		"-t", fmt.Sprintf("%.0f", durationSec),
		"-f", "s16le",
		"-ac", "1",
		"-ar", fmt.Sprintf("%d", d.cfg.SampleRate),
		"pipe:1",
	)

	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("audio capture failed: %w", err)
	}

	// Convert raw bytes to int16 samples
	samples := make([]int16, len(out)/2)
	for i := 0; i < len(samples); i++ {
		samples[i] = int16(binary.LittleEndian.Uint16(out[i*2 : i*2+2]))
	}

	return d.Decode(samples)
}
