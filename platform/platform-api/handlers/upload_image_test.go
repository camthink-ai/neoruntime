package handlers

import (
	"archive/tar"
	"bytes"
	"encoding/json"
	"mime/multipart"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/constants"
)

// buildDockerSaveTar builds a synthetic image archive. When layersByPath is
// true the manifest references archive members ("layer.tar" — the legacy
// docker-save layout the importer accepts); when false it references digests
// (OCI style, rejected by both the pre-check and containerd).
func buildDockerSaveTar(t *testing.T, layersByPath bool, payload []byte) []byte {
	t.Helper()
	layerName := "layer.tar"
	manifest := []map[string]any{{
		"Config":   "cfg.json",
		"RepoTags": []string{"e2e/upload-test:1.0.0"},
		"Layers":   []string{layerName},
	}}
	if !layersByPath {
		manifest[0]["Layers"] = []string{"sha256:deadbeef"}
	}

	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	writeMember := func(name string, content []byte) {
		if err := tw.WriteHeader(&tar.Header{Name: name, Mode: 0644, Size: int64(len(content))}); err != nil {
			t.Fatalf("write header %s: %v", name, err)
		}
		if _, err := tw.Write(content); err != nil {
			t.Fatalf("write member %s: %v", name, err)
		}
	}
	writeMember(layerName, payload)
	writeMember("cfg.json", []byte(`{"architecture":"arm64","os":"linux"}`))
	manifestBytes, err := json.Marshal(manifest)
	if err != nil {
		t.Fatalf("marshal manifest: %v", err)
	}
	writeMember("manifest.json", manifestBytes)
	if err := tw.Close(); err != nil {
		t.Fatalf("close tar: %v", err)
	}
	return buf.Bytes()
}

// callUploadImage issues a multipart upload-image request against a test
// context and returns the recorder.
func callUploadImage(t *testing.T, filename string, content []byte) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	fw, err := mw.CreateFormFile("file", filename)
	if err != nil {
		t.Fatalf("create form file: %v", err)
	}
	if _, err := fw.Write(content); err != nil {
		t.Fatalf("write form file: %v", err)
	}
	if err := mw.Close(); err != nil {
		t.Fatalf("close multipart writer: %v", err)
	}

	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	req := httptest.NewRequest("POST", "/api/v1/apps/upload-image", &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	c.Request = req

	(&APIHandlers{}).UploadImage(c)
	return w
}

func decodeUploadResponse(t *testing.T, w *httptest.ResponseRecorder) (int, string, map[string]any) {
	t.Helper()
	var resp struct {
		Code  int            `json:"code"`
		Error *ErrorDetail   `json:"error"`
		Data  map[string]any `json:"data"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatalf("decode response %q: %v", w.Body.String(), err)
	}
	detail := ""
	if resp.Error != nil {
		detail = resp.Error.Detail
	}
	return resp.Code, detail, resp.Data
}

// uploadImageTestRoot redirects the platform root to a temp dir and returns
// the app staging root plus a restore func.
func uploadImageTestRoot(t *testing.T) (string, func()) {
	t.Helper()
	oldRoot := constants.RootPath()
	root := t.TempDir()
	constants.SetRootPath(root)
	staging := filepath.Join(root, "apps", "staging")
	if err := os.MkdirAll(staging, 0755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	return staging, func() { constants.SetRootPath(oldRoot) }
}

func tarResidue(stagingRoot string) []string {
	matches, _ := filepath.Glob(filepath.Join(stagingRoot, "*", "*.tar"))
	return matches
}

func TestUploadImageValidTarSucceeds(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	content := buildDockerSaveTar(t, true, []byte("layer-payload"))
	w := callUploadImage(t, "valid.tar", content)

	code, _, data := decodeUploadResponse(t, w)
	if code != CodeSuccess {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeSuccess, w.Body.String())
	}
	if got, _ := data["image"].(string); got != "e2e/upload-test:1.0.0" {
		t.Errorf("image = %q, want e2e/upload-test:1.0.0; body: %s", got, w.Body.String())
	}
	if residue := tarResidue(imagesDir); len(residue) != 1 {
		t.Errorf("saved files = %v, want exactly 1", residue)
	}
}

func TestUploadImageInvalidTarRejectedAndDeleted(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	content := buildDockerSaveTar(t, false, []byte("layer-payload"))
	w := callUploadImage(t, "digest-ref.tar", content)

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "Invalid image tar:") {
		t.Errorf("error detail = %q, want it to start with 'Invalid image tar:'", detail)
	}
	if residue := tarResidue(imagesDir); len(residue) != 0 {
		t.Errorf("invalid upload left residue on disk: %v", residue)
	}
}

func TestUploadImageOverLimitRejected(t *testing.T) {
	_, restore := uploadImageTestRoot(t)
	defer restore()

	oldLimit := maxImageUploadBytes
	maxImageUploadBytes = 1024
	defer func() { maxImageUploadBytes = oldLimit }()

	// Valid layout, but larger than the (shrunken) limit.
	content := buildDockerSaveTar(t, true, bytes.Repeat([]byte("x"), 4096))
	w := callUploadImage(t, "big.tar", content)

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "maximum allowed size") {
		t.Errorf("error detail = %q, want it to mention the size limit", detail)
	}
}
