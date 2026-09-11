package handlers

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"gopkg.in/yaml.v3"

	"aipc/platform/app-manager/manifest"
	apppb "aipc/platform/app-manager/proto"
)

// WizardInstall handles app installation via wizard configuration
func (h *APIHandlers) WizardInstall(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	var req WizardRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	// Validate required fields
	if req.Metadata.ID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "metadata.id is required")
		return
	}
	// The ID becomes a directory name under apps/manifests — reject path
	// segments before any MkdirAll/WriteFile happens.
	if err := requireSafeAppID(req.Metadata.ID); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	if req.Metadata.Name == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "metadata.name is required")
		return
	}
	if req.Metadata.Version == "" {
		req.Metadata.Version = "1.0.0"
	}
	if req.Image == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "image is required")
		return
	}

	// Generate YAML content
	yamlData, err := h.generateAppYAML(&req)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to generate manifest: "+err.Error())
		return
	}

	// Generate into request-private staging; app-manager publishes the canonical
	// manifest only after the install has passed validation.
	manifestDir, err := newAppStagingDir()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create staging directory: "+err.Error())
		return
	}
	manifestFile := filepath.Join(manifestDir, "app.yaml")
	if err := os.WriteFile(manifestFile, yamlData, 0644); err != nil {
		os.RemoveAll(manifestDir)
		Resp(c).FailMsg(CodeServiceError, "Failed to create manifest file: "+err.Error())
		return
	}

	// Auto-populate ImagePath from Image if not explicitly set.
	// The frontend wizard only sends the "image" field (e.g. "nginx/nginx-ingress:edge-alpine").
	// If it looks like a remote registry reference (not a local .tar file), use it
	// as ImagePath so that InstallApp triggers the pull from the registry.
	imagePath := req.ImagePath
	if imagePath == "" && req.Image != "" {
		isLocalFile := strings.HasSuffix(req.Image, ".tar") ||
			strings.HasSuffix(req.Image, ".tar.gz") ||
			strings.HasSuffix(req.Image, ".tgz") ||
			strings.HasPrefix(req.Image, "/") ||
			strings.HasPrefix(req.Image, "./")
		if !isLocalFile {
			imagePath = req.Image
		}
	}

	// Call AsyncInstallApp gRPC — returns task_id immediately
	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.AsyncInstallApp(ctx, &apppb.AsyncInstallRequest{
		ManifestPath: manifestFile,
		ImagePath:    imagePath,
		Force:        req.Force,
	})
	if err != nil {
		os.RemoveAll(manifestDir)
		Resp(c).FailMsg(CodeAppInstallFailed, err.Error())
		return
	}

	Resp(c).OK(gin.H{"task_id": resp.TaskId})
}

// WizardRequest represents the wizard installation request
type WizardRequest struct {
	Metadata      WizardMetadata    `json:"metadata"`
	Image         string            `json:"image"`
	ImagePath     string            `json:"image_path,omitempty"`
	Resources     WizardResources   `json:"resources,omitempty"`
	Permissions   WizardPermissions `json:"permissions,omitempty"`
	Env           []WizardEnvVar    `json:"env,omitempty"`
	Volumes       []WizardVolume    `json:"volumes,omitempty"`
	Autostart     bool              `json:"autostart,omitempty"`
	RestartPolicy string            `json:"restart_policy,omitempty"`
	Security      WizardSecurity    `json:"security,omitempty"`
	Force         bool              `json:"force,omitempty"`

	// Models is the declarative model dependency map (spec.models): alias →
	// model id (+ optional in-image path/type, required flag). The web wizard's
	// 模型依赖 editor sends this; permissions.inference.models stays as the
	// legacy authorization list and is no longer sent by the wizard.
	Models map[string]manifest.ModelMapping `json:"models,omitempty"`
}

type WizardMetadata struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Version     string `json:"version"`
	Description string `json:"description"`
}

type WizardResources struct {
	CPU    string `json:"cpu,omitempty"`
	Memory string `json:"memory,omitempty"`
}

type WizardPermissions struct {
	Video     []string         `json:"video,omitempty"`
	Inference *WizardInference `json:"inference,omitempty"`
	Events    *WizardEvents    `json:"events,omitempty"`
	Device    *WizardDevice    `json:"device,omitempty"`
	Network   *WizardNetwork   `json:"network,omitempty"`
}

type WizardInference struct {
	// Models is the legacy authorization list. The web wizard now sends the
	// declarative top-level `models` map instead; this field stays accepted so
	// older API clients keep installing.
	Models        []string `json:"models,omitempty"`
	MaxQPS        int      `json:"max_qps,omitempty"`
	MaxConcurrent int      `json:"max_concurrent,omitempty"`
	AllowRegister bool     `json:"allow_register_model,omitempty"`
}

