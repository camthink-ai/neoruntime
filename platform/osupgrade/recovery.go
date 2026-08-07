package osupgrade

import (
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	DefaultRecoveryDir      = "/data/aipc/recovery"
	RecoveryManifestVersion = 1
)

type RecoveryArtifact struct {
	File   string `json:"file"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
}

type RecoveryManifest struct {
	Format              int              `json:"format"`
	Machine             string           `json:"machine"`
	BSPVersion          string           `json:"bsp_version"`
	RecoveryVersion     string           `json:"recovery_version"`
	LocalUpdateProtocol string           `json:"local_update_protocol"`
	LocalUpdateModes    []string         `json:"local_update_modes,omitempty"`
	SecureBootKeyID     string           `json:"secure_boot_key_id,omitempty"`
	FitImage            RecoveryArtifact `json:"fit_image"`
	RootFS              RecoveryArtifact `json:"rootfs"`
}

type RecoveryBundle struct {
	Dir      string
	Manifest RecoveryManifest
	FitPath  string
	RootPath string
}

func LoadRecoveryBundle(dir, expectedMachine string) (*RecoveryBundle, error) {
	dir = filepath.Clean(dir)
	data, err := os.ReadFile(filepath.Join(dir, "manifest.json"))
	if err != nil {
		return nil, fmt.Errorf("bundled recovery manifest is unavailable: %w", err)
	}
	var manifest RecoveryManifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, fmt.Errorf("invalid bundled recovery manifest: %w", err)
	}
	if manifest.Format != RecoveryManifestVersion {
		return nil, fmt.Errorf("unsupported recovery manifest format %d", manifest.Format)
	}
	if expectedMachine != "" && manifest.Machine != expectedMachine {
		return nil, fmt.Errorf("bundled recovery machine mismatch: recovery=%q device=%q", manifest.Machine, expectedMachine)
	}
	if manifest.LocalUpdateProtocol != SingleRecoveryMarker {
		return nil, fmt.Errorf("bundled recovery does not support %s", SingleRecoveryMarker)
	}
	if strings.TrimSpace(manifest.RecoveryVersion) == "" {
		return nil, fmt.Errorf("bundled recovery version is missing")
	}
	fitPath, err := recoveryArtifactPath(dir, manifest.FitImage, "fitImage")
	if err != nil {
		return nil, err
	}
	rootPath, err := recoveryArtifactPath(dir, manifest.RootFS, "swupdate-image")
	if err != nil {
		return nil, err
	}
	if err := validateRecoveryArtifact(fitPath, manifest.FitImage); err != nil {
		return nil, fmt.Errorf("invalid bundled fitImage: %w", err)
	}
	if err := validateRecoveryArtifact(rootPath, manifest.RootFS); err != nil {
		return nil, fmt.Errorf("invalid bundled recovery rootfs: %w", err)
	}
	if err := recoveryContainsMarker(rootPath); err != nil {
		return nil, err
	}
	return &RecoveryBundle{
		Dir:      dir,
		Manifest: manifest,
		FitPath:  fitPath,
		RootPath: rootPath,
	}, nil
}

func (b *RecoveryBundle) Compatible(target *ValidationResult) error {
	if b == nil {
		return fmt.Errorf("bundled recovery is not loaded")
	}
	if target.Machine != "" && b.Manifest.Machine != target.Machine {
		return fmt.Errorf("recovery machine %q cannot install target %q", b.Manifest.Machine, target.Machine)
	}
	if target.MinRecoveryVersion != "" &&
		compareNumericVersions(b.Manifest.RecoveryVersion, target.MinRecoveryVersion) < 0 {
		return fmt.Errorf(
			"target requires recovery >= %s, bundled recovery is %s",
			target.MinRecoveryVersion,
			b.Manifest.RecoveryVersion,
		)
	}
	if target.SecureBootKeyID != "" &&
		b.Manifest.SecureBootKeyID != target.SecureBootKeyID {
		return fmt.Errorf(
			"secure boot key mismatch: target=%q recovery=%q",
			target.SecureBootKeyID,
			b.Manifest.SecureBootKeyID,
		)
	}
	return nil
}

func recoveryArtifactPath(dir string, artifact RecoveryArtifact, expectedPrefix string) (string, error) {
	name := filepath.Base(artifact.File)
	if name != artifact.File || !strings.HasPrefix(name, expectedPrefix) {
		return "", fmt.Errorf("invalid recovery artifact name %q", artifact.File)
	}
	return filepath.Join(dir, name), nil
}

func validateRecoveryArtifact(path string, artifact RecoveryArtifact) error {
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("%s is not a regular file", path)
	}
	if artifact.Size <= 0 || info.Size() != artifact.Size {
		return fmt.Errorf("size mismatch: manifest=%d actual=%d", artifact.Size, info.Size())
	}
	sum, err := fileSHA256(path)
	if err != nil {
		return err
	}
	if !strings.EqualFold(sum, artifact.SHA256) {
		return fmt.Errorf("SHA-256 mismatch: manifest=%s actual=%s", artifact.SHA256, sum)
	}
	return nil
}

func recoveryContainsMarker(path string) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	reader, err := gzip.NewReader(file)
	if err != nil {
		return fmt.Errorf("bundled recovery rootfs is not valid gzip: %w", err)
	}
	marker := newMarkerWriter([]byte(SingleRecoveryMarker))
	if _, err := io.Copy(marker, reader); err != nil {
		_ = reader.Close()
		return fmt.Errorf("cannot read bundled recovery rootfs: %w", err)
	}
	if err := reader.Close(); err != nil {
		return err
	}
	if !marker.Found() {
		return fmt.Errorf("bundled recovery rootfs does not contain %s", SingleRecoveryMarker)
	}
	return nil
}

func (b *RecoveryBundle) SupportsLocalUpdateMode(mode string) bool {
	if b == nil {
		return false
	}
	mode = strings.TrimSpace(mode)
	if mode == "" {
		return false
	}
	modes := b.Manifest.LocalUpdateModes
	if len(modes) == 0 {
		// Manifests generated before local_update_modes existed only support
		// the original single-copy path, whose recovery init hard-coded copy-a.
		modes = []string{"copy-a"}
	}
	for _, candidate := range modes {
		if candidate == "*" || candidate == mode {
			return true
		}
	}
	return false
}

func PrepareRecoveryBundleFromPackage(
	packagePath, destination, machine, version, keyID string,
) (*RecoveryBundle, error) {
	if err := os.RemoveAll(destination); err != nil {
		return nil, err
	}
	if err := ExtractRecoveryArtifacts(packagePath, destination); err != nil {
		return nil, err
	}
	fitPath := filepath.Join(destination, "fitImage")
	rootPath, err := recoveryRootFSPath(destination)
	if err != nil {
		return nil, err
	}
	modes, err := detectRecoveryLocalUpdateModes(rootPath)
	if err != nil {
		return nil, err
	}
	manifest, err := buildRecoveryManifest(machine, version, version, keyID, fitPath, rootPath, modes)
	if err != nil {
		return nil, err
	}
	data, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return nil, err
	}
	if err := atomicWrite(filepath.Join(destination, "manifest.json"), append(data, '\n'), 0644); err != nil {
		return nil, err
	}
	return LoadRecoveryBundle(destination, machine)
}

func BuildRecoveryManifest(
	machine, bspVersion, recoveryVersion, keyID, fitPath, rootPath string,
) (*RecoveryManifest, error) {
	return buildRecoveryManifest(
		machine,
		bspVersion,
		recoveryVersion,
		keyID,
		fitPath,
		rootPath,
		[]string{"copy-a"},
	)
}

func buildRecoveryManifest(
	machine, bspVersion, recoveryVersion, keyID, fitPath, rootPath string,
	localUpdateModes []string,
) (*RecoveryManifest, error) {
	artifact := func(path string) (RecoveryArtifact, error) {
		info, err := os.Stat(path)
		if err != nil {
			return RecoveryArtifact{}, err
		}
		file, err := os.Open(path)
		if err != nil {
			return RecoveryArtifact{}, err
		}
		defer file.Close()
		hash := sha256.New()
		if _, err := io.Copy(hash, file); err != nil {
			return RecoveryArtifact{}, err
		}
		return RecoveryArtifact{
			File:   filepath.Base(path),
			SHA256: hex.EncodeToString(hash.Sum(nil)),
			Size:   info.Size(),
		}, nil
	}
	fit, err := artifact(fitPath)
	if err != nil {
		return nil, err
	}
	root, err := artifact(rootPath)
	if err != nil {
		return nil, err
	}
	return &RecoveryManifest{
		Format:              RecoveryManifestVersion,
		Machine:             machine,
		BSPVersion:          bspVersion,
		RecoveryVersion:     recoveryVersion,
		LocalUpdateProtocol: SingleRecoveryMarker,
		LocalUpdateModes:    append([]string(nil), localUpdateModes...),
		SecureBootKeyID:     keyID,
		FitImage:            fit,
		RootFS:              root,
	}, nil
}

func recoveryRootFSPath(dir string) (string, error) {
	matches, err := filepath.Glob(filepath.Join(dir, "swupdate-image-*.ext4.gz"))
	if err != nil {
		return "", err
	}
	if len(matches) != 1 {
		return "", fmt.Errorf("expected exactly one recovery rootfs in %s, got %d", dir, len(matches))
	}
	return matches[0], nil
}

func detectRecoveryLocalUpdateModes(path string) ([]string, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	reader, err := gzip.NewReader(file)
	if err != nil {
		return nil, fmt.Errorf("bundled recovery rootfs is not valid gzip: %w", err)
	}
	protocol := newMarkerWriter([]byte(SingleRecoveryMarker))
	anyMode := newMarkerWriter([]byte("AIPC_LOCAL_RECOVERY_ANY_MODE_V1"))
	if _, err := io.Copy(io.MultiWriter(protocol, anyMode), reader); err != nil {
		_ = reader.Close()
		return nil, fmt.Errorf("cannot read bundled recovery rootfs: %w", err)
	}
	if err := reader.Close(); err != nil {
		return nil, err
	}
	if !protocol.Found() {
		return nil, fmt.Errorf("bundled recovery rootfs does not contain %s", SingleRecoveryMarker)
	}
	if anyMode.Found() {
		return []string{"*"}, nil
	}
	return []string{"copy-a"}, nil
}

func compareNumericVersions(left, right string) int {
	parse := func(value string) []int {
		fields := strings.FieldsFunc(value, func(r rune) bool { return r < '0' || r > '9' })
		result := make([]int, 0, len(fields))
		for _, field := range fields {
			number, _ := strconv.Atoi(field)
			result = append(result, number)
		}
		return result
	}
	a, b := parse(left), parse(right)
	length := len(a)
	if len(b) > length {
		length = len(b)
	}
	for index := 0; index < length; index++ {
		var av, bv int
		if index < len(a) {
			av = a[index]
		}
		if index < len(b) {
			bv = b[index]
		}
		if av < bv {
			return -1
		}
		if av > bv {
			return 1
		}
	}
	return 0
}
