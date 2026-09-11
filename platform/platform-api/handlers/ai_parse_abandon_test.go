package handlers

import (
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"

	"aipc/platform/platform-api/model"
)

func postAbandon(t *testing.T, h *APIHandlers, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest(http.MethodPost, "/api/v1/ai/models/parse/abandon", strings.NewReader(body))
	c.Request.Header.Set("Content-Type", "application/json")
	h.AbandonStagedModel(c)
	return w
}

// A staged blob no registered model points at is the wizard-cancel case:
// abandon must remove exactly that blob.
func TestAbandonStagedModelDeletesUnreferencedBlob(t *testing.T) {
	h, _, store := newAIUpdateTestEnv(t)
	hash := strings.Repeat("ab", 32)
	blobPath := seedBlob(t, store, hash)

	w := postAbandon(t, h, `{"file_hash":"`+hash+`","file_path":`+strconv.Quote(blobPath)+`}`)
	if respCode(t, w) != 0 {
		t.Fatalf("abandon failed: %s", w.Body.String())
	}
	if !strings.Contains(w.Body.String(), `"deleted":true`) {
		t.Errorf("expected deleted=true, got %s", w.Body.String())
	}
	if store.Exists(hash, ".hef") {
		t.Error("staged blob must be removed after unreferenced abandon")
	}
}

// Identical uploads dedupe onto one CAS blob: if a registered model already
// references the hash, abandon keeps the blob and reports the reference.
func TestAbandonStagedModelKeepsReferencedBlob(t *testing.T) {
	h, _, store := newAIUpdateTestEnv(t)
	hash := strings.Repeat("ab", 32)
	blobPath := seedBlob(t, store, hash)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "shared_det", Name: "shared_det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: blobPath, FileHash: hash,
	})

	w := postAbandon(t, h, `{"file_hash":"`+hash+`","file_path":`+strconv.Quote(blobPath)+`}`)
	if respCode(t, w) != 0 {
		t.Fatalf("referenced abandon must still succeed: %s", w.Body.String())
	}
	if !strings.Contains(w.Body.String(), `"deleted":false`) {
		t.Errorf("expected deleted=false, got %s", w.Body.String())
	}
	if !strings.Contains(w.Body.String(), `"referenced_by":1`) {
		t.Errorf("expected referenced_by=1, got %s", w.Body.String())
	}
	if !store.Exists(hash, ".hef") {
		t.Error("blob referenced by a registered model must survive abandon")
	}
}

// The path must be exactly this hash's blob — never a neighbor's blob and
// never an arbitrary filesystem location.
func TestAbandonStagedModelRejectsForeignPath(t *testing.T) {
	h, _, store := newAIUpdateTestEnv(t)
	hashA := strings.Repeat("aa", 32)
	hashB := strings.Repeat("bb", 32)
	seedBlob(t, store, hashA)
	pathB := seedBlob(t, store, hashB)

	cases := []struct {
		name string
		path string
	}{
		{"another blobs hash", pathB},
		{"arbitrary system path", "/etc/passwd"},
		{"no extension", store.BlobPath(hashA, "")},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			w := postAbandon(t, h, `{"file_hash":"`+hashA+`","file_path":`+strconv.Quote(tc.path)+`}`)
			if respCode(t, w) != CodeInvalidRequest {
				t.Fatalf("foreign path must fail with %d, got %d body=%s",
					CodeInvalidRequest, respCode(t, w), w.Body.String())
			}
			if !store.Exists(hashA, ".hef") || !store.Exists(hashB, ".hef") {
				t.Error("rejected abandon must not delete either blob")
			}
		})
	}
}

func TestAbandonStagedModelRejectsBadHash(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	cases := []struct {
		name string
		hash string
	}{
		{"short", "abc123"},
		{"uppercase hex", strings.Repeat("AB", 32)},
		{"empty", ""},
		{"non-hex", strings.Repeat("zz", 32)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			w := postAbandon(t, h, `{"file_hash":"`+tc.hash+`","file_path":"/blobs/x.hef"}`)
			if respCode(t, w) != CodeInvalidRequest {
				t.Fatalf("%s must fail with %d, got %d body=%s",
					tc.name, CodeInvalidRequest, respCode(t, w), w.Body.String())
			}
		})
	}
}
