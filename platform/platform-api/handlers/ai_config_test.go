package handlers

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/gin-gonic/gin"

	"aipc/platform/platform-api/model"
)

// The query APIs must surface the registration-time config column (labels,
// nms_threshold, ...) so the web detail dialog can render it — while empty
// or corrupt rows simply omit the key instead of breaking the response.
func TestListModelsExposesConfig(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "cfg_det", Name: "cfg_det", Status: "uploaded", Source: "web",
		Config: `{"labels":["fire","smoke"],"nms_threshold":0.45}`,
	})
	seedAIModel(t, h, &model.AIModel{ModelID: "no_cfg", Name: "no_cfg", Status: "uploaded", Source: "web"})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "bad_cfg", Name: "bad_cfg", Status: "uploaded", Source: "web",
		Config: `{"labels":[`,
	})

	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest(http.MethodGet, "/api/v1/ai/models", nil)
	h.ListModels(c)

	var resp struct {
		Code int `json:"code"`
		Data struct {
			Models []struct {
				ModelID string          `json:"model_id"`
				Config  json.RawMessage `json:"config"`
			} `json:"models"`
		} `json:"data"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatalf("decode response %q: %v", w.Body.String(), err)
	}
	if resp.Code != 0 {
		t.Fatalf("code = %d body=%s", resp.Code, w.Body.String())
	}
	byID := map[string]json.RawMessage{}
	for _, m := range resp.Data.Models {
		byID[m.ModelID] = m.Config
	}

	cfg, ok := byID["cfg_det"]
	if !ok || cfg == nil {
		t.Fatalf("valid config must be exposed as an object, body=%s", w.Body.String())
	}
	var parsed struct {
		Labels       []string `json:"labels"`
		NmsThreshold float64  `json:"nms_threshold"`
	}
	if err := json.Unmarshal(cfg, &parsed); err != nil {
		t.Fatalf("config must decode as an object, got %s: %v", cfg, err)
	}
	if len(parsed.Labels) != 2 || parsed.Labels[0] != "fire" || parsed.NmsThreshold != 0.45 {
		t.Errorf("config content mismatch: %+v", parsed)
	}
	for _, id := range []string{"no_cfg", "bad_cfg"} {
		if byID[id] != nil {
			t.Errorf("%s must omit config (empty/corrupt row), got %s", id, byID[id])
		}
	}
}

func TestGetModelInfoExposesConfig(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "cfg_det", Name: "cfg_det", Status: "uploaded", Source: "web",
		Config: `{"labels":["fire"],"nms_threshold":0.5}`,
	})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "bad_cfg", Name: "bad_cfg", Status: "uploaded", Source: "web",
		Config: `not json`,
	})

	get := func(t *testing.T, modelID string) map[string]json.RawMessage {
		t.Helper()
		gin.SetMode(gin.TestMode)
		w := httptest.NewRecorder()
		c, _ := gin.CreateTestContext(w)
		c.Params = gin.Params{{Key: "model_id", Value: modelID}}
		c.Request = httptest.NewRequest(http.MethodGet, "/api/v1/ai/models/"+modelID, nil)
		h.GetModelInfo(c)
		var resp struct {
			Code int                        `json:"code"`
			Data map[string]json.RawMessage `json:"data"`
		}
		if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
			t.Fatalf("decode response %q: %v", w.Body.String(), err)
		}
		if resp.Code != 0 {
			t.Fatalf("code = %d body=%s", resp.Code, w.Body.String())
		}
		return resp.Data
	}

	cfg, ok := get(t, "cfg_det")["config"]
	if !ok || cfg == nil {
		t.Fatal("valid config must be exposed as an object")
	}
	var parsed struct {
		Labels []string `json:"labels"`
	}
	if err := json.Unmarshal(cfg, &parsed); err != nil {
		t.Fatalf("config must decode as an object, got %s: %v", cfg, err)
	}
	if len(parsed.Labels) != 1 || parsed.Labels[0] != "fire" {
		t.Errorf("labels mismatch: %+v", parsed)
	}
	if cfg, ok := get(t, "bad_cfg")["config"]; ok || cfg != nil {
		t.Errorf("corrupt config row must omit the key entirely, got %s", cfg)
	}
}
