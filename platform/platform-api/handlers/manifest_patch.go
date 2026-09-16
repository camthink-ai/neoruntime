package handlers

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/gin-gonic/gin"

	"aipc/platform/app-manager/manifest"
	"aipc/platform/common/logger"
)

// patchableFields is the wizard-expressible subset of the manifest. Paths not
// listed here are rejected:
//   - metadata.id is the manifest directory name (apps/manifests/<id>/);
//     renaming it would orphan the directory and break install/permission
//     lookups
//   - spec.image must keep matching the uploaded tar's RepoTag; changing it
//     would break the import reconciliation
//   - spec.models is patchable only as a whole map — the wizard's model
//     dependency editor replaces the full alias→mapping set in one op.
//     Per-alias subpaths (spec.models.<alias>.id and friends) stay rejected:
//     partial edits could silently drop path/type/required subfields the
//     form cannot express
var patchableFields = map[string]bool{
	"metadata.name":        true,
	"metadata.version":     true,
	"metadata.description": true,

	"spec.resources.cpu":    true,
	"spec.resources.memory": true,
	"spec.autostart":        true,
	"spec.restart_policy":   true,

	"spec.models": true,

	"spec.permissions.video":                          true,
	"spec.permissions.inference.models":               true,
	"spec.permissions.inference.max_qps":              true,
	"spec.permissions.inference.max_concurrent":       true,
	"spec.permissions.inference.allow_register_model": true,
	"spec.permissions.events.publish":                 true,
	"spec.permissions.events.subscribe":               true,
	"spec.permissions.device.light":                   true,
	"spec.permissions.device.ir_cut":                  true,
	"spec.permissions.device.ptz":                     true,
	"spec.permissions.device.lens":                    true,
	"spec.permissions.network.mode":                   true,
	"spec.permissions.network.inbound":                true,

	"spec.env":     true,
	"spec.volumes": true,

	"spec.security.no_new_privileges": true,
	"spec.security.readonly_rootfs":   true,
}

// ManifestPatchRequest is the body of PATCH /api/v1/apps/manifest: the stored
// manifest to edit plus field edits as dotted-path → JSON value pairs.
type ManifestPatchRequest struct {
	ManifestPath string                     `json:"manifest_path"`
	Fields       map[string]json.RawMessage `json:"fields"`
}

// PatchManifest applies field-level edits to a stored manifest, preserving
// comments, unknown fields and key order (see manifest.PatchDocument).
// PATCH /api/v1/apps/manifest
func (h *APIHandlers) PatchManifest(c *gin.Context) {
	var req ManifestPatchRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}
	if req.ManifestPath == "" {
		Resp(c).FailMsg(CodeMissingParameter, "manifest_path is required")
		return
	}
	if len(req.Fields) == 0 {
		Resp(c).FailMsg(CodeMissingParameter, "fields must contain at least one edit")
		return
	}

	// Editing is staging-only. Canonical live manifests are immutable through
	// this endpoint and are published solely by app-manager.
	manifestPath, err := safeStagingManifest(req.ManifestPath)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}

	// Whitelist first — reject the whole request if any field is off-limits,
	// so a partial acceptance can never surprise the caller.
	ops := make([]manifest.FieldPatch, 0, len(req.Fields))
	var rejected []string
	for path, value := range req.Fields {
		if !patchableFields[path] {
			rejected = append(rejected, path)
			continue
		}
		ops = append(ops, manifest.FieldPatch{Path: path, Value: value})
	}
	if len(rejected) > 0 {
		sort.Strings(rejected)
		Resp(c).FailMsg(CodeInvalidParameter,
			fmt.Sprintf("Non-patchable fields: %s", strings.Join(rejected, ", ")))
		return
	}
	// Deterministic application order: the written file must not depend on
	// Go map iteration order.
	sort.Slice(ops, func(i, j int) bool { return ops[i].Path < ops[j].Path })

	src, err := os.ReadFile(manifestPath)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Failed to read manifest: "+err.Error())
		return
	}

	appManifest, err := manifest.ParseManifest(src)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Stored manifest no longer parses: "+err.Error())
		return
	}
	// The wizard cannot express spec.containers; letting it patch a
	// multi-container manifest would imply an edit surface that does not
	// exist.
	if appManifest.IsMultiContainer() {
		Resp(c).FailMsg(CodeInvalidRequest,
			"Multi-container manifests cannot be wizard-edited (spec.containers is not wizard-expressible)")
		return
	}

	patched, err := manifest.PatchDocument(src, ops)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Patch failed: "+err.Error())
		return
	}

	// The patched bytes must still be a valid manifest — the disk file stays
	// untouched until this passes.
	patchedManifest, err := manifest.ParseManifest(patched)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Patched manifest is invalid: "+err.Error())
		return
	}

	// Atomic write: use a unique sibling temporary file so concurrent requests
	// cannot clobber one another's fixed .tmp name.
	tmp, err := os.CreateTemp(filepath.Dir(manifestPath), ".app.yaml-*")
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create manifest temp file: "+err.Error())
		return
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(0644); err != nil {
		tmp.Close()
		Resp(c).FailMsg(CodeServiceError, "Failed to set manifest permissions: "+err.Error())
		return
	}
	if _, err := tmp.Write(patched); err != nil {
		tmp.Close()
		Resp(c).FailMsg(CodeServiceError, "Failed to write manifest: "+err.Error())
		return
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		Resp(c).FailMsg(CodeServiceError, "Failed to sync manifest: "+err.Error())
		return
	}
	if err := tmp.Close(); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to close manifest: "+err.Error())
		return
	}
	if err := os.Rename(tmpName, manifestPath); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to replace manifest: "+err.Error())
		return
	}

	logger.Info("Manifest patched: %s (%d fields)", manifestPath, len(ops))

	Resp(c).OK(gin.H{
		"path":            manifestPath,
		"manifest":        patchedManifest,
		"multi_container": patchedManifest.IsMultiContainer(),
	})
}
