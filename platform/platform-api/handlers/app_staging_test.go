package handlers

import (
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/constants"
)

func stagingTestRoot(t *testing.T) string {
	t.Helper()
	old := constants.RootPath()
	root := t.TempDir()
	constants.SetRootPath(root)
	t.Cleanup(func() { constants.SetRootPath(old) })
	return root
}

func TestSafeStagingManifestStrict(t *testing.T) {
	root := stagingTestRoot(t)
	dir := filepath.Join(root, "apps", "staging", "1700000000_deadbeef")
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "app.yaml")
	if err := os.WriteFile(path, []byte("x"), 0644); err != nil {
		t.Fatal(err)
	}
	if got, err := safeStagingManifest(path); err != nil || got != path {
		t.Fatalf("safeStagingManifest = %q, %v", got, err)
	}
	for _, bad := range []string{
		filepath.Join(root, "apps", "manifests", "app", "app.yaml"),
		filepath.Join(root, "apps", "staging", "1700000000_deadbeef", "nested", "app.yaml"),
		filepath.Join(root, "apps", "staging", "not-a-token", "app.yaml"),
	} {
		if _, err := safeStagingManifest(bad); err == nil {
			t.Errorf("accepted %s", bad)
		}
	}
}

func TestAbandonAppStagingRemovesWholeTokenOnly(t *testing.T) {
	root := stagingTestRoot(t)
	dir := filepath.Join(root, "apps", "staging", "1700000000_deadbeef")
	manifest := filepath.Join(dir, "app.yaml")
	image := filepath.Join(dir, "image.tar")
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	_ = os.WriteFile(manifest, []byte("x"), 0644)
	_ = os.WriteFile(image, []byte("x"), 0644)

	body, _ := json.Marshal(map[string]any{"paths": []string{manifest, image}})
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest("POST", "/api/v1/apps/staging/abandon", strings.NewReader(string(body)))
	c.Request.Header.Set("Content-Type", "application/json")
	(&APIHandlers{}).AbandonAppStaging(c)
	if code := patchCode(t, w); code != CodeSuccess {
		t.Fatalf("body: %s", w.Body.String())
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("staging token still exists: %v", err)
	}
}

func TestCleanupAppStagingSkipsSymlinksAndUnsafeNames(t *testing.T) {
	root := stagingTestRoot(t)
	staging := filepath.Join(root, "apps", "staging")
	expired := filepath.Join(staging, "1700000000_deadbeef")
	unsafe := filepath.Join(staging, "keep-me")
	outside := filepath.Join(root, "outside")
	link := filepath.Join(staging, "1700000001_cafebabe")
	for _, dir := range []string{expired, unsafe, outside} {
		if err := os.MkdirAll(dir, 0755); err != nil {
			t.Fatal(err)
		}
	}
	old := time.Now().Add(-appStagingTTL - time.Hour)
	if err := os.Chtimes(expired, old, old); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, link); err != nil {
		t.Fatal(err)
	}

	CleanupAppStaging(time.Now())
	if _, err := os.Stat(expired); !os.IsNotExist(err) {
		t.Fatalf("expired safe token survived: %v", err)
	}
	for _, path := range []string{unsafe, outside, link} {
		if _, err := os.Lstat(path); err != nil {
			t.Errorf("protected path %s removed: %v", path, err)
		}
	}
}
