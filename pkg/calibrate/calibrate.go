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

package calibrate

import (
	"encoding/json"
	"fmt"
	"os"
	"time"

	"github.com/johnny/vhs-codec/pkg/config"
	"github.com/johnny/vhs-codec/pkg/decoder"
	"github.com/johnny/vhs-codec/pkg/encoder"
)

// Result holds the outcome of a single calibration trial
type Result struct {
	QRVersion    int     `json:"qr_version"`
	ModulePixels int     `json:"module_px"`
	GrayLevels   int     `json:"gray_levels"`
	DataFPS      float64 `json:"data_fps"`
	ECLevel      string  `json:"ec_level"`

	FramesSent    int     `json:"frames_sent"`
	FramesDecoded int     `json:"frames_decoded"`
	FrameErrors   int     `json:"frame_errors"`
	DecodeRate    float64 `json:"decode_rate"`    // fraction decoded successfully
	Throughput    float64 `json:"throughput_bps"` // effective bytes per second
	PayloadBytes  int     `json:"payload_bytes"`  // bytes per frame

	Duration time.Duration `json:"duration"`
}

// Sweep runs a calibration sweep across parameter combinations
type Sweep struct {
	cfg config.CalibrateConfig
}

func NewSweep(cfg config.CalibrateConfig) *Sweep {
	return &Sweep{cfg: cfg}
}

// Run executes the calibration sweep
func (s *Sweep) Run() ([]Result, error) {
	results := []Result{}

	ecLevels := []config.ECLevel{config.ECLevelL, config.ECLevelM, config.ECLevelH}

	totalCombos := len(s.cfg.SweepVersions) * len(s.cfg.SweepModulePx) *
		len(s.cfg.SweepGray) * len(s.cfg.SweepFPS) * len(ecLevels)

	fmt.Printf("Calibration sweep: %d parameter combinations, %d trials each
",
		totalCombos, s.cfg.TrialsPerCombo)

	combo := 0
	for _, ver := range s.cfg.SweepVersions {
		for _, modPx := range s.cfg.SweepModulePx {
			// Check if QR fits in frame
			modules := 17 + 4*ver
			qrSize := modules * modPx
			if qrSize > config.NTSCHeight-20 { // leave margin
				continue
			}

			for _, gray := range s.cfg.SweepGray {
				for _, fps := range s.cfg.SweepFPS {
					for _, ec := range ecLevels {
						combo++
						fmt.Printf("
[%d/%d] v%d %dpx %dgray %.1ffps EC-%s
",
							combo, totalCombos, ver, modPx, gray, fps, ec)

						result, err := s.runTrial(ver, modPx, gray, fps, ec)
						if err != nil {
							fmt.Printf("  ERROR: %v
", err)
							continue
						}

						results = append(results, result)
						fmt.Printf("  → decode rate: %.1f%%, throughput: %.1f KB/s
",
							result.DecodeRate*100, result.Throughput/1024)
					}
				}
			}
		}
	}

	return results, nil
}

// runTrial tests a specific parameter combination
func (s *Sweep) runTrial(version, modulePx, grayLevels int, fps float64, ec config.ECLevel) (Result, error) {
	encCfg := config.EncoderConfig{
		QRVersion:    version,
		ECLevel:      ec,
		ModulePixels: modulePx,
		GrayLevels:   grayLevels,
		DataFPS:      fps,
		SyncEveryN:   0, // no sync frames for calibration
		FECRatio:     0, // no FEC overhead for raw measurement
		Resolution:   [2]int{config.NTSCWidth, config.NTSCHeight},
	}

	// Generate test data
	payload := config.QRCapacity(version, ec) - config.FrameHeaderSize
	if payload < 1 {
		return Result{}, fmt.Errorf("payload too small: %d bytes", payload)
	}

	testData := make([]byte, payload*s.cfg.TrialsPerCombo)
	for i := range testData {
		testData[i] = byte(i % 256)
	}

	// Encode to temporary file
	tmpFile := fmt.Sprintf("/tmp/vhs-cal-%d-%d-%d-%.0f-%s.avi",
		version, modulePx, grayLevels, fps, ec)
	defer os.Remove(tmpFile)

	encCfg.OutputFile = tmpFile
	enc := encoder.New(encCfg)

	start := time.Now()

	// For file-based calibration (loopback test without hardware)
	if s.cfg.DeviceIn == "" || s.cfg.DeviceOut == "" {
		// Encode to file then decode from file (software loopback)
		err := enc.EncodeToFile(
			&staticReader{data: testData},
			tmpFile,
		)
		if err != nil {
			return Result{}, fmt.Errorf("encode failed: %w", err)
		}

		decCfg := config.DecoderConfig{
			InputFile: tmpFile,
		}
		dec := decoder.New(decCfg)
		_, err = dec.DecodeFromFile(tmpFile)
		duration := time.Since(start)

		stats := dec.Stats()
		decoded := stats["frames_decoded"].(int)
		errors := stats["frames_errors"].(int)
		total := decoded + errors

		decodeRate := 0.0
		if total > 0 {
			decodeRate = float64(decoded) / float64(total)
		}

		return Result{
			QRVersion:     version,
			ModulePixels:  modulePx,
			GrayLevels:    grayLevels,
			DataFPS:       fps,
			ECLevel:       ec.String(),
			FramesSent:    total,
			FramesDecoded: decoded,
			FrameErrors:   errors,
			DecodeRate:    decodeRate,
			Throughput:    float64(decoded*payload) / duration.Seconds(),
			PayloadBytes:  payload,
			Duration:      duration,
		}, err
	}

	// Hardware loopback: encode to output device, capture from input device
	// This requires two USB devices connected in a loop (or through a VCR)
	return Result{}, fmt.Errorf("hardware loopback calibration not yet implemented - use file-based mode")
}

// SaveResults writes calibration results to a JSON file
func SaveResults(results []Result, path string) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()

	enc := json.NewEncoder(f)
	enc.SetIndent("", "  ")
	return enc.Encode(results)
}