type WizardEvents struct {
	Publish   []string `json:"publish,omitempty"`
	Subscribe []string `json:"subscribe,omitempty"`
}

type WizardDevice struct {
	Light bool `json:"light,omitempty"`
	IrCut bool `json:"ir_cut,omitempty"`
	PTZ   bool `json:"ptz,omitempty"`
	Lens  bool `json:"lens,omitempty"`
}

type WizardNetwork struct {
	Mode    string `json:"mode,omitempty"`
	Inbound []int  `json:"inbound,omitempty"`
}

type WizardEnvVar struct {
	Name  string `json:"name"`
	Value string `json:"value"`
}

type WizardVolume struct {
	Host      string `json:"host"`
	Container string `json:"container"`
	ReadOnly  bool   `json:"readonly,omitempty"`
}

type WizardSecurity struct {
	NoNewPrivileges *bool `json:"no_new_privileges,omitempty"`
	ReadonlyRootfs  *bool `json:"readonly_rootfs,omitempty"`
}

// wizardRequestToManifest maps a WizardRequest onto the canonical manifest
// struct. Wizard pointer semantics pass through untouched: SecuritySpec uses
// the same *bool convention (nil = platform default), so an unset field stays
// nil instead of becoming an explicit false.
func wizardRequestToManifest(req *WizardRequest) *manifest.AppManifest {
	m := &manifest.AppManifest{
		APIVersion: "v1",
		Kind:       "Application",
		Metadata: manifest.Metadata{
			ID:          req.Metadata.ID,
			Name:        req.Metadata.Name,
			Version:     req.Metadata.Version,
			Description: req.Metadata.Description,
		},
		Spec: manifest.Spec{
			Image:         req.Image,
			Autostart:     req.Autostart,
			RestartPolicy: req.RestartPolicy,
		},
	}

	if req.Resources.CPU != "" || req.Resources.Memory != "" {
		m.Spec.Resources = manifest.Resources{
			CPU:    req.Resources.CPU,
			Memory: req.Resources.Memory,
		}
	}

	// Model dependencies land in spec.models verbatim — including path/type/
	// required subfields the form itself may not edit but must not drop.
	if len(req.Models) > 0 {
		m.Spec.Models = req.Models
	}

	if p := req.Permissions; p.Video != nil || p.Inference != nil || p.Events != nil || p.Device != nil || p.Network != nil {
		perms := manifest.Permissions{Video: p.Video}
		if p.Inference != nil {
			perms.Inference = manifest.InferencePerms{
				Models:        p.Inference.Models,
				MaxQPS:        p.Inference.MaxQPS,
				MaxConcurrent: p.Inference.MaxConcurrent,
				AllowRegister: p.Inference.AllowRegister,
			}
		}
		if p.Events != nil {
			perms.Events = manifest.EventPerms{
				Publish:   p.Events.Publish,
				Subscribe: p.Events.Subscribe,
			}
		}
		if p.Device != nil {
			perms.Device = manifest.DevicePerms{
				Light: p.Device.Light,
				IrCut: p.Device.IrCut,
				PTZ:   p.Device.PTZ,
				Lens:  p.Device.Lens,
			}
		}
		// Mirror the old emitter: network is only meaningful with a mode.
		if p.Network != nil && p.Network.Mode != "" {
			perms.Network = manifest.NetworkPerms{
				Mode:    p.Network.Mode,
				Inbound: p.Network.Inbound,
			}
		}
		m.Spec.Permissions = perms
	}

	for _, e := range req.Env {
		m.Spec.Env = append(m.Spec.Env, manifest.EnvVar{Name: e.Name, Value: e.Value})
	}
	for _, v := range req.Volumes {
		m.Spec.Volumes = append(m.Spec.Volumes, manifest.Volume{
			Host:      v.Host,
			Container: v.Container,
			Readonly:  v.ReadOnly,
		})
	}
	if req.Security.NoNewPrivileges != nil || req.Security.ReadonlyRootfs != nil {
		m.Spec.Security = manifest.SecuritySpec{
			NoNewPrivileges: req.Security.NoNewPrivileges,
			ReadonlyRootfs:  req.Security.ReadonlyRootfs,
		}
	}

	return m
}

// generateAppYAML renders the wizard request as app.yaml bytes. yaml.Marshal
// owns all quoting and escaping, so values containing ':', '#' or quotes
// produce valid YAML (the hand-built string version did not).
func (h *APIHandlers) generateAppYAML(req *WizardRequest) ([]byte, error) {
	return yaml.Marshal(wizardRequestToManifest(req))
}
