package handlers

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/glebarez/sqlite"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"aipc/platform/common/constants"
	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/repo"
	"aipc/platform/platform-api/storage"

	inferencepb "aipc/platform/ai-runtime/proto"
)

// newBlobGCTestEnv mirrors newAIUpdateTestEnv but also hands back the gorm
// handle, which the DB-failure test needs to break writes with.
func newBlobGCTestEnv(t *testing.T) (*APIHandlers, *fakeAIRuntime, *storage.ModelStorage, *gorm.DB) {
	t.Helper()
	gdb, err := gorm.Open(sqlite.Open(filepath.Join(t.TempDir(), "blob_gc.db")), &gorm.Config{
		Logger: logger.Default.LogMode(logger.Warn),
	})
	if err != nil {
		t.Fatalf("gorm.Open: %v", err)
	}
	t.Cleanup(func() {
		if sqlDB, err := gdb.DB(); err == nil {
			_ = sqlDB.Close()
		}
	})
	if err := gdb.AutoMigrate(&model.AIModel{}); err != nil {
		t.Fatalf("AutoMigrate: %v", err)
	}

	store, err := storage.NewModelStorage(filepath.Join(t.TempDir(), "blobs"), 0, 0)
	if err != nil {
		t.Fatalf("NewModelStorage: %v", err)
	}

	lis := bufconn.Listen(1024 * 1024)
	fake := &fakeAIRuntime{}
	srv := grpc.NewServer()
	inferencepb.RegisterInferenceServiceServer(srv, fake)
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)

	conn, err := grpc.DialContext(context.Background(), "bufnet",
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return lis.Dial() }),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("dial bufnet: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	return &APIHandlers{
		aiModelRepo: repo.NewAIModelRepo(gdb),
		grpcClients: &GRPCClients{AIRuntime: conn},
		modelStore:  store,
	}, fake, store, gdb
}

// postUploadMultipart drives UploadModel with a multipart/form-data body:
// the "model" file part plus arbitrary form fields.
func postUploadMultipart(t *testing.T, h *APIHandlers, filename string, content []byte, fields map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	for k, v := range fields {
		if err := mw.WriteField(k, v); err != nil {
			t.Fatalf("WriteField %s: %v", k, err)
		}
	}
	fw, err := mw.CreateFormFile("model", filename)
	if err != nil {
		t.Fatalf("CreateFormFile: %v", err)
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
	c.Request = httptest.NewRequest(http.MethodPost, "/api/v1/ai/models/upload", &body)
	c.Request.Header.Set("Content-Type", mw.FormDataContentType())
	h.UploadModel(c)
	return w
}

// fakeHailortcli puts a stub hailortcli on PATH so ValidateHEF succeeds —
// the build host has no Hailo runtime tooling, and its absence is now an
// explicit validation failure (#18).
func fakeHailortcli(t *testing.T) {
	t.Helper()
	dir := t.TempDir()
	script := "#!/bin/sh\necho 'Network group name: fake_net'\nexit 0\n"
	if err := os.WriteFile(filepath.Join(dir, "hailortcli"), []byte(script), 0755); err != nil {
		t.Fatalf("write fake hailortcli: %v", err)
	}
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
}

// contentHash is the sha256 a SaveWithHash of content would dedup onto.
func contentHash(t *testing.T, content []byte) string {
	t.Helper()
	sum := sha256.Sum256(content)
	return hex.EncodeToString(sum[:])
}

// backdate shifts a file's timestamps into the past so a grace-period sweep
// sees it as stale.
func backdate(t *testing.T, path string, age time.Duration) {
	t.Helper()
	past := time.Now().Add(-age)
	if err := os.Chtimes(path, past, past); err != nil {
		t.Fatalf("chtimes %s: %v", path, err)
	}
}

// Malformed form values are rejected up front, naming the field, before any
// bytes are staged or rows written.
func TestUploadModelRejectsBadFormFields(t *testing.T) {
	cases := []struct {
		name   string
		fields map[string]string
		want   string
	}{
		{"bad threshold", map[string]string{"threshold": "abc"}, "Invalid threshold"},
		{"bad max_detections", map[string]string{"max_detections": "xyz"}, "Invalid max_detections"},
		{"bad model_type", map[string]string{"model_type": "turbo"}, "Unsupported model_type"},
		{"bad model_id", map[string]string{"model_id": "bad/id"}, "Invalid model_id"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			h, _, store, _ := newBlobGCTestEnv(t)
			w := postUploadMultipart(t, h, "m.hef", []byte("bytes"), tc.fields)
			if respCode(t, w) != CodeInvalidRequest {
				t.Fatalf("code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeInvalidRequest)
			}
			if !strings.Contains(w.Body.String(), tc.want) {
				t.Errorf("response must name the bad field %q, got %s", tc.want, w.Body.String())
			}
			if blobs, _ := store.ListBlobs(); len(blobs) != 0 {
				t.Errorf("rejected upload must not stage a blob, got %d", len(blobs))
			}
			if rows, err := h.aiModelRepo.List(); err != nil || len(rows) != 0 {
				t.Errorf("rejected upload must not write a row, got %d rows (err=%v)", len(rows), err)
			}
		})
	}
}

// A duplicate model_id is refused before staging anything.
func TestUploadModelDuplicateIDRejected(t *testing.T) {
	h, _, store, _ := newBlobGCTestEnv(t)
	seedAIModel(t, h, &model.AIModel{ModelID: "dup_det", Name: "dup", Status: "uploaded"})

	w := postUploadMultipart(t, h, "dup.hef", []byte("dup-bytes"), map[string]string{"model_id": "dup_det"})
	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeInvalidRequest)
	}
	if !strings.Contains(w.Body.String(), "already exists") {
		t.Errorf("response must say the id exists, got %s", w.Body.String())
	}
	if blobs, _ := store.ListBlobs(); len(blobs) != 0 {
		t.Errorf("duplicate upload must not stage a blob, got %d", len(blobs))
	}
}

