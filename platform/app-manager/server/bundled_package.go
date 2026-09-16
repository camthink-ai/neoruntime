// Bundled private-model packages: unpacking the AMPK .bin containers that
// app manifests declare under spec.models.<alias>.path. Install (extractImageModels)
// turns each package into an on-disk HEF plus this sidecar record; PreloadModels
// reads the sidecar back at every reboot to re-register the model with the
// exact same gRPC registration (the .bin itself is deleted after unpack, so
// the registration cannot be re-derived from it).
package server

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/platform/app-manager/manifest"
	"aipc/platform/common/logger"
	"aipc/platform/modelload"
	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/storage"
)

// bundledRegistrationFile is the sidecar written next to the unpacked HEF.
const bundledRegistrationFile = "registration.json"

// bundledRegistration is the durable record of one unpacked bundled model.
// ModelType/ModelVariant are the gRPC registration values modelload composed
// at install time (empty ModelType = raw output, no postprocess session);
// RawOutputOnly records that the emptiness is the package's declared
// output_mode=raw choice, not a missing type — the runtime's transient gate
// wants the explicit opt-in before accepting a typeless registration.
type bundledRegistration struct {
	ModelID       string `json:"model_id"`
	HEF           string `json:"hef"` // basename, inside app-models/<app>/<alias>/
	ModelType     string `json:"model_type"`
	ModelVariant  string `json:"model_variant,omitempty"`
	RawOutputOnly bool   `json:"raw_output_only,omitempty"`
}

// unpackBundledPackage opens the AMPK package at binPath, verifies it, stages
// the embedded HEF into aliasDir and composes the gRPC registration the same
// way platform-api's RegisterModel API does (schema defaults merged with the
// package config, threshold/max_detections lifted into the columns the
// variant composer reads). Detection models in platform mode are staged under
// their postprocess profile basename, so modelload.RuntimeRegistration passes
// the file through unchanged — no copy under /data models runtime/.
func unpackBundledPackage(binPath, aliasDir, modelID string) (*bundledRegistration, error) {
	f, err := os.Open(binPath)
	if err != nil {
		return nil, fmt.Errorf("failed to open bundled package: %w", err)
	}
	defer f.Close()
	pr, err := storage.OpenPackage(f)
	if err != nil {
		return nil, err
	}
	meta := pr.Meta()

	modelType := model.ResolveModelType(meta.ModelType)
	if modelType == "" {
		return nil, fmt.Errorf("bundled package declares unknown model_type %q", meta.ModelType)
	}
	outputMode, ok := model.ResolveOutputMode(meta.OutputMode)
	if !ok {
		return nil, fmt.Errorf("bundled package declares unsupported output_mode %q", meta.OutputMode)
	}

	merged := model.GetFieldDefaults(modelType)
	if merged == nil {
		merged = make(map[string]interface{})
	}
	if len(meta.Config) > 0 {
		var pkgCfg map[string]interface{}
		if err := json.Unmarshal(meta.Config, &pkgCfg); err != nil {
			return nil, fmt.Errorf("bundled package config is not a JSON object: %w", err)
		}
		for k, v := range pkgCfg {
			merged[k] = v
		}
	}
	configJSON, err := json.Marshal(merged)
	if err != nil {
		return nil, fmt.Errorf("failed to re-encode merged config: %w", err)
	}

	synth := &model.AIModel{
		ModelID:    modelID,
		FilePath:   "", // filled in once the destination basename is known
		ModelType:  modelType,
		OutputMode: outputMode,
		Config:     string(configJSON),
	}
	// Column lift, mirroring RegisterModel: DetectionVariantJSON composes from
	// the Threshold/MaxDetections columns, not from Config.
	if v, ok := merged["threshold"].(float64); ok {
		synth.Threshold = float32(v)
	}
	if v, ok := merged["max_detections"].(float64); ok {
		synth.MaxDetections = int(v)
	}

	base, err := bundledHEFBasename(synth, meta, modelID)
	if err != nil {
		return nil, fmt.Errorf("bundled package postprocess_profile is not usable: %w", err)
	}
	synth.FilePath = filepath.Join(aliasDir, base)

	tmp := synth.FilePath + ".tmp"
	if err := writeVerifiedHEF(tmp, pr); err != nil {
		os.Remove(tmp)
		return nil, err
	}
	if err := os.Rename(tmp, synth.FilePath); err != nil {
		os.Remove(tmp)
		return nil, fmt.Errorf("failed to publish bundled HEF: %w", err)
	}

	hefPath, variant, grpcType, err := modelload.RuntimeRegistration(synth)
	if err != nil {
		return nil, fmt.Errorf("failed to compose runtime registration: %w", err)
	}

	reg := &bundledRegistration{
		ModelID:       modelID,
		HEF:           filepath.Base(hefPath),
		ModelType:     grpcType,
		ModelVariant:  variant,
		RawOutputOnly: grpcType == "" && outputMode == model.OutputModeRaw,
	}
	blob, err := json.MarshalIndent(reg, "", "  ")
	if err != nil {
		return nil, fmt.Errorf("failed to encode registration record: %w", err)
	}
	if err := os.WriteFile(filepath.Join(aliasDir, bundledRegistrationFile), blob, 0o644); err != nil {
		return nil, fmt.Errorf("failed to persist registration record: %w", err)
	}
	return reg, nil
}