// PrintSummary shows a ranked table of results
func PrintSummary(results []Result) {
	if len(results) == 0 {
		fmt.Println("No results to display.")
		return
	}

	// Sort by throughput descending
	for i := 0; i < len(results)-1; i++ {
		for j := i + 1; j < len(results); j++ {
			if results[j].Throughput > results[i].Throughput {
				results[i], results[j] = results[j], results[i]
			}
		}
	}

	fmt.Println("
╔══════════════════════════════════════════════════════════════════════╗")
	fmt.Println("║                    CALIBRATION RESULTS (by throughput)              ║")
	fmt.Println("╠══════╦═══════╦══════╦══════╦════╦═══════════╦═══════════╦══════════╣")
	fmt.Println("║ Rank ║ QR v  ║ ModPx║ Gray ║ EC ║ Decode %  ║  KB/sec   ║ SP 2hr   ║")
	fmt.Println("╠══════╬═══════╬══════╬══════╬════╬═══════════╬═══════════╬══════════╣")

	for i, r := range results {
		if i >= 20 {
			break
		}
		sp2hr := r.Throughput * 7200 / 1024 / 1024
		fmt.Printf("║ %4d ║  %3d  ║  %2d  ║  %2d  ║ %s  ║  %6.1f%%  ║  %7.1f  ║ %5.1f MB ║
",
			i+1, r.QRVersion, r.ModulePixels, r.GrayLevels,
			r.ECLevel, r.DecodeRate*100, r.Throughput/1024, sp2hr)
	}

	fmt.Println("╚══════╩═══════╩══════╩══════╩════╩═══════════╩═══════════╩══════════╝")
}

// staticReader wraps a byte slice as an io.Reader
type staticReader struct {
	data []byte
	pos  int
}

func (r *staticReader) Read(p []byte) (int, error) {
	if r.pos >= len(r.data) {
		return 0, fmt.Errorf("EOF")
	}
	n := copy(p, r.data[r.pos:])
	r.pos += n
	return n, nil
}
