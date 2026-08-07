package handlers

import (
	"fmt"
	"math"
	"strings"
)

// StreamValidationRule defines constraints for encoder stream parameters.
type StreamValidationRule struct {
	MinWidth, MaxWidth     uint32
	MinHeight, MaxHeight   uint32
	WidthAlign             uint32 // Width must be a multiple of this
	HeightAlign            uint32 // Height must be a multiple of this
	MinBitrate, MaxBitrate uint32
	MinFps, MaxFps         uint32
	MinGop, MaxGop         uint32
	AllowedCodecs          []string
}

var streamRules = map[string]StreamValidationRule{
	"main": {
		MinWidth: 64, MaxWidth: 4096,
		MinHeight: 64, MaxHeight: 2160,
		WidthAlign: 2, HeightAlign: 2,
		MinBitrate: 256000, MaxBitrate: 20000000,
		MinFps: 1, MaxFps: 60,
		MinGop: 1, MaxGop: 300,
		AllowedCodecs: []string{"h264", "h265"},
	},
	"sub": {
		MinWidth: 64, MaxWidth: 4096,
		MinHeight: 64, MaxHeight: 2160,
		WidthAlign: 2, HeightAlign: 2,
		MinBitrate: 64000, MaxBitrate: 10000000,
		MinFps: 1, MaxFps: 60,
		MinGop: 1, MaxGop: 300,
		AllowedCodecs: []string{"h264", "h265"},
	},
	"third": {
		MinWidth: 64, MaxWidth: 4096,
		MinHeight: 64, MaxHeight: 2160,
		WidthAlign: 2, HeightAlign: 2,
		MinBitrate: 32000, MaxBitrate: 4000000,
		MinFps: 1, MaxFps: 30,
		MinGop: 1, MaxGop: 300,
		AllowedCodecs: []string{"h264"},
	},
}

// defaultRule is used for dynamically added streams with unknown names.
// Conservative: only allow h264 with stricter bitrate/fps limits.
var defaultRule = StreamValidationRule{
	MinWidth: 64, MaxWidth: 4096,
	MinHeight: 64, MaxHeight: 2160,
	WidthAlign: 2, HeightAlign: 2,
	MinBitrate: 32000, MaxBitrate: 10000000,
	MinFps: 1, MaxFps: 30,
	MinGop: 1, MaxGop: 300,
	AllowedCodecs: []string{"h264"},
}

func getRule(streamName string) StreamValidationRule {
	if r, ok := streamRules[streamName]; ok {
		return r
	}
	return defaultRule
}

// RegisterStreamRule adds a validation rule for a dynamically created stream.
func RegisterStreamRule(streamName string, rule StreamValidationRule) {
	streamRules[streamName] = rule
}

// RemoveStreamRule removes a validation rule (e.g. when stream is deleted).
func RemoveStreamRule(streamName string) {
	delete(streamRules, streamName)
}

func isAllowedCodec(codec string, allowed []string) bool {
	lc := strings.ToLower(codec)
	for _, c := range allowed {
		if lc == c {
			return true
		}
	}
	return false
}

// ValidateEncoderHotReload validates parameters for hot-reload (bitrate/fps/gop only).
func ValidateEncoderHotReload(streamName string, bitrateBps, framerate, gop uint32) error {
	r := getRule(streamName)

	if bitrateBps > 0 {
		if bitrateBps < r.MinBitrate {
			return fmt.Errorf("bitrate %d is below minimum %d for stream %s", bitrateBps, r.MinBitrate, streamName)
		}
		if bitrateBps > r.MaxBitrate {
			return fmt.Errorf("bitrate %d exceeds maximum %d for stream %s", bitrateBps, r.MaxBitrate, streamName)
		}
	}

	if framerate > 0 {
		if framerate < r.MinFps {
			return fmt.Errorf("framerate %d is below minimum %d for stream %s", framerate, r.MinFps, streamName)
		}
		if framerate > r.MaxFps {
			return fmt.Errorf("framerate %d exceeds maximum %d for stream %s", framerate, r.MaxFps, streamName)
		}
	}

	if gop > 0 {
		if gop < r.MinGop {
			return fmt.Errorf("gop %d is below minimum %d for stream %s", gop, r.MinGop, streamName)
		}
		if gop > r.MaxGop {
			return fmt.Errorf("gop %d exceeds maximum %d for stream %s", gop, r.MaxGop, streamName)
		}
	}

	return nil
}

// ValidateEncoderReconfig validates parameters for full encoder reconfiguration.
func ValidateEncoderReconfig(streamName string, width, height uint32, codec string, bitrateBps, fps, gop uint32) error {
	r := getRule(streamName)

	if width > 0 {
		if width < r.MinWidth || width > r.MaxWidth {
			return fmt.Errorf("width %d out of range [%d, %d] for stream %s", width, r.MinWidth, r.MaxWidth, streamName)
		}
		if r.WidthAlign > 0 && width%r.WidthAlign != 0 {
			return fmt.Errorf("width %d must be a multiple of %d for stream %s", width, r.WidthAlign, streamName)
		}
	}

	if height > 0 {
		if height < r.MinHeight || height > r.MaxHeight {
			return fmt.Errorf("height %d out of range [%d, %d] for stream %s", height, r.MinHeight, r.MaxHeight, streamName)
		}
		if r.HeightAlign > 0 && height%r.HeightAlign != 0 {
			return fmt.Errorf("height %d must be a multiple of %d for stream %s", height, r.HeightAlign, streamName)
		}
	}

	if codec != "" {
		if !isAllowedCodec(codec, r.AllowedCodecs) {
			return fmt.Errorf("codec %q not supported for stream %s (allowed: %s)", codec, streamName, strings.Join(r.AllowedCodecs, ", "))
		}
	}

	// Also validate hot-reload params if provided
	return ValidateEncoderHotReload(streamName, bitrateBps, fps, gop)
}