// bundledHEFBasename picks the on-disk name for the unpacked HEF. Detection
// models in platform mode take their postprocess profile basename (the
// plugin-basename passthrough in modelload); everything else keeps the
// package's original filename when it is a usable .hef name. A package whose
// config names an unknown postprocess_profile is an error — installing it
// would silently mismatch the model to the default profile later.
func bundledHEFBasename(synth *model.AIModel, meta *storage.PackageMeta, modelID string) (string, error) {
	if synth.ModelType == "detection" && synth.OutputMode == model.OutputModePlatform {
		profile, err := modelload.DetectionPostprocessProfile(synth)
		if err != nil {
			return "", err
		}
		return profile + ".hef", nil
	}
	dest := filepath.Base(meta.HEF.Filename)
	if filepath.Ext(dest) != ".hef" {
		dest = safeHEFBasename(modelID)
	}
	return dest, nil
}

// safeHEFBasename derives a fallback filename from the model id; ids that
// cannot serve as a filename (empty, dot-only, path separators) degrade to a
// constant so the name always stays a single path segment.
func safeHEFBasename(modelID string) string {
	if modelID != "" && modelID != "." && modelID != ".." && !strings.ContainsAny(modelID, `/\`) {
		return modelID + ".hef"
	}
	return "bundled.hef"
}

// writeVerifiedHEF streams the package's HEF section into path and verifies
// the package digest once every byte is staged. path is a staging name: the
// caller publishes it by rename and removes it on failure, so the bytes never
// sit at their final path before Verify passes.
func writeVerifiedHEF(path string, pr *storage.PackageReader) error {
	out, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return fmt.Errorf("failed to stage bundled HEF: %w", err)
	}
	if _, err := io.Copy(out, pr.HEF()); err != nil {
		out.Close()
		return fmt.Errorf("failed to unpack bundled HEF: %w", err)
	}
	if err := out.Close(); err != nil {
		return fmt.Errorf("failed to close staged HEF: %w", err)
	}
	if err := pr.Verify(); err != nil {
		return err
	}
	return nil
}

// loadBundledRegistration reads the sidecar unpackBundledPackage wrote. It is
// the authoritative registration record at reboot: the .bin was deleted after
// unpack, so the composed type/variant cannot be re-derived.
func loadBundledRegistration(aliasDir string) (*bundledRegistration, error) {
	blob, err := os.ReadFile(filepath.Join(aliasDir, bundledRegistrationFile))
	if err != nil {
		return nil, fmt.Errorf("bundled registration record missing: %w", err)
	}
	reg := &bundledRegistration{}
	if err := json.Unmarshal(blob, reg); err != nil {
		return nil, fmt.Errorf("bundled registration record corrupt: %w", err)
	}
	if reg.ModelID == "" || reg.HEF == "" || reg.HEF == "." || reg.HEF == ".." || strings.ContainsAny(reg.HEF, `/\`) {
		return nil, fmt.Errorf("bundled registration record incomplete")
	}
	return reg, nil
}

// bundledPackageHEFHash returns the sha256 of the HEF embedded in the AMPK
// package at path, computed fresh from the stream while the package digest is
// verified (a corrupted package is an error, never a hash). Used to compare a
// bundled package against a platform HEF: the container bytes would never
// match, the inner HEF bytes do.
// bundledModelTransaction stages a complete replacement tree beside the
// canonical app-models directory. The old tree and runtime registrations stay
// untouched until every package has been extracted and verified.
type bundledModelTransaction struct {
	s          *AppManagerServer
	appID      string
	canonical  string
	stage      string
	backup     string
	oldRegs    []*inferencepb.ModelRegisterRequest
	items      []pendingBundledModel
	registered []string
	succeeded  int
	published  bool
	task       *InstallTask
}

func (s *AppManagerServer) prepareBundledModelTransaction(ctx context.Context, appID string, appManifest *manifest.AppManifest, pending []pendingBundledModel, task *InstallTask) (*bundledModelTransaction, error) {
	if err := requireSafePathSegment("app id", appID); err != nil {
		return nil, err
	}
	for _, p := range pending {
		if err := requireSafePathSegment("model alias", p.alias); err != nil {
			return nil, err
		}
	}

	base := appModelsDir(appID)
	parent := filepath.Dir(base)
	if err := os.MkdirAll(parent, 0o755); err != nil {
		return nil, fmt.Errorf("create app-models root: %w", err)
	}
	stage, err := os.MkdirTemp(parent, "."+appID+".staging-")
	if err != nil {
		return nil, fmt.Errorf("create bundled model staging tree: %w", err)
	}
	tx := &bundledModelTransaction{s: s, appID: appID, canonical: base, stage: stage, task: task}
	cleanup := true
	defer func() {
		if cleanup {
			_ = os.RemoveAll(stage)
		}
	}()

	// Snapshot durable registrations before changing either disk or runtime.
	entries, readErr := os.ReadDir(base)
	if readErr != nil && !os.IsNotExist(readErr) {
		return nil, fmt.Errorf("snapshot old bundled model tree: %w", readErr)
	}
	if readErr == nil {
		sort.Slice(entries, func(i, j int) bool { return entries[i].Name() < entries[j].Name() })
		oldByID := make(map[string]*inferencepb.ModelRegisterRequest)
		for _, entry := range entries {
			if !entry.IsDir() {
				continue
			}
			aliasDir := filepath.Join(base, entry.Name())
			reg, err := loadBundledRegistration(aliasDir)
			if err != nil {
				return nil, fmt.Errorf("snapshot old bundled model %q: %w", entry.Name(), err)
			}
			hefPath := filepath.Join(aliasDir, reg.HEF)
			if _, err := os.Stat(hefPath); err != nil {
				return nil, fmt.Errorf("snapshot old bundled model %q HEF: %w", entry.Name(), err)
			}
			req := bundledRegisterRequest(appID, reg, hefPath)
			if prior, ok := oldByID[reg.ModelID]; ok {
				if prior.ModelPath != req.ModelPath || prior.ModelType != req.ModelType || prior.ModelVariant != req.ModelVariant || prior.RawOutputOnly != req.RawOutputOnly {
					return nil, fmt.Errorf("snapshot old bundled model tree has conflicting paths or registrations for id %q", reg.ModelID)
				}
				continue
			}
			oldByID[reg.ModelID] = req
			tx.oldRegs = append(tx.oldRegs, req)
		}
	}

	if len(pending) > 0 {
		s.aiRuntimeMutex.RLock()
		client := s.aiRuntimeClient
		s.aiRuntimeMutex.RUnlock()
		available := client != nil && s.config.AIRuntime.Enabled && s.extractModelFile != nil
		refs := appManifest.ImageReferences()
		var requiredErrs, warnings []string
		seen := make(map[string]pendingBundledModel)
		for _, p := range pending {
			if prior, ok := seen[p.id]; ok {
				if prior.path != p.path {
					return nil, fmt.Errorf("model id %q has conflicting bundled paths", p.id)
				}
				continue
			}
			seen[p.id] = p
			fail := func(msg string) {
				if p.required {
					requiredErrs = append(requiredErrs, "required "+msg)
				} else {
					warnings = append(warnings, "optional "+msg)
				}
			}
			if !available {
				fail(fmt.Sprintf("model %q (alias %q) cannot be prepared (ai-runtime or containerd unavailable)", p.id, p.alias))
				continue
			}
			if len(refs) == 0 {
				fail(fmt.Sprintf("model %q (alias %q): app declares no image", p.id, p.alias))
				continue
			}
			aliasDir := filepath.Join(stage, p.alias)
			binPath, err := s.extractModelFile(ctx, refs[0], p.path, aliasDir)
			if err != nil {
				_ = os.RemoveAll(aliasDir)
				fail(fmt.Sprintf("model %q (alias %q): extract %q failed: %v", p.id, p.alias, p.path, err))
				continue
			}
			reg, err := unpackBundledPackage(binPath, aliasDir, p.id)
			_ = os.Remove(binPath)
			if err != nil {
				_ = os.RemoveAll(aliasDir)
				fail(fmt.Sprintf("model %q (alias %q): package validation failed: %v", p.id, p.alias, err))
				continue
			}
			p.request = bundledRegisterRequest(appID, reg, filepath.Join(base, p.alias, reg.HEF))
			tx.items = append(tx.items, p)
		}
		if err := reportModelValidationAt("registering", 82, requiredErrs, warnings, task); err != nil {
			return nil, err
		}
	}
	cleanup = false
	return tx, nil
}

func bundledRegisterRequest(appID string, reg *bundledRegistration, hefPath string) *inferencepb.ModelRegisterRequest {
	return &inferencepb.ModelRegisterRequest{ModelId: reg.ModelID, ModelPath: hefPath, OwnerId: appID, ModelType: reg.ModelType, ModelVariant: reg.ModelVariant, Transient: true, RawOutputOnly: reg.RawOutputOnly}
}

func registerRequest(ctx context.Context, client inferencepb.InferenceServiceClient, req *inferencepb.ModelRegisterRequest) error {
	resp, err := client.RegisterModel(ctx, req)
	if err != nil {
		return err
	}
	if resp == nil || resp.Status == nil || !resp.Status.Success {
		message := "missing runtime status"
		if resp != nil && resp.Status != nil && resp.Status.Message != "" {
			message = resp.Status.Message
		}
		return fmt.Errorf("runtime refused registration: %s", message)
	}
	return nil
}

// Publish performs the deliberately narrow destructive window: old ownership
// must be released and physical absence proven before the tree swap and new
// registrations. Any failure restores released ownership and/or the old tree.
func (tx *bundledModelTransaction) Publish(ctx context.Context) error {
	tx.s.aiRuntimeMutex.RLock()
	client := tx.s.aiRuntimeClient
	tx.s.aiRuntimeMutex.RUnlock()
	if client == nil || !tx.s.config.AIRuntime.Enabled {
		if len(tx.oldRegs)+len(tx.items) == 0 {
			return nil
		}
		return fmt.Errorf("ai-runtime is not available")
	}

	released := make([]*inferencepb.ModelRegisterRequest, 0, len(tx.oldRegs))
	restoreReleased := func() {
		for _, req := range released {
			if err := registerRequest(ctx, client, req); err != nil {
				logger.Error("Rollback: failed to restore ownership of model %s for app %s: %v", req.ModelId, tx.appID, err)
			}
		}
	}
	seen := make(map[string]bool)
	for _, req := range tx.oldRegs {
		if seen[req.ModelId] {
			continue
		}
		seen[req.ModelId] = true

		// A solo ai-runtime restart legitimately loses the old transient
		// registration while the durable app-models tree survives. In that case
		// there is nothing to quiesce and replacement may proceed.
		_, infoErr := client.GetModelInfo(ctx, &inferencepb.ModelInfo{ModelId: req.ModelId})
		if status.Code(infoErr) == codes.NotFound {
			continue
		}
		if infoErr != nil {
			restoreReleased()
			return fmt.Errorf("cannot inspect old bundled model %s before replacement: %w", req.ModelId, infoErr)
		}

		resp, err := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: req.ModelId, OwnerId: tx.appID})
		if err != nil {
			restoreReleased()
			return fmt.Errorf("unregister old bundled model %s: %w", req.ModelId, err)
		}
		if resp == nil || !resp.Success {
			restoreReleased()
			return fmt.Errorf("runtime refused to unregister old bundled model %s: %s", req.ModelId, resp.GetMessage())
		}
		// Logical success commits this app's owner release even when another
		// owner keeps the physical entry resident. Track it before the absence
		// check so a co-owner veto restores this app's ownership too.
		released = append(released, req)
		_, err = client.GetModelInfo(ctx, &inferencepb.ModelInfo{ModelId: req.ModelId})
		if status.Code(err) != codes.NotFound {
			restoreReleased()
			if err == nil {
				return fmt.Errorf("old bundled model %s is still physically registered (busy or co-owned)", req.ModelId)
			}
			return fmt.Errorf("cannot confirm physical removal of old bundled model %s: %w", req.ModelId, err)
		}
	}

	if _, err := os.Stat(tx.canonical); err == nil {
		backup, mkErr := os.MkdirTemp(filepath.Dir(tx.canonical), "."+tx.appID+".backup-")
		if mkErr != nil {
			restoreReleased()
			return fmt.Errorf("create old bundled tree backup: %w", mkErr)
		}
		_ = os.Remove(backup)
		if err := os.Rename(tx.canonical, backup); err != nil {
			restoreReleased()
			return fmt.Errorf("backup old bundled tree: %w", err)
		}
		tx.backup = backup
	} else if !os.IsNotExist(err) {
		restoreReleased()
		return fmt.Errorf("inspect old bundled tree: %w", err)
	}
	if err := os.Rename(tx.stage, tx.canonical); err != nil {
		if tx.backup != "" {
			_ = os.Rename(tx.backup, tx.canonical)
		}
		restoreReleased()
		return fmt.Errorf("publish staged bundled tree: %w", err)
	}
	tx.stage = ""
	tx.published = true

	for _, item := range tx.items {
		req := item.request
		tx.registered = append(tx.registered, req.ModelId)
		failure := registerRequest(ctx, client, req)
		if failure == nil && req.ModelType == "detection" {
			failure = tx.s.probeFreshRegistration(ctx, client, tx.appID, req.ModelId)
		}
		if failure == nil {
			tx.succeeded++
			continue
		}
		if item.required {
			return fmt.Errorf("required new bundled model %s (alias %q) publish failed: %w", req.ModelId, item.alias, failure)
		}
		_, _ = client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: req.ModelId, OwnerId: tx.appID})
		// A transport failure may have occurred after runtime acceptance, and a
		// smoke failure may already have unregistered the model. Only the final
		// runtime state is authoritative: never delete an optional HEF while a
		// registration may still reference it.
		_, infoErr := client.GetModelInfo(ctx, &inferencepb.ModelInfo{ModelId: req.ModelId})
		if status.Code(infoErr) != codes.NotFound {
			return fmt.Errorf("optional new bundled model %s cleanup could not confirm runtime removal after publish failure: %w", req.ModelId, failure)
		}
		if err := os.RemoveAll(filepath.Join(tx.canonical, item.alias)); err != nil {
			return fmt.Errorf("remove failed optional bundled model %s: %w", req.ModelId, err)
		}
		for i, id := range tx.registered {
			if id == req.ModelId {
				tx.registered = append(tx.registered[:i], tx.registered[i+1:]...)
				break
			}
		}
		reportModelValidationAt("registering", 82, nil, []string{fmt.Sprintf("optional model %q (alias %q) publish failed and was removed: %v", req.ModelId, item.alias, failure)}, tx.task)
	}
	return nil
}

func (tx *bundledModelTransaction) Rollback(_ context.Context) {
	if tx == nil {
		return
	}
	// Compensation must outlive the initiating HTTP/gRPC request. A client
	// cancellation or exhausted publish deadline must not prevent releasing a
	// partially accepted new registration and restoring the old tree/runtime.
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	wasPublished := tx.published
	tx.s.aiRuntimeMutex.RLock()
	client := tx.s.aiRuntimeClient
	tx.s.aiRuntimeMutex.RUnlock()
	runtimeClear := true
	if client != nil {
		seen := make(map[string]bool)
		for _, id := range tx.registered {
			if seen[id] {
				continue
			}
			seen[id] = true
			resp, unregErr := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: id, OwnerId: tx.appID})
			// GetModelInfo is authoritative: the smoke-test helper may already
			// have rolled this registration back, in which case a second
			// UnregisterModel legitimately reports failure/not-found.
			_, infoErr := client.GetModelInfo(ctx, &inferencepb.ModelInfo{ModelId: id})
			if status.Code(infoErr) == codes.NotFound {
				continue
			}
			runtimeClear = false
			if unregErr != nil || resp == nil || !resp.Success {
				logger.Error("Rollback: cannot release new bundled model %s for app %s: rpc=%v status=%v", id, tx.appID, unregErr, resp)
			}
			logger.Error("Rollback: new bundled model %s for app %s remains registered; preserving its HEF and old backup", id, tx.appID)
		}
	}
	if tx.published && runtimeClear {
		if err := os.RemoveAll(tx.canonical); err != nil {
			runtimeClear = false
			logger.Error("Rollback: failed to remove new bundled model tree for app %s: %v", tx.appID, err)
		}
		if runtimeClear && tx.backup != "" {
			if err := os.Rename(tx.backup, tx.canonical); err != nil {
				runtimeClear = false
				logger.Error("Rollback: failed to restore bundled model tree for app %s: %v", tx.appID, err)
			}
		}
	}
	if client != nil && wasPublished && runtimeClear {
		for _, req := range tx.oldRegs {
			if err := registerRequest(ctx, client, req); err != nil {
				logger.Error("Rollback: failed to restore bundled model %s for app %s: %v", req.ModelId, tx.appID, err)
			}
		}
	}
	if tx.stage != "" {
		_ = os.RemoveAll(tx.stage)
	}
	tx.published = false
}

func (tx *bundledModelTransaction) Commit() {
	if tx == nil {
		return
	}
	if tx.backup != "" {
		_ = os.RemoveAll(tx.backup)
	}
	if tx.stage != "" {
		_ = os.RemoveAll(tx.stage)
	}
	if tx.succeeded == 0 {
		_ = os.RemoveAll(tx.canonical)
	}
}

func bundledPackageHEFHash(binPath string) (string, error) {
	f, err := os.Open(binPath)
	if err != nil {
		return "", fmt.Errorf("failed to open bundled package: %w", err)
	}
	defer f.Close()
	pr, err := storage.OpenPackage(f)
	if err != nil {
		return "", err
	}
	h := sha256.New()
	if _, err := io.Copy(h, pr.HEF()); err != nil {
		return "", fmt.Errorf("failed to read bundled HEF: %w", err)
	}
	if err := pr.Verify(); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
