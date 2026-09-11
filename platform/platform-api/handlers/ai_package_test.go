package handlers

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/constants"
	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/storage"
)

// Vstream info snippets as the wizard carries them back — the JSON-serialized
// parse-hef vstream text; the classifier keys off the NMS tensor name.
const (
	nmsVstreamInfo        = `hailo_yolov8n_384_640/yolov8_nms_postprocess`
	featureMapVstreamInfo = `yolov5m_vehicles/conv21`
)

func postRegister(t *testing.T, h *APIHandlers, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest(http.MethodPost, "/api/v1/ai/models", strings.NewReader(body))
	c.Request.Header.Set("Content-Type", "application/json")
	h.RegisterModel(c)
	return w
}

// Unknown output_mode values are rejected outright — silently coercing them
// to platform would register a model whose delivery mode is a lie.
func TestRegisterModelRejectsInvalidOutputMode(t *testing.T) {
	h, fake, _ := newAIUpdateTestEnv(t)
	w := postRegister(t, h, `{"model_id":"bad_mode","model_path":"/x.hef","model_type":"detection","output_mode":"turbo"}`)
	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeInvalidRequest)
	}
	if !strings.Contains(w.Body.String(), "output_mode") {
		t.Errorf("error must name the offending field: %s", w.Body.String())
	}
	if row, _ := h.aiModelRepo.GetByModelID("bad_mode"); row != nil {
		t.Errorf("rejected model must not be persisted, got %+v", row)
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("no runtime calls expected, got %v", calls)
	}
}

// The server-side guard behind the wizard's disabled radio card: a
// feature-map HEF registered as platform+detection would load and then fail
// postprocess on every frame.
func TestRegisterModelRejectsFeatureMapPlatformDetection(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	w := postRegister(t, h, `{"model_id":"fm_det","model_path":"/x.hef","model_type":"detection","output_mode":"platform","vstream_info":"`+featureMapVstreamInfo+`"}`)
	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeInvalidRequest)
	}
	if !strings.Contains(w.Body.String(), "feature maps") {
		t.Errorf("error must explain the feature-map cause: %s", w.Body.String())
	}
	if row, _ := h.aiModelRepo.GetByModelID("fm_det"); row != nil {
		t.Errorf("rejected model must not be persisted, got %+v", row)
	}
}

// Raw mode is the escape hatch for feature-map HEFs — the same registration
// must pass and store the mode on the row.
func TestRegisterModelRawModeStoredOnRow(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	w := postRegister(t, h, `{"model_id":"raw_det","model_path":"/x.hef","model_type":"detection","output_mode":"raw","vstream_info":"`+featureMapVstreamInfo+`"}`)
	if respCode(t, w) != 0 {
		t.Fatalf("raw registration failed: %s", w.Body.String())
	}
	row, err := h.aiModelRepo.GetByModelID("raw_det")
	if err != nil || row == nil {
		t.Fatalf("row missing: %v", err)
	}
	if row.OutputMode != model.OutputModeRaw {
		t.Errorf("row output_mode = %q, want %q", row.OutputMode, model.OutputModeRaw)
	}
	if row.DesiredState != "unloaded" {
		t.Errorf("registered row desired_state = %q, want unloaded — registration is not a load promise", row.DesiredState)
	}
	if !strings.Contains(w.Body.String(), `"output_mode":"raw"`) {
		t.Errorf("response must echo output_mode: %s", w.Body.String())
	}
}

// UpdateModel applies the same cross-check against the post-update view: a
// raw feature-map model cannot be flipped to platform delivery.
func TestUpdateModelRejectsRawToPlatformOnFeatureMap(t *testing.T) {
	h, fake, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "fm_raw", Name: "fm_raw", Status: "uploaded", Source: "web",
		ModelType: "detection", OutputMode: model.OutputModeRaw,
		VStreamInfo: featureMapVstreamInfo,
		FilePath:    "/blobs/h1.hef", FileHash: "h1",
	})
	w := putUpdate(t, h, "fm_raw", `{"output_mode":"platform"}`)
	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeInvalidRequest)
	}
	if !strings.Contains(w.Body.String(), "feature maps") {
		t.Errorf("error must explain the feature-map cause: %s", w.Body.String())
	}
	row, _ := h.aiModelRepo.GetByModelID("fm_raw")
	if row.OutputMode != model.OutputModeRaw {
		t.Errorf("rejected swap must leave row raw, got %q", row.OutputMode)
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("no runtime calls expected, got %v", calls)
	}
}

