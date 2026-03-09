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

package config

import (
	"fmt"
	"time"
)

const (
	NTSCWidth     = 720
	NTSCHeight    = 480
	NTSCFrameRate = 29.97
	NTSCPixelFmt  = "uyvy422"
)

type ECLevel int

const (
	ECLevelL ECLevel = iota
	ECLevelM
	ECLevelQ
	ECLevelH
)

func (e ECLevel) String() string {
	return [...]string{"L", "M", "Q", "H"}[e]
}

func ParseECLevel(s string) (ECLevel, error) {
	switch s {
	case "L", "l":
		return ECLevelL, nil
	case "M", "m":
		return ECLevelM, nil
	case "Q", "q":
		return ECLevelQ, nil
	case "H", "h":
		return ECLevelH, nil
	default:
		return ECLevelM, fmt.Errorf("unknown EC level: %s (use L/M/Q/H)", s)
	}
}

type FrameType byte

const (
	FrameTypeData      FrameType = 0x01
	FrameTypeSync      FrameType = 0x02
	FrameTypeCalibr    FrameType = 0x03
	FrameTypeHeader    FrameType = 0x04
	FrameTypeAudioSync FrameType = 0x05
	FrameTypeEOF       FrameType = 0xFF
)

type EncoderConfig struct {
	QRVersion    int     `json:"qr_version"`
	ECLevel      ECLevel `json:"ec_level"`
	ModulePixels int     `json:"module_pixels"`
	GrayLevels   int     `json:"gray_levels"`
	DataFPS      float64 `json:"data_fps"`
	SyncEveryN   int     `json:"sync_every_n"`
	FECRatio     float64 `json:"fec_ratio"`
	AudioEnabled bool    `json:"audio_enabled"`
	AudioBitrate int     `json:"audio_bitrate"`
	AudioFSKLow  int     `json:"audio_fsk_low"`
	AudioFSKHigh int     `json:"audio_fsk_high"`
	AudioBaudRate int    `json:"audio_baud_rate"`
	VideoDevice  string  `json:"video_device"`
	AudioDevice  string  `json:"audio_device"`
	Resolution   [2]int  `json:"resolution"`
	OutputFile   string  `json:"output_file"`
	InputFile    string  `json:"input_file"`
}

func DefaultEncoderConfig() EncoderConfig {
	return EncoderConfig{
		QRVersion:    25,
		ECLevel:      ECLevelM,
		ModulePixels: 4,
		GrayLevels:   2,
		DataFPS:      10.0,
		SyncEveryN:   50,
		FECRatio:     0.3,
		AudioEnabled: false,
		AudioBitrate: 44100,
		AudioFSKLow:  1200,
		AudioFSKHigh: 2400,
		AudioBaudRate: 1200,
		Resolution:   [2]int{NTSCWidth, NTSCHeight},
	}
}

type DecoderConfig struct {
	VideoDevice  string        `json:"video_device"`
	AudioDevice  string        `json:"audio_device"`
	InputFile    string        `json:"input_file"`
	OutputFile   string        `json:"output_file"`
	AudioEnabled bool          `json:"audio_enabled"`
	Timeout      time.Duration `json:"timeout"`
}

type CalibrateConfig struct {
	DeviceIn       string    `json:"device_in"`
	DeviceOut      string    `json:"device_out"`
	SweepVersions  []int     `json:"sweep_versions"`
	SweepGray      []int     `json:"sweep_gray"`
	SweepFPS       []float64 `json:"sweep_fps"`
	SweepModulePx  []int     `json:"sweep_module_px"`
	TrialsPerCombo int       `json:"trials_per_combo"`
}

func DefaultCalibrateConfig() CalibrateConfig {
	return CalibrateConfig{
		SweepVersions:  []int{10, 15, 20, 25, 30, 35, 40},
		SweepGray:      []int{2, 4},
		SweepFPS:       []float64{5, 10, 15, 20, 29.97},
		SweepModulePx:  []int{3, 4, 5, 6},
		TrialsPerCombo: 10,
	}
}

type FrameHeader struct {
	Magic       [2]byte
	Type        FrameType
	SeqNum      uint32
	TotalChunks uint32
	ChunkSize   uint16
	Checksum    uint32
}

const FrameHeaderSize = 2 + 1 + 4 + 4 + 2 + 4 // 17 bytes

func QRCapacity(version int, ec ECLevel) int {
	table := map[int][4]int{
		1:  {17, 14, 11, 7},
		5:  {106, 84, 62, 46},
		10: {271, 213, 151, 119},
		15: {412, 311, 235, 178},
		20: {586, 450, 331, 261},
		25: {755, 590, 427, 341},
		30: {1003, 769, 573, 445},
		35: {1249, 959, 706, 552},
		40: {2953, 2331, 1663, 1273},
	}
	closest := 1
	for v := range table {
		if v <= version && v > closest {
			closest = v
		}
	}
	caps := table[closest]
	return caps[ec]
}

func EstimateThroughput(cfg EncoderConfig) ThroughputEstimate {
	qrCap := QRCapacity(cfg.QRVersion, cfg.ECLevel)
	binaryCapacity := (qrCap / 4) * 3
	payloadPerFrame := binaryCapacity - FrameHeaderSize

	bitsPerModule := 1
	switch cfg.GrayLevels {
	case 4:
		bitsPerModule = 2
	case 8:
		bitsPerModule = 3
	}
	payloadPerFrame = payloadPerFrame * bitsPerModule

	effectivePayload := float64(payloadPerFrame) * (1.0 - cfg.FECRatio)

	syncOverhead := 1.0 - (1.0 / float64(cfg.SyncEveryN))

	bytesPerSec := effectivePayload * cfg.DataFPS * syncOverhead

	audioBPS := 0.0
	if cfg.AudioEnabled {
		audioBPS = float64(cfg.AudioBaudRate) / 8.0 * 0.8
	}

	totalBPS := bytesPerSec + audioBPS

	return ThroughputEstimate{
		PayloadPerFrame: int(effectivePayload),
		VideoBytesSec:   bytesPerSec,
		AudioBytesSec:   audioBPS,
		TotalBytesSec:   totalBPS,
		CapacitySP2Hr:   totalBPS * 7200,
		CapacityLP4Hr:   totalBPS * 14400,
		CapacityEP6Hr:   totalBPS * 21600,
	}
}

type ThroughputEstimate struct {
	PayloadPerFrame int     `json:"payload_per_frame"`
	VideoBytesSec   float64 `json:"video_bytes_sec"`
	AudioBytesSec   float64 `json:"audio_bytes_sec"`
	TotalBytesSec   float64 `json:"total_bytes_sec"`
	CapacitySP2Hr   float64 `json:"capacity_sp_2hr"`
	CapacityLP4Hr   float64 `json:"capacity_lp_4hr"`
	CapacityEP6Hr   float64 `json:"capacity_ep_6hr"`
}

func (t ThroughputEstimate) String() string {
	return fmt.Sprintf(
		"Throughput: %.1f KB/s (video: %.1f KB/s, audio: %.1f KB/s)\n"+
			"Capacity estimates:\n"+
			"  SP (2hr): %.1f MB\n"+
			"  LP (4hr): %.1f MB\n"+
			"  EP (6hr): %.1f MB",
		t.TotalBytesSec/1024, t.VideoBytesSec/1024, t.AudioBytesSec/1024,
		t.CapacitySP2Hr/1024/1024,
		t.CapacityLP4Hr/1024/1024,
		t.CapacityEP6Hr/1024/1024,
	)
}
