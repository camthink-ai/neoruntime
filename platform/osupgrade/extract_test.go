package osupgrade

import (
	"os"
	"path/filepath"
	"testing"
)

func TestExtractRecoveryArtifacts(t *testing.T) {
	packagePath := filepath.Join(t.TempDir(), "image.swu")
	writeCPIO(t, packagePath, map[string][]byte{
		"sw-description":                           []byte(`software = { version = "1"; };`),
		"fitImage":                                 []byte("kernel"),
		"swupdate-image-hailo15-ne503.ext4.gz":     []byte("recovery"),
		"core-image-minimal-hailo15-ne503.ext4.gz": []byte("large-rootfs"),
	})
	destination := t.TempDir()
	if err := ExtractRecoveryArtifacts(packagePath, destination); err != nil {
		t.Fatal(err)
	}
	fit, _ := os.ReadFile(filepath.Join(destination, "fitImage"))
	recovery, _ := os.ReadFile(filepath.Join(destination, "swupdate-image-hailo15-ne503.ext4.gz"))
	if string(fit) != "kernel" || string(recovery) != "recovery" {
		t.Fatalf("unexpected extracted artifacts: fit=%q recovery=%q", fit, recovery)
	}
	if _, err := os.Stat(filepath.Join(destination, "core-image-minimal-hailo15-ne503.ext4.gz")); !os.IsNotExist(err) {
		t.Fatalf("main rootfs must not be extracted: %v", err)
	}
}