// A platform→raw swap on a loaded model changes the whole gRPC payload (empty
// model_type/variant, no materialized copy), so it must reload — and the
// reload must register the raw view, not the detection postprocess view.
func TestUpdateModelPlatformToRawReloadsBare(t *testing.T) {
	h, fake, store := newAIUpdateTestEnv(t)
	oldRoot := constants.RootPath()
	constants.SetRootPath(t.TempDir())
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })
	blob := seedBlob(t, store, "h1")
	// Row loaded and the runtime truly serves it — the mode swap's
	// unload→reload decision probes runtime presence now.
	fake.markLive("swap_det")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "swap_det", Name: "swap_det", Status: "loaded", Source: "web",
		ModelType: "detection", OutputMode: model.OutputModePlatform,
		VStreamInfo: nmsVstreamInfo,
		FilePath:    blob, FileHash: "h1", DesiredState: "loaded",
	})
	w := putUpdate(t, h, "swap_det", `{"output_mode":"raw"}`)
	if respCode(t, w) != 0 {
		t.Fatalf("output-mode swap failed: %s", w.Body.String())
	}
	calls, regPaths := fake.snapshot()
	if len(calls) != 2 || calls[0] != "unload:swap_det" || calls[1] != "load:swap_det" {
		t.Fatalf("mode swap on a loaded model must unload→reload, got %v", calls)
	}
	if got := fake.registeredVariant("swap_det"); got != "" {
		t.Errorf("raw reload must send an empty variant, got %q", got)
	}
	if regPaths["swap_det"] != blob {
		t.Errorf("raw reload must use the stored path %q, got %q", blob, regPaths["swap_det"])
	}
	// No materialized postprocess copy may be created for a raw model.
	if _, err := os.Stat(filepath.Join(constants.ModelsPath(), "runtime", "swap_det")); !os.IsNotExist(err) {
		t.Errorf("raw model must not materialize a runtime copy (err=%v)", err)
	}
	row, _ := h.aiModelRepo.GetByModelID("swap_det")
	if row.OutputMode != model.OutputModeRaw || row.Status != "loaded" {
		t.Errorf("row after swap: output_mode=%q status=%q, want raw/loaded", row.OutputMode, row.Status)
	}
}

// getExport runs the export handler for a model id and returns the recorder.
func getExport(t *testing.T, h *APIHandlers, modelID string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Params = gin.Params{{Key: "model_id", Value: modelID}}
	c.Request = httptest.NewRequest(http.MethodGet, "/api/v1/ai/models/"+modelID+"/export", nil)
	h.ExportModel(c)
	return w
}

