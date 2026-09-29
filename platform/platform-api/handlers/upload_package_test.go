package handlers

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"mime/multipart"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
)

// buildNrtPackage builds a .neoapp package (tar.gz) with the given entries laid
// out under a single top-level directory, mirroring the layout produced by
// the apps-repo build scripts (<app>-<version>-<arch>/app.yaml + image.tar).
func buildNrtPackage(t *testing.T, topDir string, entries map[string][]byte) []byte {
	t.Helper()
	var tarBuf bytes.Buffer
	tw := tar.NewWriter(&tarBuf)
	// Directory entry first, like tar -c does for the staging dir.
	if err := tw.WriteHeader(&tar.Header{Name: topDir + "/", Mode: 0755, Typeflag: tar.TypeDir}); err != nil {
		t.Fatalf("write dir header: %v", err)
	}
	for name, content := range entries {
		full := topDir + "/" + name
		if err := tw.WriteHeader(&tar.Header{Name: full, Mode: 0644, Size: int64(len(content))}); err != nil {
			t.Fatalf("write header %s: %v", full, err)
		}
		if _, err := tw.Write(content); err != nil {
			t.Fatalf("write member %s: %v", full, err)
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatalf("close tar: %v", err)
	}

	var gzBuf bytes.Buffer
	gw := gzip.NewWriter(&gzBuf)
	if _, err := gw.Write(tarBuf.Bytes()); err != nil {
		t.Fatalf("gzip tar: %v", err)
	}
	if err := gw.Close(); err != nil {
		t.Fatalf("close gzip: %v", err)
	}
	return gzBuf.Bytes()
}

// validPackageManifest returns a minimal manifest matching the synthetic
// image tar from buildDockerSaveTar.
func validPackageManifest() []byte {
	return []byte("apiVersion: v1\n" +
		"kind: Application\n" +
		"metadata:\n" +
		"  id: e2e-package-app\n" +
		"  name: E2E Package App\n" +
		"  version: 1.0.0\n" +
		"spec:\n" +
		"  image: e2e/upload-test:1.0.0\n")
}

// callUploadPackage issues a multipart upload-package request against a test
// context and returns the recorder.
func callUploadPackage(t *testing.T, filename string, content []byte) *httptest.ResponseRecorder {
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
	req := httptest.NewRequest("POST", "/api/v1/apps/upload-package", &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	c.Request = req

	(&APIHandlers{}).UploadPackage(c)
	return w
}

// imagesDirEntries lists everything left in the images dir after a request.
func imagesDirEntries(t *testing.T, imagesDir string) []string {
	t.Helper()
	matches, err := filepath.Glob(filepath.Join(imagesDir, "*", "*"))
	if err != nil {
		t.Fatalf("glob images dir: %v", err)
	}
	return matches
}

func TestUploadPackageValidSucceeds(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	appYAML := validPackageManifest()
	imageTar := buildDockerSaveTar(t, true, []byte("layer-payload"))
	pkg := buildNrtPackage(t, "e2e-package-app-1.0.0-arm64", map[string][]byte{
		"app.yaml":   appYAML,
		"image.tar":  imageTar,
		"SHA256SUMS": []byte("placeholder-checksums\n"),
	})
	w := callUploadPackage(t, "e2e-package-app-1.0.0-arm64.neoapp", pkg)

	code, _, data := decodeUploadResponse(t, w)
	if code != CodeSuccess {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeSuccess, w.Body.String())
	}

	// Manifest half mirrors upload-manifest.
	manifestPath, _ := data["path"].(string)
	if manifestPath == "" {
		t.Fatalf("data.path is empty; body: %s", w.Body.String())
	}
	saved, err := os.ReadFile(manifestPath)
	if err != nil {
		t.Fatalf("read saved manifest %s: %v", manifestPath, err)
	}
	if !bytes.Equal(saved, appYAML) {
		t.Errorf("saved manifest differs from package app.yaml:\n%s\nvs\n%s", saved, appYAML)
	}
	metadata, _ := data["metadata"].(map[string]any)
	if got, _ := metadata["id"].(string); got != "e2e-package-app" {
		t.Errorf("metadata.id = %v, want e2e-package-app", metadata["id"])
	}
	if got, _ := data["multi_container"].(bool); got {
		t.Errorf("multi_container = true, want false")
	}
	// Original yaml text must ride along for the web YAML editor baseline.
	if got, _ := data["manifest_yaml"].(string); got != string(appYAML) {
		t.Errorf("manifest_yaml differs from package app.yaml:\n%q\nvs\n%q", got, string(appYAML))
	}

	// Image half mirrors upload-image.
	if got, _ := data["image"].(string); got != "e2e/upload-test:1.0.0" {
		t.Errorf("image = %q, want e2e/upload-test:1.0.0; body: %s", got, w.Body.String())
	}
	imagePath, _ := data["image_path"].(string)
	if imagePath == "" {
		t.Fatalf("data.image_path is empty; body: %s", w.Body.String())
	}
	extracted, err := os.ReadFile(imagePath)
	if err != nil {
		t.Fatalf("read extracted image tar %s: %v", imagePath, err)
	}
	if !bytes.Equal(extracted, imageTar) {
		t.Error("extracted image.tar differs from package member")
	}

	// Exactly the two install payloads survive together: staged app.yaml and
	// extracted image tar. The uploaded package itself must be gone.
	if residue := imagesDirEntries(t, imagesDir); len(residue) != 2 {
		t.Errorf("staging dir = %v, want app.yaml plus *_image.tar", residue)
	}
}

func TestUploadPackageMissingManifestRejected(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	pkg := buildNrtPackage(t, "e2e-package-app-1.0.0-arm64", map[string][]byte{
		"image.tar": buildDockerSaveTar(t, true, []byte("layer-payload")),
	})
	w := callUploadPackage(t, "no-manifest.neoapp", pkg)

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "app.yaml") {
		t.Errorf("error detail = %q, want it to mention app.yaml", detail)
	}
	if residue := imagesDirEntries(t, imagesDir); len(residue) != 0 {
		t.Errorf("rejected upload left residue: %v", residue)
	}
}

