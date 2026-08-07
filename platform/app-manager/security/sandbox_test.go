package security

import (
	"os"
	"path/filepath"
	"testing"

	"aipc/platform/app-manager/manifest"
)

func TestBuildContainerConfigCreatesWritableVolumeDirectory(t *testing.T) {
	hostPath := filepath.Join(t.TempDir(), "logs", "parking_lot")

	cfg, err := BuildContainerConfig(&manifest.AppManifest{
		Spec: manifest.Spec{
			Volumes: []manifest.Volume{
				{
					Host:      hostPath,
					Container: "/app/logs",
					Readonly:  false,
				},
			},
		},
	}, "")
	if err != nil {
		t.Fatalf("BuildContainerConfig failed: %v", err)
	}

	info, err := os.Stat(hostPath)
	if err != nil {
		t.Fatalf("expected writable host volume directory to be created: %v", err)
	}
	if !info.IsDir() {
		t.Fatalf("expected %s to be a directory", hostPath)
	}
	if info.Mode().Perm() != writableVolumeMode {
		t.Fatalf("expected mode %o, got %o", writableVolumeMode, info.Mode().Perm())
	}
	if len(cfg.Mounts) == 0 || cfg.Mounts[0].Source != hostPath {
		t.Fatalf("expected mount source %s, got %#v", hostPath, cfg.Mounts)
	}
}

func TestBuildContainerConfigDoesNotCreateReadonlyVolumeDirectory(t *testing.T) {
	hostPath := filepath.Join(t.TempDir(), "models")

	if _, err := BuildContainerConfig(&manifest.AppManifest{
		Spec: manifest.Spec{
			Volumes: []manifest.Volume{
				{
					Host:      hostPath,
					Container: "/data/aipc/models",
					Readonly:  true,
				},
			},
		},
	}, ""); err != nil {
		t.Fatalf("BuildContainerConfig failed: %v", err)
	}

	if _, err := os.Stat(hostPath); !os.IsNotExist(err) {
		t.Fatalf("expected readonly host volume path to remain absent, stat err=%v", err)
	}
}

func TestBuildContainerConfigRejectsMissingWritableFileMount(t *testing.T) {
	hostPath := filepath.Join(t.TempDir(), "app.conf")

	_, err := BuildContainerConfig(&manifest.AppManifest{
		Spec: manifest.Spec{
			Volumes: []manifest.Volume{
				{
					Host:      hostPath,
					Container: "/etc/app.conf",
					Readonly:  false,
				},
			},
		},
	}, "")
	if err == nil {
		t.Fatal("expected missing writable file-like mount to fail")
	}
	if _, statErr := os.Stat(hostPath); !os.IsNotExist(statErr) {
		t.Fatalf("expected file-like host path to remain absent, stat err=%v", statErr)
	}
}