// A fresh blob whose upload then fails HEF validation is reclaimed.
func TestUploadModelValidationFailureCleansFreshBlob(t *testing.T) {
	h, _, store, _ := newBlobGCTestEnv(t)
	content := []byte("not-actually-a-hef")
	// No fake hailortcli: validation genuinely fails on this host.

	w := postUploadMultipart(t, h, "fresh.hef", content, map[string]string{"model_id": "fresh_det"})
	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeInvalidRequest)
	}
	if !strings.Contains(w.Body.String(), "HEF validation failed") {
		t.Errorf("response must say validation failed, got %s", w.Body.String())
	}
	if store.Exists(contentHash(t, content), ".hef") {
		t.Error("blob of the failed fresh upload must be reclaimed")
	}
	if blobs, _ := store.ListBlobs(); len(blobs) != 0 {
		t.Errorf("blob dir must be empty after failed fresh upload, got %d", len(blobs))
	}
}

// The dedup regression proof: uploading bytes whose blob a live row already
// references must never delete that blob when the new upload fails — the old
// code removed it unconditionally.
func TestUploadModelValidationFailureKeepsDedupedBlob(t *testing.T) {
	h, _, store, _ := newBlobGCTestEnv(t)
	content := []byte("shared-hef-bytes")
	hash := contentHash(t, content)
	blobPath := store.BlobPath(hash, ".hef")
	if err := os.WriteFile(blobPath, content, 0644); err != nil {
		t.Fatalf("seed blob: %v", err)
	}
	seedAIModel(t, h, &model.AIModel{
		ModelID: "live_det", Name: "live", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: blobPath, FileHash: hash,
	})

	w := postUploadMultipart(t, h, "wannabe.hef", content, map[string]string{"model_id": "wannabe_det"})
	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("code = %d body=%s, want %d (validation fails without hailortcli)", respCode(t, w), w.Body.String(), CodeInvalidRequest)
	}
	if !store.Exists(hash, ".hef") {
		t.Fatal("deduped blob referenced by a live row must survive the failed upload")
	}
	if row, err := h.aiModelRepo.GetByModelID("live_det"); err != nil || row == nil {
		t.Error("the live row must be untouched")
	}
}

