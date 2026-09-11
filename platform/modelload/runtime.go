// Package modelload holds the load-time model composition shared by every
// service that registers models with ai-runtime: platform-api's LoadModel and
// app-manager's PreloadModels. Both must derive the exact same gRPC
// registration (materialized path, variant blob, semantic type) from a stored
// AIModel row — PreloadModels used to bypass this and hand the runtime the raw
// CAS blob path with an empty variant, silently degrading non-default
// detection models to raw tensors after a reboot (the plugin's default
// backend function never matches their NMS tensor names).
package modelload

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
	"aipc/platform/platform-api/model"
)

// The vendor postprocess plugin (libyolo_hailortpp_post.so, unmodifiable)
// selects its postprocess function from the HEF file basename: HAL rewrites
// the NMS tensor name to <basename>/yolov8_nms_postprocess and the plugin's
// compiled-in basename table never matches the sha256 blob name wizard
// imports get. The helpers below re-materialize such models under a
// recognized basename at load time and compose a schema-valid variant JSON,
// so the stored threshold / max_detections actually reach the runtime.

// modelIDPattern guards the directory name used under <ModelsPath>/runtime/.
// RegisterModel does not sanitize model_id (only empty/duplicate checks) and
// the id becomes a path segment here, so anything outside this set is rejected.
var modelIDPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*$`)

// Tuning defaults for rows stored before the columns existed (or zeroed by
// hand): they mirror the gorm column defaults on AIModel and the NMS default
// from the reference config the vendor plugin ships.
const (
	defaultDetectionThreshold = 0.25
	defaultNmsThreshold       = 0.45
	defaultMaxDetections      = 64
)

// defaultPostprocessLabels mirrors the plugin's compiled-in label table
// shape: index 0 is a placeholder so labels[N] names class_id N.
var defaultPostprocessLabels = []string{"unlabeled", "person", "vehicle", "face", "license_plate"}

// DetectionPostprocessProfile returns the stored postprocess_profile for a
// detection model, falling back to the default when Config is missing,
// unparseable, or has no postprocess_profile key (legacy rows). A value that
// is present but names an unknown profile — a typo'd postprocess_profile — is
// an error rather than a silent fallback: the typo would otherwise mismatch
// the model to the default profile's materialization basename and backend
// function at load time with no diagnostic. Exported because app-manager's
// bundled-package unpacker names the extracted HEF after it (the plugin
// basename passthrough) and platform-api pins it into exported packages.
func DetectionPostprocessProfile(m *model.AIModel) (string, error) {
	if m.Config != "" {
		var cfg map[string]interface{}
		if err := json.Unmarshal([]byte(m.Config), &cfg); err == nil {
			if v, ok := cfg["postprocess_profile"]; ok {
				name, isStr := v.(string)
				if !isStr {
					return "", fmt.Errorf("postprocess_profile must be a string, got %T (supported: %s)", v, supportedProfileBasenames())
				}
				if _, valid := model.LookupDetectionProfile(name); !valid {
					return "", fmt.Errorf("postprocess_profile %q is not supported (supported: %s)", name, supportedProfileBasenames())
				}
				return name, nil
			}
		}
	}
	return model.DefaultDetectionProfile, nil
}

// supportedProfileBasenames lists the verified plugin basenames for error
// messages, derived from the same table LookupDetectionProfile consults.
func supportedProfileBasenames() string {
	names := make([]string, 0, len(model.DetectionPostprocessProfiles))
	for _, p := range model.DetectionPostprocessProfiles {
		names = append(names, p.Basename)
	}
	return strings.Join(names, ", ")
}

// RuntimeRegistration returns the model path, variant and semantic model type
// to hand to ai-runtime when loading. Raw-output models skip the postprocess
// session entirely — an empty gRPC ModelType makes grpc_service skip
// init_post_process, so the stored file loads as-is and Infer returns bare
// tensors (the consumer decodes). Platform-mode detection models get
// materialized under a plugin-recognized basename with a composed variant;
// everything else passes through unchanged.
func RuntimeRegistration(m *model.AIModel) (path string, variant string, grpcModelType string, err error) {
	if mode, ok := model.ResolveOutputMode(m.OutputMode); ok && mode == model.OutputModeRaw {
		return m.FilePath, "", "", nil
	}
	if model.ResolveModelType(m.ModelType) != "detection" {
		return m.FilePath, m.Variant, m.ModelType, nil
	}
	path, err = detectionRuntimePath(m)
	if err != nil {
		return "", "", "", err
	}
	variant, err = DetectionVariantJSON(m)
	if err != nil {
		return "", "", "", err
	}
	return path, variant, m.ModelType, nil
}

// detectionRuntimePath materializes a detection model under a HEF basename the
// postprocess plugin recognizes: <ModelsPath>/runtime/<model_id>/<profile>.hef
// (hardlink to the stored file, copy fallback for foreign filesystems). Files
// already carrying a recognized basename — built-in disk models — pass through
// unchanged. The DB row is never modified; re-materialization is idempotent.
func detectionRuntimePath(m *model.AIModel) (string, error) {
	base := strings.TrimSuffix(filepath.Base(m.FilePath), filepath.Ext(m.FilePath))
	if _, ok := model.LookupDetectionProfile(base); ok {
		return m.FilePath, nil
	}
	if !modelIDPattern.MatchString(m.ModelID) {
		return "", fmt.Errorf("model id %q cannot be used as a runtime directory name (allowed: letters, digits, '.', '_', '-')", m.ModelID)
	}
	profile, err := DetectionPostprocessProfile(m)
	if err != nil {
		return "", err
	}
	src := m.FilePath
	if abs, err := filepath.Abs(src); err == nil {
		src = abs
	}
	dir := filepath.Join(constants.ModelsPath(), "runtime", m.ModelID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", fmt.Errorf("failed to create runtime model dir: %w", err)
	}
	dst := filepath.Join(dir, profile+".hef")
	if err := materialize(src, dst); err != nil {
		return "", fmt.Errorf("failed to materialize model under postprocess profile: %w", err)
	}
	return dst, nil
}

// materialize places the stored HEF at dst, preferring a hardlink and falling
// back to a copy for foreign filesystems. Both variants stage into a temp name
// and rename into place: platform-api loads and app-manager preloads can
// materialize the same model concurrently, and a plain truncate-write fallback
// could clobber the shared inode (the CAS blob itself) of a hardlink another
// process just published.
//
// The temp name is pid + nanotime unique, and the copy fallback creates with
// O_EXCL. A pid-only name is not enough: a staging file left behind by a killed
// or crashed attempt is itself a hardlink to the blob, so the next same-pid
// attempt's link() hits EEXIST, falls back to the copy, and its O_TRUNC create
// zeroes that shared inode — observed on device as a CAS blob, its runtime
// copy, and two stale temp names all collapsed to one 0-byte inode. With a
// unique name the stale file is never reused, and O_EXCL makes copyInto
// structurally incapable of truncating any pre-existing path.
func materialize(src, dst string) error {
	// Already published: dst and src are hardlinks to the same stored bytes.
	// Short-circuit instead of staging — rename(tmp, dst) between two links of
	// one inode is a POSIX no-op, so a staged temp published over its own
	// source would survive as debris (and, before O_EXCL, was the trap that
	// let the next attempt's copy fallback truncate the blob).
	if sameFile(src, dst) {
		return nil
	}
	tmp := fmt.Sprintf("%s.tmp-%d-%d", dst, os.Getpid(), time.Now().UnixNano())
	if err := os.Link(src, tmp); err != nil {
		if err := copyInto(src, tmp); err != nil {
			os.Remove(tmp)
			return err
		}
	}
	if err := os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("failed to publish runtime copy: %w", err)
	}
	return nil
}

// sameFile reports whether path and candidate already refer to one inode —
// missing candidate is simply not-yet-published.
func sameFile(path, candidate string) bool {
	pathStat, err := os.Stat(path)
	if err != nil {
		return false
	}
	candidateStat, err := os.Stat(candidate)
	if err != nil {
		return false
	}
	return os.SameFile(pathStat, candidateStat)
}

// copyInto streams src into a freshly created dst. Creation is O_EXCL: this
// must never open (let alone truncate) an existing path — a dst that already
// exists could be a hardlink to the stored blob itself.
func copyInto(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return fmt.Errorf("failed to open stored model: %w", err)
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		return fmt.Errorf("failed to create runtime copy: %w", err)
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return fmt.Errorf("failed to copy model bytes: %w", err)
	}
	return out.Close()
}

// DetectionVariantJSON builds the model_variant sent to ai-runtime for
// detection models. A JSON-object variant is passed through verbatim (advanced
// escape hatch — its backend_function must match the selected profile);
// anything else is replaced with the plugin's full config blob, composed from
// the selected postprocess profile plus stored tuning values. The vendor
// postprocess plugin rejects partial JSON against its schema ("required"
// fields), so every key must be present — device-verified 2026-09-02. Of
// those keys the hailo_* functions only read detection_threshold and
// max_boxes; labels come from a compiled-in table, so the JSON labels
// (config labels with a leading placeholder, index 0) are advisory only —
// consumers map class_id N -> labels[N-1] from stored metadata. Exported
// because callers also compare variants to decide whether a loaded model
// needs reloading — this function is side-effect-free (it never touches the
// materialized runtime path), unlike RuntimeRegistration. It errors when the
// stored postprocess_profile names an unknown profile (see
// DetectionPostprocessProfile); JSON-object variants exit before that check.
func DetectionVariantJSON(m *model.AIModel) (string, error) {
	if model.ResolveModelType(m.ModelType) != "detection" {
		return m.Variant, nil
	}
	if strings.HasPrefix(strings.TrimSpace(m.Variant), "{") {
		return m.Variant, nil
	}
	profile, err := DetectionPostprocessProfile(m)
	if err != nil {
		return "", err
	}
	profileDef, _ := model.LookupDetectionProfile(profile)
	threshold := detectionThreshold(m)
	maxBoxes := m.MaxDetections
	if maxBoxes <= 0 {
		maxBoxes = defaultMaxDetections
	}
	cfg := map[string]interface{}{
		"backend_function":    profileDef.BackendFunction,
		"iou_threshold":       detectionNmsThreshold(m),
		"detection_threshold": threshold,
		"output_activation":   "none",
		"label_offset":        1,
		"max_boxes":           maxBoxes,
		"labels":              detectionLabels(m),
	}
	blob, err := json.Marshal(cfg)
	if err != nil {
		// Unreachable for these value types; keep the stored variant if so.
		return m.Variant, nil
	}
	return string(blob), nil
}

// detectionThreshold resolves the confidence threshold with explicit-zero
// support. The field schema allows threshold: 0 (retain every detection),
// so zero is honored when the row's config JSON carries the key — every
// detection row the register/update/upload paths writes does, because the
// schema default merge puts it there. Rows without the key are legacy:
// their zero column means "never set" and keeps the platform default.
func detectionThreshold(m *model.AIModel) float32 {
	if m.Config != "" {
		var cfg map[string]interface{}
		if err := json.Unmarshal([]byte(m.Config), &cfg); err == nil {
			if v, ok := cfg["threshold"].(float64); ok {
				return float32(v)
			}
		}
	}
	if m.Threshold <= 0 {
		return defaultDetectionThreshold
	}
	return m.Threshold
}

// detectionNmsThreshold reads nms_threshold from the schema-driven Config
// JSON, falling back to the schema default for rows without one. An
// explicit 0 (disable NMS filtering) is a legal schema value and is
// preserved — presence of the key decides, not the value's sign.
func detectionNmsThreshold(m *model.AIModel) float64 {
	if m.Config != "" {
		var cfg map[string]interface{}
		if err := json.Unmarshal([]byte(m.Config), &cfg); err == nil {
			if v, ok := cfg["nms_threshold"].(float64); ok {
				return v
			}
		}
	}
	return defaultNmsThreshold
}

// detectionLabels turns the comma-separated labels config into the JSON
// labels array the plugin schema requires, keeping index 0 a placeholder so
// labels[N] names class_id N. Without stored labels the reference default
// table is used.
func detectionLabels(m *model.AIModel) []string {
	if m.Config != "" {
		var cfg map[string]interface{}
		if err := json.Unmarshal([]byte(m.Config), &cfg); err == nil {
			if raw, ok := cfg["labels"].(string); ok && strings.TrimSpace(raw) != "" {
				parts := strings.Split(raw, ",")
				labels := []string{"unlabeled"}
				for _, p := range parts {
					if name := strings.TrimSpace(p); name != "" {
						labels = append(labels, name)
					}
				}
				if len(labels) > 1 {
					return labels
				}
			}
		}
	}
	return defaultPostprocessLabels
}

// RemoveRuntimeCopy deletes the materialized runtime copy of a model, if any.
// The CAS blob is not touched (its refcount is handled by the caller).
func RemoveRuntimeCopy(modelID string) {
	if !modelIDPattern.MatchString(modelID) {
		return
	}
	if err := os.RemoveAll(filepath.Join(constants.ModelsPath(), "runtime", modelID)); err != nil {
		logger.Warn("Failed to remove runtime copy for model %s: %v", modelID, err)
	}
}

// RunLoadSmokeTest probes a freshly loaded model with one zero-filled input
// tensor. Detection postprocess failures (NMS tensor-name mismatches in the
// vendor plugin) only surface at infer time — RegisterModel succeeds and
// every frame then fails — so a probe infer at load time catches them while
// the caller can still roll the registration back. A missing modelInfo skips
// the probe (nothing to size a tensor from).
func RunLoadSmokeTest(ctx context.Context, client inferencepb.InferenceServiceClient, modelID string, modelInfo *inferencepb.ModelInfo) error {
	if modelInfo == nil || len(modelInfo.Inputs) == 0 {
		logger.Warn("Skipping load smoke test for %s: no input tensor info available", modelID)
		return nil
	}
	tensor, err := zeroInputTensor(modelInfo.Inputs[0])
	if err != nil {
		return fmt.Errorf("cannot build probe tensor: %w", err)
	}
	smokeCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	resp, err := client.Infer(smokeCtx, &inferencepb.InferRequest{
		ModelId:   modelID,
		Inputs:    []*inferencepb.Tensor{tensor},
		TimeoutMs: 10000,
	})
	if err != nil {
		return fmt.Errorf("probe infer failed: %w", err)
	}
	if resp.GetStatus() != nil && !resp.GetStatus().GetSuccess() {
		return fmt.Errorf("%s", resp.GetStatus().GetMessage())
	}
	return nil
}

func dtypeSize(dt inferencepb.DataType) int {
	switch dt {
	case inferencepb.DataType_UINT8, inferencepb.DataType_INT8:
		return 1
	case inferencepb.DataType_UINT16, inferencepb.DataType_INT16, inferencepb.DataType_FLOAT16:
		return 2
	case inferencepb.DataType_FLOAT32, inferencepb.DataType_INT32, inferencepb.DataType_UINT32:
		return 4
	default:
		return 0
	}
}

// zeroInputTensor mirrors an input TensorSpec into a zero-filled Tensor. The
// size comes from the spec's byte_size when the runtime provides it — NV12
// inputs report an RGB-like shape whose product is not the host frame size
// (W*H*3/2 is) — falling back to shape × dtype for specs without one.
func zeroInputTensor(spec *inferencepb.TensorSpec) (*inferencepb.Tensor, error) {
	total := int(spec.GetByteSize())
	if total == 0 {
		elem := dtypeSize(spec.GetDtype())
		if elem == 0 {
			return nil, fmt.Errorf("unsupported input dtype %v", spec.GetDtype())
		}
		total = elem
		for _, d := range spec.GetShape() {
			if d <= 0 {
				return nil, fmt.Errorf("invalid input shape %v", spec.GetShape())
			}
			total *= int(d)
		}
	}
	return &inferencepb.Tensor{
		Shape: spec.GetShape(),
		Dtype: spec.GetDtype(),
		Data:  make([]byte, total),
	}, nil
}