// Export → import round-trip: the package reproduces the row's effective
// registration (id/type/mode/tuning) and its HEF lands in CAS under the
// content hash — the same blob a plain .hef import of the same bytes yields.
func TestExportImportRoundTrip(t *testing.T) {
	h, _, store := newAIUpdateTestEnv(t)

	// A real file with real content so the export hash and the staged-blob
	// hash agree.
	hefBody := []byte("hef-bytes-for-roundtrip")
	sum := sha256.Sum256(hefBody)
	contentHash := hex.EncodeToString(sum[:])
	hefPath := store.BlobPath(contentHash, ".hef")
	if err := os.WriteFile(hefPath, hefBody, 0644); err != nil {
		t.Fatalf("seed hef: %v", err)
	}

	seedAIModel(t, h, &model.AIModel{
		ModelID: "rt_det", Name: "rt_det", Status: "uploaded", Source: "web",
		ModelType: "detection", OutputMode: model.OutputModePlatform,
		VStreamInfo: nmsVstreamInfo,
		FilePath:    hefPath, FileHash: contentHash,
		Threshold: 0.4, MaxDetections: 32,
		Config: `{"postprocess_profile":"hailo_yolov8s_384_640","nms_threshold":0.5}`,
	})

	w := getExport(t, h, "rt_det")
	if w.Code != http.StatusOK {
		t.Fatalf("export status = %d body=%s", w.Code, w.Body.String())
	}
	if cd := w.Header().Get("Content-Disposition"); !strings.Contains(cd, "rt_det.bin") {
		t.Errorf("Content-Disposition = %q, want rt_det.bin attachment", cd)
	}

	pkgPath := filepath.Join(t.TempDir(), "rt_det.bin")
	if err := os.WriteFile(pkgPath, w.Body.Bytes(), 0644); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open(pkgPath)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()

	st, err := f.Stat()
	if err != nil {
		t.Fatal(err)
	}
	meta, result, err := h.importModelPackage(f, st.Size())
	if err != nil {
		t.Fatalf("importModelPackage: %v", err)
	}
	if meta.ModelID != "rt_det" || meta.ModelType != "detection" || meta.OutputMode != model.OutputModePlatform {
		t.Errorf("meta id/type/mode = %q/%q/%q, want rt_det/detection/platform", meta.ModelID, meta.ModelType, meta.OutputMode)
	}
	if meta.HEF.SHA256 != contentHash || result.Hash != contentHash {
		t.Errorf("hash mismatch: meta=%s staged=%s, want %s", meta.HEF.SHA256, result.Hash, contentHash)
	}
	if !store.Exists(contentHash, ".hef") {
		t.Error("staged package HEF missing from CAS")
	}
	// Detection column values are authoritative in the exported config —
	// the composed runtime variant consumes them, so the package must carry them.
	var cfg map[string]interface{}
	if err := json.Unmarshal(meta.Config, &cfg); err != nil {
		t.Fatalf("exported config not JSON: %v (%s)", err, meta.Config)
	}
	if cfg["threshold"] != 0.4 || cfg["max_detections"] != float64(32) {
		t.Errorf("exported tuning = %v/%v, want 0.4/32", cfg["threshold"], cfg["max_detections"])
	}
	if cfg["postprocess_profile"] != "hailo_yolov8s_384_640" {
		t.Errorf("stored profile must survive export, got %v", cfg["postprocess_profile"])
	}
	if meta.OutputFormat != model.OutputFormatNMS {
		t.Errorf("meta output_format = %q, want %q", meta.OutputFormat, model.OutputFormatNMS)
	}
}

// A detection row whose stored Config predates the profile field must still
// export with the resolved postprocess_profile pinned: without it the
// importing side composes the default profile's backend_function instead of
// the one this row runs.
func TestExportPinsResolvedPostprocessProfile(t *testing.T) {
	h, _, store := newAIUpdateTestEnv(t)

	hefBody := []byte("hef-bytes-profile-pin")
	sum := sha256.Sum256(hefBody)
	contentHash := hex.EncodeToString(sum[:])
	hefPath := store.BlobPath(contentHash, ".hef")
	if err := os.WriteFile(hefPath, hefBody, 0644); err != nil {
		t.Fatalf("seed hef: %v", err)
	}

	// Legacy tuning columns but a config that never named a profile.
	seedAIModel(t, h, &model.AIModel{
		ModelID: "legacy_det", Name: "legacy_det", Status: "uploaded", Source: "web",
		ModelType: "detection", OutputMode: model.OutputModePlatform,
		VStreamInfo: nmsVstreamInfo,
		FilePath:    hefPath, FileHash: contentHash,
		Threshold: 0.5, MaxDetections: 16,
		Config: `{"nms_threshold":0.45}`,
	})

	w := getExport(t, h, "legacy_det")
	if w.Code != http.StatusOK {
		t.Fatalf("export status = %d body=%s", w.Code, w.Body.String())
	}

	pkg, err := os.CreateTemp(t.TempDir(), "legacy_det-*.bin")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := pkg.Write(w.Body.Bytes()); err != nil {
		t.Fatal(err)
	}
	if err := pkg.Close(); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open(pkg.Name())
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()

	pr, err := storage.OpenPackage(f)
	if err != nil {
		t.Fatalf("OpenPackage: %v", err)
	}
	var cfg map[string]interface{}
	if err := json.Unmarshal(pr.Meta().Config, &cfg); err != nil {
		t.Fatalf("exported config not JSON: %v (%s)", err, pr.Meta().Config)
	}
	if cfg["postprocess_profile"] != model.DefaultDetectionProfile {
		t.Errorf("postprocess_profile = %v, want resolved default %q", cfg["postprocess_profile"], model.DefaultDetectionProfile)
	}
	// The pre-existing tuning keys survive alongside the pinned profile.
	if cfg["nms_threshold"] != 0.45 || cfg["threshold"] != 0.5 || cfg["max_detections"] != float64(16) {
		t.Errorf("tuning drift in exported config: %v", cfg)
	}
}

