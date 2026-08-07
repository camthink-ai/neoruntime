package handlers

import (
	"aipc/platform/common/constants"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/gin-gonic/gin"

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
	yamlContent := h.generateAppYAML(&req)

	// Create persistent manifest directory
	manifestDir := fmt.Sprintf(constants.RootPath()+"/apps/manifests/%s", req.Metadata.ID)
	if err := os.MkdirAll(manifestDir, 0755); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create manifest directory: "+err.Error())
		return
	}

	// Write to persistent manifest file
	manifestFile := filepath.Join(manifestDir, "app.yaml")
	if err := os.WriteFile(manifestFile, []byte(yamlContent), 0644); err != nil {
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

// generateAppYAML generates app.yaml content from wizard request
func (h *APIHandlers) generateAppYAML(req *WizardRequest) string {
	var sb strings.Builder

	// API Version and Kind
	sb.WriteString("apiVersion: v1\n")
	sb.WriteString("kind: Application\n\n")

	// Metadata
	sb.WriteString("metadata:\n")
	sb.WriteString("  id: " + req.Metadata.ID + "\n")
	sb.WriteString("  name: " + req.Metadata.Name + "\n")
	sb.WriteString("  version: " + req.Metadata.Version + "\n")
	if req.Metadata.Description != "" {
		sb.WriteString("  description: " + req.Metadata.Description + "\n")
	}
	sb.WriteString("\n")

	// Spec
	sb.WriteString("spec:\n")
	sb.WriteString("  image: " + req.Image + "\n")
	sb.WriteString("\n")

	// Resources
	if req.Resources.CPU != "" || req.Resources.Memory != "" {
		sb.WriteString("  resources:\n")
		if req.Resources.CPU != "" {
			sb.WriteString("    cpu: \"" + req.Resources.CPU + "\"\n")
		}
		if req.Resources.Memory != "" {
			sb.WriteString("    memory: \"" + req.Resources.Memory + "\"\n")
		}
		sb.WriteString("\n")
	}

	// Permissions
	hasPermissions := len(req.Permissions.Video) > 0 ||
		req.Permissions.Inference != nil ||
		req.Permissions.Events != nil ||
		req.Permissions.Device != nil ||
		req.Permissions.Network != nil

	if hasPermissions {
		sb.WriteString("  permissions:\n")

		if len(req.Permissions.Video) > 0 {
			sb.WriteString("    video:\n")
			for _, v := range req.Permissions.Video {
				sb.WriteString("      - " + v + "\n")
			}
		}

		if req.Permissions.Inference != nil {
			sb.WriteString("    inference:\n")
			if len(req.Permissions.Inference.Models) > 0 {
				sb.WriteString("      models:\n")
				for _, m := range req.Permissions.Inference.Models {
					sb.WriteString("        - " + m + "\n")
				}
			}
			if req.Permissions.Inference.MaxQPS > 0 {
				sb.WriteString(fmt.Sprintf("      max_qps: %d\n", req.Permissions.Inference.MaxQPS))
			}
			if req.Permissions.Inference.MaxConcurrent > 0 {
				sb.WriteString(fmt.Sprintf("      max_concurrent: %d\n", req.Permissions.Inference.MaxConcurrent))
			}
			if req.Permissions.Inference.AllowRegister {
				sb.WriteString("      allow_register_model: true\n")
			}
		}

		if req.Permissions.Events != nil {
			sb.WriteString("    events:\n")
			if len(req.Permissions.Events.Publish) > 0 {
				sb.WriteString("      publish:\n")
				for _, p := range req.Permissions.Events.Publish {
					sb.WriteString("        - " + p + "\n")
				}
			}
			if len(req.Permissions.Events.Subscribe) > 0 {
				sb.WriteString("      subscribe:\n")
				for _, s := range req.Permissions.Events.Subscribe {
					sb.WriteString("        - " + s + "\n")
				}
			}
		}

		if req.Permissions.Device != nil {
			hasDevice := req.Permissions.Device.Light || req.Permissions.Device.IrCut || req.Permissions.Device.PTZ || req.Permissions.Device.Lens
			if hasDevice {
				sb.WriteString("    device:\n")
				if req.Permissions.Device.Light {
					sb.WriteString("      light: true\n")
				}
				if req.Permissions.Device.IrCut {
					sb.WriteString("      ir_cut: true\n")
				}
				if req.Permissions.Device.PTZ {
					sb.WriteString("      ptz: true\n")
				}
				if req.Permissions.Device.Lens {
					sb.WriteString("      lens: true\n")
				}
			}
		}

		if req.Permissions.Network != nil && req.Permissions.Network.Mode != "" {
			sb.WriteString("    network:\n")
			sb.WriteString("      mode: " + req.Permissions.Network.Mode + "\n")
			if len(req.Permissions.Network.Inbound) > 0 {
				sb.WriteString("      inbound:\n")
				for _, port := range req.Permissions.Network.Inbound {
					sb.WriteString(fmt.Sprintf("        - %d\n", port))
				}
			}
		}
		sb.WriteString("\n")
	}

	// Environment variables
	if len(req.Env) > 0 {
		sb.WriteString("  env:\n")
		for _, e := range req.Env {
			sb.WriteString(fmt.Sprintf("    - name: %s\n      value: \"%s\"\n", e.Name, e.Value))
		}
		sb.WriteString("\n")
	}

	// Volumes
	if len(req.Volumes) > 0 {
		sb.WriteString("  volumes:\n")
		for _, v := range req.Volumes {
			sb.WriteString(fmt.Sprintf("    - host: %s\n      container: %s\n", v.Host, v.Container))
			if v.ReadOnly {
				sb.WriteString("      readonly: true\n")
			}
		}
		sb.WriteString("\n")
	}

	// Autostart
	if req.Autostart {
		sb.WriteString("  autostart: true\n")
	}

	// Restart policy
	if req.RestartPolicy != "" {
		sb.WriteString("  restart_policy: " + req.RestartPolicy + "\n")
	}

	// Security overrides
	if req.Security.NoNewPrivileges != nil || req.Security.ReadonlyRootfs != nil {
		sb.WriteString("  security:\n")
		if req.Security.NoNewPrivileges != nil {
			sb.WriteString(fmt.Sprintf("    no_new_privileges: %v\n", *req.Security.NoNewPrivileges))
		}
		if req.Security.ReadonlyRootfs != nil {
			sb.WriteString(fmt.Sprintf("    readonly_rootfs: %v\n", *req.Security.ReadonlyRootfs))
		}
	}

	return sb.String()
}
