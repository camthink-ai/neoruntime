package handlers

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"mime/multipart"
	"net"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
	"gopkg.in/yaml.v3"

	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/common/constants"
)

// guardTestRoot points the platform root at a temp dir. A traversal app ID
// that slipped past the guard would create <tmp>/evil (../../evil resolves
// out of apps/manifests); the tests assert that never happens.
func guardTestRoot(t *testing.T) string {
	t.Helper()
	oldRoot := constants.RootPath()
	root := t.TempDir()
	constants.SetRootPath(root)
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })
	return root
}

// validManifestYAML builds a manifest that parses cleanly, carrying the
// (possibly hostile) app ID — the guard under test runs after parsing.
func validManifestYAML(t *testing.T, id string) []byte {
	t.Helper()
	req := &WizardRequest{
		Metadata: WizardMetadata{ID: id, Name: "Guard Test", Version: "1.0.0"},
		Image:    "docker.io/library/alpine:latest",
	}
	data, err := yaml.Marshal(wizardRequestToManifest(req))
	if err != nil {
		t.Fatalf("marshal manifest: %v", err)
	}
	return data
}

func postMultipart(t *testing.T, url, filename string, content []byte, h func(*gin.Context)) *httptest.ResponseRecorder {
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
	req := httptest.NewRequest("POST", url, &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	c.Request = req
	h(c)
	return w
}

// buildNeoapp packs app.yaml + image.tar into a .neoapp (tar.gz) package.
func buildNeoapp(t *testing.T, appYAML, imageTar []byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	gz := gzip.NewWriter(&buf)
	tw := tar.NewWriter(gz)
	writeMember := func(name string, content []byte) {
		if err := tw.WriteHeader(&tar.Header{Name: name, Mode: 0644, Size: int64(len(content))}); err != nil {
			t.Fatalf("write header %s: %v", name, err)
		}
		if _, err := tw.Write(content); err != nil {
			t.Fatalf("write member %s: %v", name, err)
		}
	}
	writeMember("app.yaml", appYAML)
	writeMember("image.tar", imageTar)
	if err := tw.Close(); err != nil {
		t.Fatalf("close tar: %v", err)
	}
	if err := gz.Close(); err != nil {
		t.Fatalf("close gzip: %v", err)
	}
	return buf.Bytes()
}

// assertNoEscape verifies a rejected traversal upload left nothing behind:
// no directory outside the manifests root, and an empty manifests root.
func assertNoEscape(t *testing.T, root string) {
	t.Helper()
	if _, err := os.Stat(filepath.Join(root, "evil")); !os.IsNotExist(err) {
		t.Errorf("traversal app id must not create %s", filepath.Join(root, "evil"))
	}
	manifests := filepath.Join(root, "apps", "manifests")
	if entries, err := os.ReadDir(manifests); err == nil && len(entries) != 0 {
		t.Errorf("manifests root must stay empty, got %v", entries)
	}
}

func TestRequireSafeAppIDBoundaries(t *testing.T) {
	for _, id := range []string{"app", "com.example.app-2", "a", "A1._-", "0"} {
		if err := requireSafeAppID(id); err != nil {
			t.Errorf("requireSafeAppID(%q) = %v, want nil", id, err)
		}
	}
	for _, id := range []string{"", "..", ".", "../../evil", "/etc", "a/b", `a\b`, ".hidden", "-lead", "sp ace", "app:tag"} {
		if err := requireSafeAppID(id); err == nil {
			t.Errorf("requireSafeAppID(%q) = nil, want rejection", id)
		}
	}
}

func TestUploadManifestStagesWithoutTouchingCanonical(t *testing.T) {
	root := guardTestRoot(t)
	canonical := filepath.Join(root, "apps", "manifests", "app", "app.yaml")
	if err := os.MkdirAll(filepath.Dir(canonical), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(canonical, []byte("live"), 0644); err != nil {
		t.Fatal(err)
	}
	w := postMultipart(t, "/api/v1/apps/upload-manifest", "app.yaml",
		validManifestYAML(t, "app"), func(c *gin.Context) {
			(&APIHandlers{}).UploadManifest(c)
		})
	code, _, data := decodeUploadResponse(t, w)
	if code != CodeSuccess {
		t.Fatalf("body: %s", w.Body.String())
	}
	path, _ := data["path"].(string)
	if _, err := safeStagingManifest(path); err != nil {
		t.Fatalf("response path is not strict staging: %s: %v", path, err)
	}
	got, _ := os.ReadFile(canonical)
	if string(got) != "live" {
		t.Fatalf("upload changed canonical manifest: %q", got)
	}
}

func TestUploadManifestRejectsTraversalAppID(t *testing.T) {
	root := guardTestRoot(t)
	w := postMultipart(t, "/api/v1/apps/upload-manifest", "app.yaml",
		validManifestYAML(t, "../../evil"), func(c *gin.Context) {
			(&APIHandlers{}).UploadManifest(c)
		})
	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest || !strings.Contains(detail, "not usable as a manifest directory name") {
		t.Fatalf("code=%d detail=%q, want safe-app-id guard rejection", code, detail)
	}
	assertNoEscape(t, root)
}

func TestUploadPackageRejectsTraversalAppID(t *testing.T) {
	root := guardTestRoot(t)
	imageTar := buildDockerSaveTar(t, true, []byte("layer-payload"))
	pkg := buildNeoapp(t, validManifestYAML(t, "../../../evil"), imageTar)
	w := postMultipart(t, "/api/v1/apps/upload-package", "evil.neoapp", pkg, func(c *gin.Context) {
		(&APIHandlers{}).UploadPackage(c)
	})
	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest || !strings.Contains(detail, "not usable as a manifest directory name") {
		t.Fatalf("code=%d detail=%q, want safe-app-id guard rejection", code, detail)
	}
	assertNoEscape(t, root)
}

func TestWizardInstallRejectsTraversalAppID(t *testing.T) {
	root := guardTestRoot(t)

	// WizardInstall gates on a live AppManager client before anything else;
	// an Unimplemented bufconn server is enough — the guard fires long
	// before any RPC.
	lis := bufconn.Listen(1024 * 1024)
	srv := grpc.NewServer()
	apppb.RegisterAppManagerServer(srv, &fakeAppLister{})
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	conn, err := grpc.DialContext(context.Background(), "bufnet",
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return lis.Dial() }),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("dial app-manager bufnet: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	h := &APIHandlers{grpcClients: &GRPCClients{AppManager: conn}}

	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest("POST", "/api/v1/apps/wizard-install",
		strings.NewReader(`{"metadata":{"id":"../../evil","name":"x"},"image":"docker.io/library/alpine:latest"}`))
	c.Request.Header.Set("Content-Type", "application/json")
	h.WizardInstall(c)
	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest || !strings.Contains(detail, "not usable as a manifest directory name") {
		t.Fatalf("code=%d detail=%q, want safe-app-id guard rejection", code, detail)
	}
	assertNoEscape(t, root)
}

func TestUploadTokenUnique(t *testing.T) {
	seen := make(map[string]bool, 256)
	for range 256 {
		tok := uploadToken()
		if seen[tok] {
			t.Fatalf("uploadToken repeated %q — concurrent uploads would truncate each other", tok)
		}
		seen[tok] = true
	}
}
