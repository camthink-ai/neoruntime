package handlers

import "testing"

// TestCanonicalEncoderDims verifies the YAML encoder-dim persistence guard that
// prevents the rotation-residual black screen: portrait-transposed (H>W) pairs
// are swapped back to landscape before being written to camera-daemon.yaml, so a
// stale portrait dim set never survives into the next boot.
func TestCanonicalEncoderDims(t *testing.T) {
	tests := []struct {
		name string
		// Arrange
		width  uint32
		height uint32
		// Assert
		wantW uint32
		wantH uint32
	}{
		{name: "main landscape passthrough", width: 3840, height: 2160, wantW: 3840, wantH: 2160},
		{name: "sub landscape passthrough", width: 640, height: 480, wantW: 640, wantH: 480},
		{name: "third landscape passthrough", width: 640, height: 384, wantW: 640, wantH: 384},
		{name: "main portrait-transposed swapped back", width: 2160, height: 3840, wantW: 3840, wantH: 2160},
		{name: "sub portrait-transposed swapped back", width: 480, height: 640, wantW: 640, wantH: 480},
		{name: "third portrait-transposed swapped back", width: 384, height: 640, wantW: 640, wantH: 384},
		{name: "square dims left alone", width: 512, height: 512, wantW: 512, wantH: 512},
		{name: "zero dims passthrough", width: 0, height: 0, wantW: 0, wantH: 0},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			// Act
			gotW, gotH := canonicalEncoderDims(tt.width, tt.height)
			// Assert
			if gotW != tt.wantW || gotH != tt.wantH {
				t.Errorf("canonicalEncoderDims(width=%d, height=%d) = (%d, %d), want (%d, %d)",
					tt.width, tt.height, gotW, gotH, tt.wantW, tt.wantH)
			}
		})
	}
}
