package handlers

import (
	"bytes"
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/constants"
)

func TestFileHandlerDefaultAllowsDataRoot(t *testing.T) {
	h := NewFileHandler(nil, "/data/aipc")

	got, err := h.validatePath("/data")
	if err != nil {
		t.Fatalf("validate /data: %v", err)
	}
	if got != "/data" {
		t.Fatalf("validate /data = %q, want /data", got)
	}

	got, err = h.validatePath("/data/aipc/etc")
	if err != nil {
		t.Fatalf("validate /data/aipc/etc: %v", err)
	}
	if got != "/data/aipc/etc" {
		t.Fatalf("validate /data/aipc/etc = %q, want /data/aipc/etc", got)
	}
}

func TestFileHandlerRejectsPrefixSibling(t *testing.T) {
	h := NewFileHandler([]string{"/data"}, "/data/aipc")

	if _, err := h.validatePath("/datax"); err == nil {
		t.Fatal("validate /datax succeeded, want access denied")
	}
}

func TestGenericDeleteRejectsCanonicalManifest(t *testing.T) {
	old := constants.RootPath()
	root := t.TempDir()
	constants.SetRootPath(root)
	defer constants.SetRootPath(old)
	path := filepath.Join(root, "apps", "manifests", "app", "app.yaml")
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("live"), 0644); err != nil {
		t.Fatal(err)
	}
	h := NewFileHandler([]string{root}, root)

	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest("DELETE", "/api/v1/files?path="+path, nil)
	h.Delete(c)
	if code := patchCode(t, w); code != CodeAccessDenied {
		t.Fatalf("delete body: %s", w.Body.String())
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("canonical manifest deleted: %v", err)
	}

	body, _ := json.Marshal(map[string]any{"paths": []string{path}})
	w = httptest.NewRecorder()
	c, _ = gin.CreateTestContext(w)
	c.Request = httptest.NewRequest("POST", "/api/v1/files/batch-delete", bytes.NewReader(body))
	c.Request.Header.Set("Content-Type", "application/json")
	h.BatchDelete(c)
	if code := patchCode(t, w); code != CodeAccessDenied {
		t.Fatalf("batch delete body: %s", w.Body.String())
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("canonical manifest batch-deleted: %v", err)
	}
}