// A DB write failure is an explicit error — never the old silent success
// that reported OK with no row. Cleanup fails closed: with the DB unusable
// the reference count cannot be checked, so the blob is kept for the sweep
// to reclaim once the DB recovers.
func TestUploadModelDBFailureErrorsAndFailsClosed(t *testing.T) {
	h, _, store, gdb := newBlobGCTestEnv(t)
	fakeHailortcli(t)
	if err := gdb.Migrator().DropTable(&model.AIModel{}); err != nil {
		t.Fatalf("drop table: %v", err)
	}
	content := []byte("dbfail-hef-bytes")

	w := postUploadMultipart(t, h, "dbfail.hef", content, map[string]string{"model_id": "dbfail_det"})
	if respCode(t, w) != CodeServiceError {
		t.Fatalf("code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeServiceError)
	}
	if !strings.Contains(w.Body.String(), "Failed to save model record") {
		t.Errorf("response must say the record save failed, got %s", w.Body.String())
	}
	if !store.Exists(contentHash(t, content), ".hef") {
		t.Error("fail-closed cleanup must keep the blob when the reference count is uncheckable")
	}
}

// Happy path with a stub hailortcli: row persisted with the parsed form
// values and the CAS path/blob in place.
func TestUploadModelHappyPathCreatesRow(t *testing.T) {
	h, _, store, _ := newBlobGCTestEnv(t)
	fakeHailortcli(t)
	content := []byte("happy-hef-bytes")
	hash := contentHash(t, content)

	w := postUploadMultipart(t, h, "happy.hef", content, map[string]string{
		"model_id":       "happy_det",
		"model_type":     "detection",
		"threshold":      "0.5",
		"max_detections": "32",
	})
	if respCode(t, w) != 0 {
		t.Fatalf("code = %d body=%s, want 0", respCode(t, w), w.Body.String())
	}
	row, err := h.aiModelRepo.GetByModelID("happy_det")
	if err != nil || row == nil {
		t.Fatalf("row must exist after upload, err=%v", err)
	}
	if row.Status != "uploaded" || row.ModelType != "detection" ||
		row.Threshold != 0.5 || row.MaxDetections != 32 {
		t.Errorf("row fields = type:%q thr:%v max:%d status:%q, want detection/0.5/32/uploaded",
			row.ModelType, row.Threshold, row.MaxDetections, row.Status)
	}
	var cfg map[string]interface{}
	if err := json.Unmarshal([]byte(row.Config), &cfg); err != nil {
		t.Fatalf("uploaded detection config must be JSON: %v (%q)", err, row.Config)
	}
	if cfg["threshold"] != 0.5 || cfg["nms_threshold"] != 0.45 {
		t.Errorf("uploaded config = %v, want threshold=0.5 and schema nms default=0.45", cfg)
	}
	if row.DesiredState != "unloaded" {
		t.Errorf("uploaded row desired_state = %q, want unloaded — upload is not a load promise", row.DesiredState)
	}
	if row.FileHash != hash || row.FilePath != store.BlobPath(hash, ".hef") {
		t.Errorf("row file = %q@%q, want CAS %s", row.FilePath, row.FileHash, hash)
	}
	if !store.Exists(hash, ".hef") {
		t.Error("blob must exist after successful upload")
	}
}

// A FilePath outside the platform model root — an app's bundled HEF, a
// user-registered model_path — survives row deletion, with or without a
// backfilled FileHash.
func TestUnregisterModelKeepsForeignFilePath(t *testing.T) {
	h, _, _, _ := newBlobGCTestEnv(t)
	appDir := t.TempDir()

	plainPath := filepath.Join(appDir, "plain.hef")
	if err := os.WriteFile(plainPath, []byte("app-owned"), 0644); err != nil {
		t.Fatalf("seed plain: %v", err)
	}
	seedAIModel(t, h, &model.AIModel{
		ModelID: "app_plain", Name: "plain", Status: "uploaded", Source: "web",
		FilePath: plainPath,
	})

	hashedPath := filepath.Join(appDir, "hashed.hef")
	if err := os.WriteFile(hashedPath, []byte("app-owned-2"), 0644); err != nil {
		t.Fatalf("seed hashed: %v", err)
	}
	seedAIModel(t, h, &model.AIModel{
		ModelID: "app_hashed", Name: "hashed", Status: "uploaded", Source: "web",
		FilePath: hashedPath, FileHash: strings.Repeat("ab", 32),
	})

	for _, id := range []string{"app_plain", "app_hashed"} {
		if w := deleteModel(t, h, id); respCode(t, w) != 0 {
			t.Fatalf("delete %s: %s", id, w.Body.String())
		}
	}
	for _, path := range []string{plainPath, hashedPath} {
		if _, err := os.Stat(path); err != nil {
			t.Errorf("foreign model file %s must survive row deletion: %v", path, err)
		}
	}
}

// A disk-scan row whose file lives inside the platform model root is
// reclaimed together with the row — the old code left it behind for
// SeedDiskModels to resurrect.
func TestUnregisterModelRemovesDiskScanFileInRoot(t *testing.T) {
	withTempConstantsRoot(t)
	h, _, store, _ := newBlobGCTestEnv(t)
	diskDir := filepath.Join(constants.ModelsPath(), "detection")
	if err := os.MkdirAll(diskDir, 0755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	diskPath := filepath.Join(diskDir, "diskscan.hef")
	if err := os.WriteFile(diskPath, []byte("disk-scan-model"), 0644); err != nil {
		t.Fatalf("seed disk file: %v", err)
	}
	seedAIModel(t, h, &model.AIModel{
		ModelID: "disk_det", Name: "disk", Status: "uploaded", Source: "disk",
		FilePath: diskPath, FileHash: strings.Repeat("cd", 32),
	})

	if w := deleteModel(t, h, "disk_det"); respCode(t, w) != 0 {
		t.Fatalf("delete disk_det: %s", w.Body.String())
	}
	if _, err := os.Stat(diskPath); !os.IsNotExist(err) {
		t.Errorf("platform-root model file must be removed with the row, stat err=%v", err)
	}
	// The phantom CAS blob referenced by the backfilled hash is a no-op
	// delete, not an error.
	if blobs, _ := store.ListBlobs(); len(blobs) != 0 {
		t.Errorf("no CAS blob should have been created, got %d", len(blobs))
	}
}

// The sweep keeps referenced and fresh blobs, collects stale orphans and
// stale upload temp files.
func TestSweepOrphanBlobs(t *testing.T) {
	h, _, store, _ := newBlobGCTestEnv(t)

	refHash := contentHash(t, []byte("referenced"))
	if err := os.WriteFile(store.BlobPath(refHash, ".hef"), []byte("referenced"), 0644); err != nil {
		t.Fatalf("seed referenced: %v", err)
	}
	seedAIModel(t, h, &model.AIModel{
		ModelID: "ref_det", Name: "ref", Status: "loaded", Source: "web",
		FilePath: store.BlobPath(refHash, ".hef"), FileHash: refHash,
	})

	staleHash := contentHash(t, []byte("stale-orphan"))
	stalePath := store.BlobPath(staleHash, ".hef")
	if err := os.WriteFile(stalePath, []byte("stale-orphan"), 0644); err != nil {
		t.Fatalf("seed stale: %v", err)
	}
	backdate(t, stalePath, 2*time.Hour)

	freshHash := contentHash(t, []byte("fresh-orphan"))
	if err := os.WriteFile(store.BlobPath(freshHash, ".hef"), []byte("fresh-orphan"), 0644); err != nil {
		t.Fatalf("seed fresh: %v", err)
	}

	blobDir := filepath.Dir(store.BlobPath("x", ".hef"))
	staleTmp := filepath.Join(blobDir, "upload-stale.tmp")
	if err := os.WriteFile(staleTmp, []byte("crashed"), 0644); err != nil {
		t.Fatalf("seed stale tmp: %v", err)
	}
	backdate(t, staleTmp, 2*time.Hour)
	freshTmp := filepath.Join(blobDir, "upload-fresh.tmp")
	if err := os.WriteFile(freshTmp, []byte("in-flight"), 0644); err != nil {
		t.Fatalf("seed fresh tmp: %v", err)
	}

	h.sweepOrphanBlobs(time.Hour)

	if !store.Exists(refHash, ".hef") {
		t.Error("referenced blob must be kept")
	}
	if !store.Exists(freshHash, ".hef") {
		t.Error("fresh orphan must be kept until the grace period passes")
	}
	if store.Exists(staleHash, ".hef") {
		t.Error("stale orphan must be collected")
	}
	if _, err := os.Stat(staleTmp); !os.IsNotExist(err) {
		t.Error("stale upload temp must be collected")
	}
	if _, err := os.Stat(freshTmp); err != nil {
		t.Error("fresh upload temp must be kept", err)
	}
}

// The sweep rides the self-heal tick.
func TestSelfHealPassSweepsOrphanBlobs(t *testing.T) {
	withTempConstantsRoot(t)
	h, _, store, _ := newBlobGCTestEnv(t)
	staleHash := contentHash(t, []byte("crashed-import"))
	stalePath := store.BlobPath(staleHash, ".hef")
	if err := os.WriteFile(stalePath, []byte("crashed-import"), 0644); err != nil {
		t.Fatalf("seed stale: %v", err)
	}
	backdate(t, stalePath, 2*time.Hour)

	h.selfHealPass(context.Background(), newModelHealBackoff(), time.Minute)

	if store.Exists(staleHash, ".hef") {
		t.Error("heal pass must collect stale orphan blobs")
	}
}

// Admission (register/update/upload committing a referencing row) and the
// sweep serialize on blobRefMu: while an admission holds the lock, the
// sweep must not collect — even an aged, unreferenced blob survives until
// the lock is released, and collects right after.
func TestSweepOrphanBlobsWaitsForAdmission(t *testing.T) {
	h, _, store, _ := newBlobGCTestEnv(t)
	staleHash := contentHash(t, []byte("admission-raced"))
	stalePath := store.BlobPath(staleHash, ".hef")
	if err := os.WriteFile(stalePath, []byte("admission-raced"), 0644); err != nil {
		t.Fatalf("seed aged orphan: %v", err)
	}
	backdate(t, stalePath, 2*time.Hour)

	done := make(chan struct{})
	h.blobRefMu.Lock()
	go func() {
		h.sweepOrphanBlobs(time.Hour)
		close(done)
	}()
	time.Sleep(100 * time.Millisecond)
	select {
	case <-done:
		t.Fatal("sweep must block while an admission holds blobRefMu")
	default:
	}
	if !store.Exists(staleHash, ".hef") {
		t.Fatal("aged orphan must survive while admission holds the lock")
	}
	h.blobRefMu.Unlock()
	<-done
	if store.Exists(staleHash, ".hef") {
		t.Error("sweep must collect the orphan once admission released the lock")
	}
}

// cleanupStagedBlob must serialize its count→delete decision with admission.
// Once admission commits a reference while holding blobRefMu, cleanup recounts
// after acquiring the lock and preserves the newly referenced blob.
func TestCleanupStagedBlobConcurrentAdmissionPreservesBlob(t *testing.T) {
	h, _, store, _ := newBlobGCTestEnv(t)
	hash := contentHash(t, []byte("cleanup-admission"))
	path := store.BlobPath(hash, ".hef")
	if err := os.WriteFile(path, []byte("cleanup-admission"), 0644); err != nil {
		t.Fatal(err)
	}

	h.blobRefMu.Lock()
	cleanupDone := make(chan struct{})
	go func() {
		h.cleanupStagedBlob(hash, ".hef", false)
		close(cleanupDone)
	}()
	select {
	case <-cleanupDone:
		t.Fatal("cleanup must wait for concurrent admission")
	case <-time.After(100 * time.Millisecond):
	}
	seedAIModel(t, h, &model.AIModel{
		ModelID: "admitted", Name: "admitted", Status: "uploaded", Source: "web",
		FilePath: path, FileHash: hash,
	})
	h.blobRefMu.Unlock()
	<-cleanupDone

	if !store.Exists(hash, ".hef") {
		t.Fatal("cleanup deleted a blob admitted by a concurrent model")
	}
}

func TestUploadModelPersistsExplicitZeroDetectionThreshold(t *testing.T) {
	h, _, _, _ := newBlobGCTestEnv(t)
	fakeHailortcli(t)

	w := postUploadMultipart(t, h, "zero.hef", []byte("zero-hef"), map[string]string{
		"model_id":   "zero_det",
		"model_type": "detection",
		"threshold":  "0",
	})
	if respCode(t, w) != 0 {
		t.Fatalf("upload failed: %s", w.Body.String())
	}
	row, err := h.aiModelRepo.GetByModelID("zero_det")
	if err != nil || row == nil {
		t.Fatalf("uploaded row missing: %v", err)
	}
	if row.Threshold != 0 {
		t.Fatalf("row threshold = %v, want explicit 0", row.Threshold)
	}
	var cfg map[string]interface{}
	if err := json.Unmarshal([]byte(row.Config), &cfg); err != nil {
		t.Fatalf("uploaded config is not JSON: %v (%q)", err, row.Config)
	}
	if v, ok := cfg["threshold"]; !ok || v != float64(0) {
		t.Fatalf("uploaded config threshold = %v (present=%v), want explicit 0 presence signal", v, ok)
	}
}