// ValidatePipelineReconfig validates a full pipeline reconfiguration request.
func ValidatePipelineReconfig(streams []PipelineStreamInput) error {
	if len(streams) == 0 {
		return fmt.Errorf("at least one stream is required")
	}
	if len(streams) > 4 {
		return fmt.Errorf("maximum 4 streams supported, got %d", len(streams))
	}

	sinkToName := map[string]string{"sink0": "main", "sink1": "sub", "sink2": "third"}

	seen := make(map[string]bool)
	for _, s := range streams {
		if s.StreamID == "" {
			return fmt.Errorf("stream_id is required")
		}
		if seen[s.StreamID] {
			return fmt.Errorf("duplicate stream_id: %s", s.StreamID)
		}
		seen[s.StreamID] = true

		name := s.StreamID
		if mapped, ok := sinkToName[name]; ok {
			name = mapped
		}
		err := ValidateEncoderReconfig(name,
			s.EncoderWidth, s.EncoderHeight, s.Codec,
			s.EncoderBitrate, s.EncoderFramerate, s.EncoderGOP)
		if err != nil {
			return fmt.Errorf("stream %s: %w", s.StreamID, err)
		}
	}

	return nil
}

// ValidateAddStream validates parameters for adding a new stream.
func ValidateAddStream(streamID string, width, height, fps, bitrate, gop uint32, codec string) error {
	if streamID == "" {
		return fmt.Errorf("stream_id is required")
	}

	if width == 0 || height == 0 {
		return fmt.Errorf("width and height are required")
	}

	if codec == "" {
		codec = "h264"
	}

	// Use streamID for rule lookup (may fall back to default)
	return ValidateEncoderReconfig(streamID, width, height, codec, bitrate, fps, gop)
}

// PipelineStreamInput is a Go representation of a pipeline stream config for validation.
type PipelineStreamInput struct {
	StreamID         string
	InputWidth       uint32
	InputHeight      uint32
	InputFramerate   uint32
	Codec            string
	EncoderWidth     uint32
	EncoderHeight    uint32
	EncoderFramerate uint32
	EncoderBitrate   uint32
	EncoderGOP       uint32
}

// ValidateISPUpdate validates ISP parameters for range compliance.
// nil pointers are treated as "no change" and are skipped.
func ValidateISPUpdate(
	brightness, contrast, saturation, sharpness *int32,
	noiseReduction, wdrValue, backlight *int32,
	exposureTimeUs, gain *int32,
	powerlineFreq *int32,
	awbIndex *int32,
) error {
	// 0–100 range fields
	rangedFields := []struct {
		name string
		val  *int32
	}{
		{"brightness", brightness},
		{"contrast", contrast},
		{"saturation", saturation},
		{"sharpness", sharpness},
		{"noise_reduction", noiseReduction},
		{"wdr_value", wdrValue},
		{"backlight", backlight},
	}
	for _, f := range rangedFields {
		if f.val != nil {
			if *f.val < 0 || *f.val > 100 {
				return fmt.Errorf("%s value %d out of range [0, 100]", f.name, *f.val)
			}
		}
	}

	if exposureTimeUs != nil {
		if *exposureTimeUs < 1 || *exposureTimeUs > 1000000 {
			return fmt.Errorf("exposure_time_us value %d out of range [1, 1000000]", *exposureTimeUs)
		}
	}

	if gain != nil {
		if *gain < 0 || *gain > 1000 {
			return fmt.Errorf("gain value %d out of range [0, 1000]", *gain)
		}
	}

	if powerlineFreq != nil {
		allowed := map[int32]bool{0: true, 1: true, 2: true}
		if !allowed[*powerlineFreq] {
			return fmt.Errorf("powerline_freq value %d must be 0 (off), 1 (50Hz), or 2 (60Hz)", *powerlineFreq)
		}
	}

	if awbIndex != nil {
		if *awbIndex < -1 || *awbIndex > 16 {
			return fmt.Errorf("awb_index value %d out of range [-1, 16]", *awbIndex)
		}
	}

	return nil
}

// ValidateTransformUpdate validates transform parameters.
// nil pointers are treated as "no change" and are skipped.
func ValidateTransformUpdate(rotation, flip *uint32) error {
	if rotation != nil {
		allowed := map[uint32]bool{0: true, 1: true, 2: true, 3: true}
		if !allowed[*rotation] {
			return fmt.Errorf("rotation value %d must be 0 (0°), 1 (90°), 2 (180°), or 3 (270°)", *rotation)
		}
	}

	if flip != nil {
		allowed := map[uint32]bool{0: true, 1: true, 2: true, 3: true}
		if !allowed[*flip] {
			return fmt.Errorf("flip value %d must be 0 (none), 1 (horizontal), 2 (vertical), or 3 (both)", *flip)
		}
	}

	return nil
}

// clampUint32 returns value clamped to [min, max].
func clampUint32(val, min, max uint32) uint32 {
	return uint32(math.Max(float64(min), math.Min(float64(max), float64(val))))
}
