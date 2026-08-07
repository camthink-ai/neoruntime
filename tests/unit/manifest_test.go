package unit

import (
	"os"
	"testing"

	"aipc/platform/app-manager/manifest"
)

func TestLoadManifest(t *testing.T) {
	// Create test manifest
	testManifest := `apiVersion: v1
kind: Application
metadata:
  id: test_app
  name: Test App
  version: 1.0.0
spec:
  image: test:1.0.0
  resources:
    cpu: "50%"
    memory: "256Mi"
  permissions:
    inference:
      models: [person_v1]
  autostart: true
`

	// Write to temp file
	tmpfile, err := os.CreateTemp("", "manifest-*.yaml")
	if err != nil {
		t.Fatal(err)
	}
	defer os.Remove(tmpfile.Name())

	if _, err := tmpfile.Write([]byte(testManifest)); err != nil {
		t.Fatal(err)
	}
	tmpfile.Close()

	// Load manifest
	m, err := manifest.LoadManifest(tmpfile.Name())
	if err != nil {
		t.Fatalf("LoadManifest failed: %v", err)
	}

	// Validate fields
	if m.Metadata.ID != "test_app" {
		t.Errorf("Expected ID 'test_app', got '%s'", m.Metadata.ID)
	}

	if m.Spec.Image != "test:1.0.0" {
		t.Errorf("Expected image 'test:1.0.0', got '%s'", m.Spec.Image)
	}

	if m.Spec.Resources.CPU != "50%" {
		t.Errorf("Expected CPU '50%%', got '%s'", m.Spec.Resources.CPU)
	}
}

func TestResourceParsing(t *testing.T) {
	resources := &manifest.Resources{
		CPU:    "75%",
		Memory: "512Mi",
	}

	// Test CPU parsing
	cpu, err := resources.GetCPUQuota()
	if err != nil {
		t.Fatalf("GetCPUQuota failed: %v", err)
	}
	if cpu != 0.75 {
		t.Errorf("Expected CPU 0.75, got %v", cpu)
	}

	// Test memory parsing
	mem, err := resources.GetMemoryBytes()
	if err != nil {
		t.Fatalf("GetMemoryBytes failed: %v", err)
	}
	expected := int64(512 * 1024 * 1024)
	if mem != expected {
		t.Errorf("Expected memory %d, got %d", expected, mem)
	}
}

func TestPermissionCheck(t *testing.T) {
	m := &manifest.AppManifest{
		Spec: manifest.Spec{
			Permissions: manifest.Permissions{
				Video: []string{"cam0_main.raw"},
				Inference: manifest.InferencePerms{
					Models: []string{"person_v1", "vehicle_v1"},
				},
				Device: manifest.DevicePerms{
					Light: true,
					PTZ:   false,
				},
			},
		},
	}

	// Test video permission
	if !m.HasPermission("video", "cam0_main.raw") {
		t.Error("Should have permission for cam0_main.raw")
	}

	if m.HasPermission("video", "cam1_main.raw") {
		t.Error("Should not have permission for cam1_main.raw")
	}

	// Test model permission
	if !m.HasPermission("model", "person_v1") {
		t.Error("Should have permission for person_v1")
	}

	if m.HasPermission("model", "face_v1") {
		t.Error("Should not have permission for face_v1")
	}

	// Test device permission
	if !m.HasPermission("device.light", "") {
		t.Error("Should have permission for light")
	}

	if m.HasPermission("device.ptz", "") {
		t.Error("Should not have permission for PTZ")
	}
}

func TestEventPermissions(t *testing.T) {
	m := &manifest.AppManifest{
		Spec: manifest.Spec{
			Permissions: manifest.Permissions{
				Events: manifest.EventPerms{
					Publish:   []string{"app/myapp/*", "alerts/critical"},
					Subscribe: []string{"model/*/detections"},
				},
			},
		},
	}

	// Test publish permissions
	if !m.CanPublishEvent("app/myapp/test") {
		t.Error("Should be able to publish to app/myapp/test")
	}

	if !m.CanPublishEvent("alerts/critical") {
		t.Error("Should be able to publish to alerts/critical")
	}

	if m.CanPublishEvent("system/admin") {
		t.Error("Should not be able to publish to system/admin")
	}
}

func TestManifestValidation(t *testing.T) {
	// Test invalid API version
	m := &manifest.AppManifest{
		APIVersion: "v2",
		Kind:       "Application",
		Metadata: manifest.Metadata{
			ID:      "test",
			Name:    "Test",
			Version: "1.0.0",
		},
		Spec: manifest.Spec{
			Image: "test:1.0.0",
			Resources: manifest.Resources{
				CPU:    "50%",
				Memory: "256Mi",
			},
		},
	}

	err := m.Validate()
	if err == nil {
		t.Error("Should fail validation for invalid API version")
	}

	// Test missing required fields
	m2 := &manifest.AppManifest{
		APIVersion: "v1",
		Kind:       "Application",
		Metadata: manifest.Metadata{
			ID: "test",
			// Missing Name
		},
	}

	err = m2.Validate()
	if err == nil {
		t.Error("Should fail validation for missing name")
	}
}