// App-owned rows are not device-level models — export must 404, never leak
// another app's bundled model bytes.
func TestExportAppOwnedRowNotFound(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "app_model", Name: "app_model", Status: "loaded", Source: "dynamic",
		OwnerAppID: "app-x", FilePath: "/data/x.hef",
	})
	if w := getExport(t, h, "app_model"); respCode(t, w) != CodeNotFound {
		t.Fatalf("app-owned export code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeNotFound)
	}
	if w := getExport(t, h, "ghost"); respCode(t, w) != CodeNotFound {
		t.Fatalf("missing export code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeNotFound)
	}
}

// A corrupted package (single flipped byte) must be rejected before anything
// is staged — the two-pass import proves the digest first, and a rejected
// import must not disturb blobs that already exist.
func TestImportPackageHashMismatchPreservesAdjacentModelBlob(t *testing.T) {
	h, _, store := newAIUpdateTestEnv(t)
	hefBody := []byte("shared-package-hef")
	sum := sha256.Sum256(hefBody)
	hash := hex.EncodeToString(sum[:])
	path := store.BlobPath(hash, ".hef")
	// Simulate an adjacent admitted row whose missing CAS file is repaired by
	// this import. Metadata is deliberately wrong, so cleanup must still retain
	// the newly staged bytes because the row references their actual hash.
	seedAIModel(t, h, &model.AIModel{
		ModelID: "adjacent", Name: "adjacent", Status: "uploaded", Source: "web",
		FilePath: path, FileHash: hash,
	})

	hefFile, err := os.CreateTemp(t.TempDir(), "adjacent-*.hef")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := hefFile.Write(hefBody); err != nil {
		t.Fatal(err)
	}
	if _, err := hefFile.Seek(0, 0); err != nil {
		t.Fatal(err)
	}
	pkgFile, err := os.CreateTemp(t.TempDir(), "adjacent-*.bin")
	if err != nil {
		t.Fatal(err)
	}
	meta := &storage.PackageMeta{
		ModelID: "bad_meta",
		HEF:     storage.PackageHEF{Filename: "adjacent.hef", SHA256: strings.Repeat("0", 64)},
	}
	if err := storage.WritePackage(pkgFile, meta, hefFile); err != nil {
		t.Fatal(err)
	}
	if _, err := pkgFile.Seek(0, 0); err != nil {
		t.Fatal(err)
	}
	st, err := pkgFile.Stat()
	if err != nil {
		t.Fatal(err)
	}

	if _, _, err := h.importModelPackage(pkgFile, st.Size()); err == nil || !strings.Contains(err.Error(), "sha256 mismatch") {
		t.Fatalf("want metadata hash mismatch, got %v", err)
	}
	if !store.Exists(hash, ".hef") {
		t.Fatal("hash-mismatch cleanup deleted a blob referenced by an adjacent model")
	}
}

func TestImportPackageTamperedRejectedNothingStaged(t *testing.T) {
	h, _, store := newAIUpdateTestEnv(t)

	hefBody := []byte("tamper-target-hef")
	sum := sha256.Sum256(hefBody)
	contentHash := hex.EncodeToString(sum[:])
	hefPath := store.BlobPath(contentHash, ".hef")
	if err := os.WriteFile(hefPath, hefBody, 0644); err != nil {
		t.Fatal(err)
	}
	seedAIModel(t, h, &model.AIModel{
		ModelID: "tamper_src", Name: "tamper_src", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: hefPath, FileHash: contentHash,
	})
	w := getExport(t, h, "tamper_src")
	if w.Code != http.StatusOK {
		t.Fatalf("export status = %d", w.Code)
	}

	// Flip one byte in the middle of the package.
	body := w.Body.Bytes()
	body[len(body)/2] ^= 0xFF
	pkgPath := filepath.Join(t.TempDir(), "tampered.bin")
	if err := os.WriteFile(pkgPath, body, 0644); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open(pkgPath)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()

	if _, _, err := h.importModelPackage(f, int64(len(body))); err == nil {
		t.Fatal("tampered package must be rejected")
	}
	// The pre-existing blob must survive untouched.
	if !store.Exists(contentHash, ".hef") {
		t.Error("existing CAS blob must not be disturbed by a rejected import")
	}
}
