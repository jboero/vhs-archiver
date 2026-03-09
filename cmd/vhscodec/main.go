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

package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/spf13/cobra"

	"github.com/johnny/vhs-codec/pkg/audio"
	"github.com/johnny/vhs-codec/pkg/calibrate"
	"github.com/johnny/vhs-codec/pkg/config"
	"github.com/johnny/vhs-codec/pkg/decoder"
	"github.com/johnny/vhs-codec/pkg/encoder"
)

func main() {
	rootCmd := &cobra.Command{
		Use:   "vhs-codec",
		Short: "VHS-Codec: Digital data storage on VHS tape via QR codes",
		Long: `VHS-Codec encodes arbitrary files into QR code video frames
for recording onto VHS tape, and decodes them back from playback.

Uses composite video (and optionally hi-fi audio) channels for data transfer.`,
	}

	// --- encode command ---
	var encCfg config.EncoderConfig
	var ecLevelStr string

	encodeCmd := &cobra.Command{
		Use:   "encode",
		Short: "Encode a file into QR code video for VHS recording",
		RunE: func(cmd *cobra.Command, args []string) error {
			ec, err := config.ParseECLevel(ecLevelStr)
			if err != nil {
				return err
			}
			encCfg.ECLevel = ec

			if encCfg.InputFile == "" {
				return fmt.Errorf("--input is required")
			}

			f, err := os.Open(encCfg.InputFile)
			if err != nil {
				return fmt.Errorf("opening input: %w", err)
			}
			defer f.Close()

			enc := encoder.New(encCfg)

			if encCfg.OutputFile != "" {
				fmt.Printf("Encoding %s → %s
", encCfg.InputFile, encCfg.OutputFile)
				return enc.EncodeToFile(f, encCfg.OutputFile)
			}

			if encCfg.VideoDevice != "" {
				fmt.Printf("Encoding %s → device %s
", encCfg.InputFile, encCfg.VideoDevice)
				return enc.EncodeToDevice(f)
			}

			return fmt.Errorf("specify --output (file) or --device (V4L2 device)")
		},
	}

	defaults := config.DefaultEncoderConfig()
	encCfg = defaults // start from defaults, flags override individual fields
	encodeCmd.Flags().StringVarP(&encCfg.InputFile, "input", "i", "", "Input file to encode")
	encodeCmd.Flags().StringVarP(&encCfg.OutputFile, "output", "o", "", "Output video file")
	encodeCmd.Flags().StringVarP(&encCfg.VideoDevice, "device", "d", "", "V4L2 output device (/dev/videoN)")
	encodeCmd.Flags().IntVar(&encCfg.QRVersion, "qr-version", defaults.QRVersion, "QR code version (1-40)")
	encodeCmd.Flags().StringVar(&ecLevelStr, "ec-level", "M", "Error correction level (L/M/Q/H)")
	encodeCmd.Flags().IntVar(&encCfg.ModulePixels, "module-px", defaults.ModulePixels, "Pixels per QR module")
	encodeCmd.Flags().IntVar(&encCfg.GrayLevels, "gray-levels", defaults.GrayLevels, "Gray levels (2=B&W, 4, 8)")
	encodeCmd.Flags().Float64Var(&encCfg.DataFPS, "fps", defaults.DataFPS, "Data frames per second")
	encodeCmd.Flags().IntVar(&encCfg.SyncEveryN, "sync-every", defaults.SyncEveryN, "Sync frame interval")
	encodeCmd.Flags().Float64Var(&encCfg.FECRatio, "fec-ratio", defaults.FECRatio, "FEC redundancy ratio (0.0-1.0)")
	encodeCmd.Flags().BoolVar(&encCfg.AudioEnabled, "audio", false, "Enable audio data channel")
	encodeCmd.Flags().StringVar(&encCfg.AudioDevice, "audio-device", "", "ALSA audio output device")
	encodeCmd.Flags().IntVar(&encCfg.AudioBitrate, "audio-bitrate", defaults.AudioBitrate, "Audio sample rate")

	// --- decode command ---
	var decCfg config.DecoderConfig
	var decTimeout float64

	decodeCmd := &cobra.Command{
		Use:   "decode",
		Short: "Decode data from VHS playback video",
		RunE: func(cmd *cobra.Command, args []string) error {
			if decCfg.InputFile == "" && decCfg.VideoDevice == "" {
				return fmt.Errorf("specify --file or --device")
			}

			dec := decoder.New(decCfg)

			var data []byte
			var err error

			if decCfg.InputFile != "" {
				data, err = dec.DecodeFromFile(decCfg.InputFile)
			} else {
				data, err = dec.DecodeFromDevice()
			}

			if err != nil {
				return fmt.Errorf("decode failed: %w", err)
			}

			if decCfg.OutputFile == "" {
				decCfg.OutputFile = "restored_output"
			}

			if err := os.WriteFile(decCfg.OutputFile, data, 0644); err != nil {
				return fmt.Errorf("writing output: %w", err)
			}

			fmt.Printf("Restored %d bytes → %s
", len(data), decCfg.OutputFile)
			return nil
		},
	}

	decodeCmd.Flags().StringVarP(&decCfg.InputFile, "file", "f", "", "Input video file to decode")
	decodeCmd.Flags().StringVarP(&decCfg.VideoDevice, "device", "d", "", "V4L2 capture device")
	decodeCmd.Flags().StringVarP(&decCfg.OutputFile, "output", "o", "", "Output file path")
	decodeCmd.Flags().StringVar(&decCfg.AudioDevice, "audio-device", "", "ALSA audio input device")
	decodeCmd.Flags().BoolVar(&decCfg.AudioEnabled, "audio", false, "Enable audio data decoding")
	decodeCmd.Flags().Float64Var(&decTimeout, "timeout", 0, "Capture timeout in seconds (0=unlimited)")

	// --- calibrate command ---
	var calCfg config.CalibrateConfig
	var calVersions, calGray, calFPS, calModPx string
	var calOutput string

	calibrateCmd := &cobra.Command{
		Use:   "calibrate",
		Short: "Run calibration sweep to find optimal settings",
		RunE: func(cmd *cobra.Command, args []string) error {
			calCfg.SweepVersions = parseIntList(calVersions)
			calCfg.SweepGray = parseIntList(calGray)
			calCfg.SweepFPS = parseFloatList(calFPS)
			calCfg.SweepModulePx = parseIntList(calModPx)

			if calCfg.TrialsPerCombo < 1 {
				calCfg.TrialsPerCombo = 10
			}

			sweep := calibrate.NewSweep(calCfg)
			results, err := sweep.Run()
			if err != nil {
				return err
			}

			calibrate.PrintSummary(results)

			if calOutput != "" {
				if err := calibrate.SaveResults(results, calOutput); err != nil {
					return fmt.Errorf("saving results: %w", err)
				}
				fmt.Printf("Results saved to %s
", calOutput)
			}

			return nil
		},
	}

	calDefaults := config.DefaultCalibrateConfig()
	calibrateCmd.Flags().StringVar(&calCfg.DeviceIn, "device-in", "", "V4L2 input device")
	calibrateCmd.Flags().StringVar(&calCfg.DeviceOut, "device-out", "", "V4L2 output device")
	calibrateCmd.Flags().StringVar(&calVersions, "sweep-versions", intSliceToStr(calDefaults.SweepVersions), "QR versions to test")
	calibrateCmd.Flags().StringVar(&calGray, "sweep-gray", intSliceToStr(calDefaults.SweepGray), "Gray levels to test")
	calibrateCmd.Flags().StringVar(&calFPS, "sweep-fps", floatSliceToStr(calDefaults.SweepFPS), "FPS values to test")
	calibrateCmd.Flags().StringVar(&calModPx, "sweep-module-px", intSliceToStr(calDefaults.SweepModulePx), "Module pixel sizes to test")
	calibrateCmd.Flags().IntVar(&calCfg.TrialsPerCombo, "trials", 10, "Frames per combination")
	calibrateCmd.Flags().StringVarP(&calOutput, "output", "o", "", "Save results to JSON file")

	// --- estimate command ---
	var estCfg config.EncoderConfig
	var estEC string

	estimateCmd := &cobra.Command{
		Use:   "estimate",
		Short: "Estimate throughput and capacity for given settings",
		RunE: func(cmd *cobra.Command, args []string) error {
			ec, err := config.ParseECLevel(estEC)
			if err != nil {
				return err
			}
			estCfg.ECLevel = ec
			estCfg.Resolution = [2]int{config.NTSCWidth, config.NTSCHeight}

			est := config.EstimateThroughput(estCfg)

			fmt.Println("╔══════════════════════════════════════════════════════╗")
			fmt.Println("║            VHS-CODEC CAPACITY ESTIMATE              ║")
			fmt.Println("╠══════════════════════════════════════════════════════╣")
			fmt.Printf("║ QR Version:    %-5d  EC Level:    %-3s              ║
",
				estCfg.QRVersion, estCfg.ECLevel)
			fmt.Printf("║ Module Pixels: %-5d  Gray Levels: %-3d              ║
",
				estCfg.ModulePixels, estCfg.GrayLevels)
			fmt.Printf("║ Data FPS:      %-5.1f  FEC Ratio:   %-5.2f           ║
",
				estCfg.DataFPS, estCfg.FECRatio)
			fmt.Printf("║ Audio Channel: %-5v                                 ║
",
				estCfg.AudioEnabled)
			fmt.Println("╠══════════════════════════════════════════════════════╣")
			fmt.Printf("║ Payload/frame: %d bytes                         ║
",
				est.PayloadPerFrame)
			fmt.Printf("║ Video:  %8.1f KB/s                               ║
",
				est.VideoBytesSec/1024)
			fmt.Printf("║ Audio:  %8.1f KB/s                               ║
",
				est.AudioBytesSec/1024)
			fmt.Printf("║ Total:  %8.1f KB/s                               ║
",
				est.TotalBytesSec/1024)
			fmt.Println("╠══════════════════════════════════════════════════════╣")
			fmt.Printf("║ SP  (2 hr):  %8.1f MB                            ║
",
				est.CapacitySP2Hr/1024/1024)
			fmt.Printf("║ LP  (4 hr):  %8.1f MB                            ║
",
				est.CapacityLP4Hr/1024/1024)
			fmt.Printf("║ EP  (6 hr):  %8.1f MB                            ║
",
				est.CapacityEP6Hr/1024/1024)
			fmt.Println("╚══════════════════════════════════════════════════════╝")

			return nil
		},
	}

	estimateCmd.Flags().IntVar(&estCfg.QRVersion, "qr-version", defaults.QRVersion, "QR code version")
	estimateCmd.Flags().StringVar(&estEC, "ec-level", "M", "Error correction level")
	estimateCmd.Flags().IntVar(&estCfg.ModulePixels, "module-px", defaults.ModulePixels, "Pixels per QR module")
	estimateCmd.Flags().IntVar(&estCfg.GrayLevels, "gray-levels", defaults.GrayLevels, "Gray levels")
	estimateCmd.Flags().Float64Var(&estCfg.DataFPS, "fps", defaults.DataFPS, "Data FPS")
	estimateCmd.Flags().Float64Var(&estCfg.FECRatio, "fec-ratio", defaults.FECRatio, "FEC ratio")
	estimateCmd.Flags().IntVar(&estCfg.SyncEveryN, "sync-every", defaults.SyncEveryN, "Sync interval")
	estimateCmd.Flags().BoolVar(&estCfg.AudioEnabled, "audio", false, "Include audio channel")
	estimateCmd.Flags().IntVar(&estCfg.AudioBaudRate, "audio-baud", defaults.AudioBaudRate, "Audio baud rate")

	// --- devices command ---
	devicesCmd := &cobra.Command{
		Use:   "devices",
		Short: "List available V4L2 video and ALSA audio devices",
		RunE: func(cmd *cobra.Command, args []string) error {
			fmt.Println("Video devices (V4L2):")
			fmt.Println("  Run: v4l2-ctl --list-devices")
			fmt.Println()
			fmt.Println("Audio devices (ALSA):")
			fmt.Println("  Run: arecord -l  (capture)")
			fmt.Println("  Run: aplay -l    (playback)")
			fmt.Println()
			fmt.Println("For USB composite adapters, look for entries like:")
			fmt.Println("  'usbtv' or 'AV to USB' in the device listings")
			return nil
		},
	}

	// --- audio test command ---
	var audioTestInput, audioTestOutput string
	audioTestCmd := &cobra.Command{
		Use:   "audio-test",
		Short: "Test audio FSK encoding/decoding",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg := audio.DefaultFSKConfig()
			enc := audio.NewEncoder(cfg)

			testData := []byte("Hello VHS! This is a test of the audio data channel. " +
				"If you can read this, FSK modulation is working correctly.")

			wavFile := audioTestOutput
			if wavFile == "" {
				wavFile = "audio_test.wav"
			}

			fmt.Printf("Encoding %d bytes to %s via FSK...
", len(testData), wavFile)
			if err := enc.EncodeToWAV(testData, wavFile); err != nil {
				return err
			}

			fmt.Printf("Audio test file written: %s
", wavFile)
			fmt.Println("Play with: aplay", wavFile)
			fmt.Println("Or pipe to composite audio out for VHS recording")

			return nil
		},
	}

	audioTestCmd.Flags().StringVar(&audioTestInput, "input", "", "Test data file")
	audioTestCmd.Flags().StringVar(&audioTestOutput, "output", "audio_test.wav", "Output WAV file")

	rootCmd.AddCommand(encodeCmd, decodeCmd, calibrateCmd, estimateCmd, devicesCmd, audioTestCmd)

	if err := rootCmd.Execute(); err != nil {
		os.Exit(1)
	}
}

// --- helpers ---

func parseIntList(s string) []int {
	parts := strings.Split(s, ",")
	result := make([]int, 0, len(parts))
	for _, p := range parts {
		v, err := strconv.Atoi(strings.TrimSpace(p))
		if err == nil {
			result = append(result, v)
		}
	}
	return result
}

func parseFloatList(s string) []float64 {
	parts := strings.Split(s, ",")
	result := make([]float64, 0, len(parts))
	for _, p := range parts {
		v, err := strconv.ParseFloat(strings.TrimSpace(p), 64)
		if err == nil {
			result = append(result, v)
		}
	}
	return result
}

func intSliceToStr(v []int) string {
	parts := make([]string, len(v))
	for i, n := range v {
		parts[i] = strconv.Itoa(n)
	}
	return strings.Join(parts, ",")
}

func floatSliceToStr(v []float64) string {
	parts := make([]string, len(v))
	for i, n := range v {
		parts[i] = fmt.Sprintf("%.2f", n)
	}
	return strings.Join(parts, ",")
}
