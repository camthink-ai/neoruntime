package handlers

import (
	"errors"
	"strings"
	"testing"
)

func TestHumanizeLoadError(t *testing.T) {
	tests := []struct {
		name string
		err  error
		want string // substring assertions avoid brittle full-text checks
	}{
		{
			name: "nil error yields empty detail",
			err:  nil,
			want: "",
		},
		{
			name: "raw tensor error at registration",
			err:  errors.New(`No tensor with name 'yolov8_nms_postprocess/concat' in model`),
			want: "the model does not match its configured postprocess profile",
		},
		{
			name: "tensor error wrapped by smoke test",
			err:  errors.New(`postprocess smoke test failed: No tensor with name 'yolov5_nms_postprocess/nms' in model`),
			want: "the model does not match its configured postprocess profile",
		},
		{
			name: "case-insensitive signature",
			err:  errors.New(`no tensor with name 'x'`),
			want: "the model does not match its configured postprocess profile",
		},
		{
			name: "unmatched error passes through unchanged",
			err:  errors.New("connection refused"),
			want: "connection refused",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := humanizeLoadError(tt.err)

			switch tt.name {
			case "nil error yields empty detail":
				if got != "" {
					t.Fatalf("humanizeLoadError(nil) = %q, want empty", got)
				}
			case "unmatched error passes through unchanged":
				if got != tt.want {
					t.Fatalf("humanizeLoadError() = %q, want %q", got, tt.want)
				}
			default:
				// The explanation leads; the raw runtime error is demoted
				// but preserved for debugging.
				if !strings.Contains(got, tt.want) {
					t.Fatalf("humanizeLoadError() = %q, want it to contain %q", got, tt.want)
				}
				if !strings.Contains(got, tt.err.Error()) {
					t.Fatalf("humanizeLoadError() = %q, want it to preserve raw error %q", got, tt.err.Error())
				}
				if strings.Index(got, tt.want) > strings.Index(got, tt.err.Error()) {
					t.Fatalf("humanizeLoadError() = %q, want explanation before raw error", got)
				}
			}
		})
	}
}
