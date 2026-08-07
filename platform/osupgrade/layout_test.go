package osupgrade

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLayoutCheckerDetectsSingleCopyLayout(t *testing.T) {
	layout := NewABLayout(filepath.Join(t.TempDir(), "mmcblk1"))
	for _, path := range []string{layout.BootA, layout.RootA, layout.BootB} {
		if err := os.WriteFile(path, nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	checker := &LayoutChecker{
		Layout:             layout,
		Stat:               os.Stat,
		RootSource:         func() (string, error) { return layout.RootA, nil },
		IsMounted:          func(string) (bool, error) { return false, nil },
		RequireBlockDevice: false,
	}
	result, err := checker.Detect("A")
	if err != nil {
		t.Fatal(err)
	}
	if result.Mode != LayoutSingle || result.TargetCopy != "A" {
		t.Fatalf("unexpected layout result: %+v", result)
	}
}

func TestLayoutCheckerAcceptsDualCopyLayout(t *testing.T) {
	layout := NewABLayout(filepath.Join(t.TempDir(), "mmcblk1"))
	for _, path := range []string{layout.BootA, layout.RootA, layout.BootB, layout.RootB, layout.Data} {
		if err := os.WriteFile(path, nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	checker := &LayoutChecker{
		Layout:             layout,
		Stat:               os.Stat,
		RootSource:         func() (string, error) { return layout.RootA, nil },
		IsMounted:          func(string) (bool, error) { return false, nil },
		RequireBlockDevice: false,
	}
	if err := checker.Check("A", "B"); err != nil {
		t.Fatal(err)
	}
}

func TestLayoutCheckerRejectsMountedInactivePartition(t *testing.T) {
	layout := NewABLayout(filepath.Join(t.TempDir(), "mmcblk1"))
	for _, path := range []string{layout.BootA, layout.RootA, layout.BootB, layout.RootB, layout.Data} {
		if err := os.WriteFile(path, nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	checker := &LayoutChecker{
		Layout:     layout,
		Stat:       os.Stat,
		RootSource: func() (string, error) { return layout.RootA, nil },
		IsMounted: func(path string) (bool, error) {
			return path == layout.RootB, nil
		},
		RequireBlockDevice: false,
	}
	if err := checker.Check("A", "B"); err == nil || !strings.Contains(err.Error(), "mounted") {
		t.Fatalf("expected mounted target rejection, got %v", err)
	}
}

// A device with the full p1..p5 partition table but the bootloader pinned to
// copy A (copy-a present, copy-b absent in swupdate_update_modes) must be
// classified single-copy so the UI and staging route to recovery mode.
func TestLayoutCheckerDowngradesDualPartitionsToSingleOnCopyAOnlyEnv(t *testing.T) {
	layout := NewABLayout(filepath.Join(t.TempDir(), "mmcblk1"))
	for _, path := range []string{layout.BootA, layout.RootA, layout.BootB, layout.RootB, layout.Data} {
		if err := os.WriteFile(path, nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	checker := &LayoutChecker{
		Layout:             layout,
		Stat:               os.Stat,
		RootSource:         func() (string, error) { return layout.RootA, nil },
		IsMounted:          func(string) (bool, error) { return false, nil },
		ModeProbe:          func() (string, error) { return "init-partitions-dual,init-scu-bl,copy-a", nil },
		RequireBlockDevice: false,
	}
	result, err := checker.Detect("A")
	if err != nil {
		t.Fatal(err)
	}
	if result.Mode != LayoutSingle || result.CurrentCopy != "A" || result.TargetCopy != "A" {
		t.Fatalf("expected single-copy downgrade, got %+v", result)
	}
	// Check() must reject A/B staging and steer toward recovery mode.
	if err := checker.Check("A", "B"); err == nil || !strings.Contains(err.Error(), "recovery") {
		t.Fatalf("expected recovery-mode steering, got %v", err)
	}
}

// A true A/B device (copy-a and copy-b both present) keeps dual classification
// even though the partition table looks identical to the copy-a-only case.
func TestLayoutCheckerKeepsDualWhenCopyBInEnv(t *testing.T) {
	layout := NewABLayout(filepath.Join(t.TempDir(), "mmcblk1"))
	for _, path := range []string{layout.BootA, layout.RootA, layout.BootB, layout.RootB, layout.Data} {
		if err := os.WriteFile(path, nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	checker := &LayoutChecker{
		Layout:             layout,
		Stat:               os.Stat,
		RootSource:         func() (string, error) { return layout.RootA, nil },
		IsMounted:          func(string) (bool, error) { return false, nil },
		ModeProbe:          func() (string, error) { return "init-scu-bl,copy-a,copy-b", nil },
		RequireBlockDevice: false,
	}
	if err := checker.Check("A", "B"); err != nil {
		t.Fatalf("expected dual-copy acceptance, got %v", err)
	}
}

// Probe failure must be non-fatal: fall back to partition-layout dual so the
// detector never regresses on devices where fw_printenv is unavailable.
func TestLayoutCheckerFallsBackToDualWhenProbeErrors(t *testing.T) {
	layout := NewABLayout(filepath.Join(t.TempDir(), "mmcblk1"))
	for _, path := range []string{layout.BootA, layout.RootA, layout.BootB, layout.RootB, layout.Data} {
		if err := os.WriteFile(path, nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	checker := &LayoutChecker{
		Layout:             layout,
		Stat:               os.Stat,
		RootSource:         func() (string, error) { return layout.RootA, nil },
		IsMounted:          func(string) (bool, error) { return false, nil },
		ModeProbe:          func() (string, error) { return "", os.ErrNotExist },
		RequireBlockDevice: false,
	}
	if err := checker.Check("A", "B"); err != nil {
		t.Fatalf("expected dual fallback on probe error, got %v", err)
	}
}
