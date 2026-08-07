package handlers

import (
	"context"
	"testing"

	"github.com/gin-gonic/gin"
)

// aiOverlayBaseline is the full ai_overlay section including the 5 yaml-only
// keys (event_bus_endpoint, topic_prefix, draw_landmarks, enable_face_blur,
// stream_map) that the gRPC overlay request does NOT carry and must survive.
const aiOverlayBaseline = `ai_overlay:
  enabled: true
  event_bus_endpoint: "unix:///run/aipc/event-bus.sock"
  topic_prefix: "inference/"
  draw_labels: false
  draw_confidence: false
  draw_landmarks: true
  enable_face_blur: false
  box_thickness: 2
  stream_map: "third:main,sub:main"
`

// TestWriteAiOverlayConfig_TranslatesFieldsAndPreservesYamlOnlyKeys asserts the
// proto→yaml field-name translation (show_label→draw_labels,
// show_confidence→draw_confidence, line_thickness→box_thickness) and that the
// five yaml-only keys are preserved by the read-modify-write.
func TestWriteAiOverlayConfig_TranslatesFieldsAndPreservesYamlOnlyKeys(t *testing.T) {
	gin.SetMode(gin.TestMode)
	path := writeTempYaml(t, aiOverlayBaseline)
	h := &MediaHandlers{configPath: path}

	h.writeAiOverlayConfig(context.Background(), "bob",
		false /*enabled*/, true /*showLabel*/, false /*showConfidence*/, 5 /*lineThickness*/)

	ov := readYamlMap(t, path)["ai_overlay"].(map[string]interface{})
	// Translated fields.
	if ov["enabled"] != false {
		t.Errorf("enabled = %v, want false", ov["enabled"])
	}
	if ov["draw_labels"] != true {
		t.Errorf("draw_labels = %v, want true (translated from show_label)", ov["draw_labels"])
	}
	if ov["draw_confidence"] != false {
		t.Errorf("draw_confidence = %v, want false (translated from show_confidence)", ov["draw_confidence"])
	}
	if ov["box_thickness"] != 5 {
		t.Errorf("box_thickness = %v, want 5 (translated from line_thickness)", ov["box_thickness"])
	}
	// yaml-only keys preserved.
	for _, k := range []string{"event_bus_endpoint", "topic_prefix", "draw_landmarks", "enable_face_blur", "stream_map"} {
		if _, ok := ov[k]; !ok {
			t.Errorf("yaml-only key %q was dropped by writeback", k)
		}
	}
	if ov["event_bus_endpoint"] != "unix:///run/aipc/event-bus.sock" {
		t.Errorf("event_bus_endpoint = %v, want preserved value", ov["event_bus_endpoint"])
	}
}

// TestWriteAiOverlayConfig_CreatesSectionIfMissing asserts a missing ai_overlay
// section is created with the translated fields rather than no-oping.
func TestWriteAiOverlayConfig_CreatesSectionIfMissing(t *testing.T) {
	gin.SetMode(gin.TestMode)
	path := writeTempYaml(t, `rtsp:
  enabled: true
`)
	h := &MediaHandlers{configPath: path}

	h.writeAiOverlayConfig(context.Background(), "", true, true, true, 3)

	ov, ok := readYamlMap(t, path)["ai_overlay"].(map[string]interface{})
	if !ok {
		t.Fatal("ai_overlay section should be created")
	}
	if ov["enabled"] != true || ov["draw_labels"] != true || ov["box_thickness"] != 3 {
		t.Fatalf("created ai_overlay fields wrong: %+v", ov)
	}
}