func TestUploadPackageMissingImageRejected(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	pkg := buildNrtPackage(t, "e2e-package-app-1.0.0-arm64", map[string][]byte{
		"app.yaml": validPackageManifest(),
	})
	w := callUploadPackage(t, "no-image.neoapp", pkg)

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "image.tar") {
		t.Errorf("error detail = %q, want it to mention image.tar", detail)
	}
	if residue := imagesDirEntries(t, imagesDir); len(residue) != 0 {
		t.Errorf("rejected upload left residue: %v", residue)
	}
}

func TestUploadPackageInvalidManifestRejected(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	pkg := buildNrtPackage(t, "e2e-package-app-1.0.0-arm64", map[string][]byte{
		"app.yaml":  []byte("metadata: [broken\n"),
		"image.tar": buildDockerSaveTar(t, true, []byte("layer-payload")),
	})
	w := callUploadPackage(t, "bad-manifest.neoapp", pkg)

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "Invalid manifest") {
		t.Errorf("error detail = %q, want it to start with 'Invalid manifest'", detail)
	}
	if residue := imagesDirEntries(t, imagesDir); len(residue) != 0 {
		t.Errorf("rejected upload left residue: %v", residue)
	}
}

func TestUploadPackageInvalidImageTarRejected(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	pkg := buildNrtPackage(t, "e2e-package-app-1.0.0-arm64", map[string][]byte{
		"app.yaml":  validPackageManifest(),
		"image.tar": buildDockerSaveTar(t, false, []byte("layer-payload")),
	})
	w := callUploadPackage(t, "digest-image.neoapp", pkg)

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "Invalid image tar inside package") {
		t.Errorf("error detail = %q, want it to mention 'Invalid image tar inside package'", detail)
	}
	if residue := imagesDirEntries(t, imagesDir); len(residue) != 0 {
		t.Errorf("rejected upload left residue: %v", residue)
	}
}

func TestUploadPackageWrongExtensionRejected(t *testing.T) {
	_, restore := uploadImageTestRoot(t)
	defer restore()

	pkg := buildNrtPackage(t, "e2e-package-app-1.0.0-arm64", map[string][]byte{
		"app.yaml":  validPackageManifest(),
		"image.tar": buildDockerSaveTar(t, true, []byte("layer-payload")),
	})
	w := callUploadPackage(t, "e2e-package-app.zip", pkg)

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "zip packages are not supported") {
		t.Errorf("error detail = %q, want it to mention zip is not supported", detail)
	}
}

func TestUploadPackageNotGzipRejected(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	// A bare (uncompressed) tar must be rejected: .neoapp is tar.gz.
	var tarBuf bytes.Buffer
	tw := tar.NewWriter(&tarBuf)
	if err := tw.WriteHeader(&tar.Header{Name: "x/app.yaml", Mode: 0644, Size: 4}); err != nil {
		t.Fatalf("write header: %v", err)
	}
	if _, err := tw.Write([]byte("data")); err != nil {
		t.Fatalf("write member: %v", err)
	}
	if err := tw.Close(); err != nil {
		t.Fatalf("close tar: %v", err)
	}
	w := callUploadPackage(t, "bare.neoapp", tarBuf.Bytes())

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "not a gzip/tar.gz archive") {
		t.Errorf("error detail = %q, want it to mention gzip", detail)
	}
	if residue := imagesDirEntries(t, imagesDir); len(residue) != 0 {
		t.Errorf("rejected upload left residue: %v", residue)
	}
}

func TestUploadPackageBombImageOverLimitRejected(t *testing.T) {
	imagesDir, restore := uploadImageTestRoot(t)
	defer restore()

	oldLimit := maxImageUploadBytes
	maxImageUploadBytes = 1024
	defer func() { maxImageUploadBytes = oldLimit }()

	// The gzipped package stays tiny (repeated bytes compress well) but the
	// unpacked image.tar exceeds the shrunken limit — the copy cap inside
	// the unpack loop must catch it, not the package-size check.
	pkg := buildNrtPackage(t, "e2e-package-app-1.0.0-arm64", map[string][]byte{
		"app.yaml":  validPackageManifest(),
		"image.tar": buildDockerSaveTar(t, true, bytes.Repeat([]byte("x"), 64<<10)),
	})
	w := callUploadPackage(t, "bomb.neoapp", pkg)

	code, detail, _ := decodeUploadResponse(t, w)
	if code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	if !strings.Contains(detail, "image.tar exceeds the maximum allowed size") {
		t.Errorf("error detail = %q, want it to mention the image size limit", detail)
	}
	if residue := imagesDirEntries(t, imagesDir); len(residue) != 0 {
		t.Errorf("rejected upload left residue: %v", residue)
	}
}
