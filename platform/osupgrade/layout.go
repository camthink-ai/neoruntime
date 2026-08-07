package osupgrade

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

const DefaultFilesystemDevice = "mmcblk1"

const (
	LayoutDual   = "dual"
	LayoutSingle = "single-recovery"
)

// ABLayout describes the partition contract used by the Hailo SWUpdate image:
// A boot/rootfs, B boot/rootfs, followed by the persistent data partition.
type ABLayout struct {
	Device string
	BootA  string
	RootA  string
	BootB  string
	RootB  string
	Data   string
}

func NewABLayout(device string) ABLayout {
	device = strings.TrimSpace(device)
	if device == "" {
		device = DefaultFilesystemDevice
	}
	if !filepath.IsAbs(device) {
		device = filepath.Join("/dev", device)
	}
	return ABLayout{
		Device: device,
		BootA:  device + "p1",
		RootA:  device + "p2",
		BootB:  device + "p3",
		RootB:  device + "p4",
		Data:   device + "p5",
	}
}

func (l ABLayout) Boot(copy string) string {
	if strings.EqualFold(copy, "A") {
		return l.BootA
	}
	if strings.EqualFold(copy, "B") {
		return l.BootB
	}
	return ""
}

func (l ABLayout) Root(copy string) string {
	if strings.EqualFold(copy, "A") {
		return l.RootA
	}
	if strings.EqualFold(copy, "B") {
		return l.RootB
	}
	return ""
}

type LayoutChecker struct {
	Layout             ABLayout
	Stat               func(string) (os.FileInfo, error)
	RootSource         func() (string, error)
	IsMounted          func(string) (bool, error)
	RequireBlockDevice bool
	// ModeProbe reads the swupdate_update_modes U-Boot env var so Detect can
	// distinguish a true A/B device (copy-a + copy-b) from one that carries a
	// dual partition table but runs copy-a-only single-copy recovery upgrades.
	// nil/empty/error falls back to partition-layout classification, preserving
	// the previous behavior on devices where the probe is unavailable.
	ModeProbe func() (string, error)
}

type LayoutResult struct {
	Mode        string
	CurrentCopy string
	TargetCopy  string
}

func NewLayoutChecker(device string) *LayoutChecker {
	return &LayoutChecker{
		Layout:             NewABLayout(device),
		Stat:               os.Stat,
		RootSource:         findRootSource,
		IsMounted:          findMountedSource,
		RequireBlockDevice: true,
		ModeProbe:          defaultSwupdateModesProbe(),
	}
}

func NewDeviceLayoutChecker() *LayoutChecker {
	return NewLayoutChecker(envOrDefault("AIPC_FILESYSTEM_DEVICE", DefaultFilesystemDevice))
}

// Check rejects upgrades unless the complete dual-copy partition layout is
// already present. It never creates or modifies partitions.
func (c *LayoutChecker) Check(currentCopy, targetCopy string) error {
	result, err := c.Detect(currentCopy)
	if err != nil {
		return err
	}
	if result.Mode != LayoutDual {
		return fmt.Errorf(
			"device uses the single-copy layout; use recovery upgrade mode instead of online A/B update",
		)
	}
	if !strings.EqualFold(result.TargetCopy, targetCopy) {
		return fmt.Errorf("target copy mismatch: detected=%s requested=%s", result.TargetCopy, targetCopy)
	}
	return c.checkDualMounts(currentCopy, targetCopy)
}

// Detect classifies the device without modifying it. A complete p1..p5 layout
// is dual-copy. p1..p3 with no p4/p5 is the legacy single-copy layout.
func (c *LayoutChecker) Detect(currentCopy string) (*LayoutResult, error) {
	if c == nil {
		return nil, errors.New("partition layout checker is not configured")
	}
	if c.Stat == nil {
		c.Stat = os.Stat
	}
	exists := make(map[string]bool)
	var invalid []string
	for _, path := range []string{c.Layout.BootA, c.Layout.RootA, c.Layout.BootB, c.Layout.RootB, c.Layout.Data} {
		info, err := c.Stat(path)
		if err != nil {
			if os.IsNotExist(err) {
				continue
			}
			return nil, fmt.Errorf("cannot inspect partition %s: %w", path, err)
		}
		exists[path] = true
		if c.RequireBlockDevice &&
			(info.Mode()&os.ModeDevice == 0 || info.Mode()&os.ModeCharDevice != 0) {
			invalid = append(invalid, path)
		}
	}
	if len(invalid) > 0 {
		return nil, fmt.Errorf("partition paths are not block devices: %s", strings.Join(invalid, ", "))
	}
	dual := exists[c.Layout.BootA] && exists[c.Layout.RootA] && exists[c.Layout.BootB] &&
		exists[c.Layout.RootB] && exists[c.Layout.Data]
	single := exists[c.Layout.BootA] && exists[c.Layout.RootA] && exists[c.Layout.BootB] &&
		!exists[c.Layout.RootB] && !exists[c.Layout.Data]
	switch {
	case dual:
		// A device can carry the full p1..p5 partition table but still be pinned
		// to copy A by the bootloader (SCU BL copy selection, no copy-b in
		// swupdate_update_modes). Such devices must use single-copy recovery
		// upgrades, not A/B slot switching. Any probe failure falls through to
		// the layout-based dual classification, preserving prior behavior.
		if c.isCopyAOnly() {
			if !strings.EqualFold(currentCopy, "A") {
				return nil, fmt.Errorf("single-copy device must boot copy A, got %q", currentCopy)
			}
			if err := c.checkSingleMounts(); err != nil {
				return nil, err
			}
			return &LayoutResult{Mode: LayoutSingle, CurrentCopy: "A", TargetCopy: "A"}, nil
		}
		target := oppositeCopy(currentCopy)
		if target == "" {
			return nil, fmt.Errorf("invalid current copy %q", currentCopy)
		}
		return &LayoutResult{Mode: LayoutDual, CurrentCopy: currentCopy, TargetCopy: target}, nil
	case single:
		if !strings.EqualFold(currentCopy, "A") {
			return nil, fmt.Errorf("single-copy layout must boot copy A, got %q", currentCopy)
		}
		if err := c.checkSingleMounts(); err != nil {
			return nil, err
		}
		return &LayoutResult{Mode: LayoutSingle, CurrentCopy: "A", TargetCopy: "A"}, nil
	}
	var missing []string
	for _, path := range []string{c.Layout.BootA, c.Layout.RootA, c.Layout.BootB, c.Layout.RootB, c.Layout.Data} {
		if !exists[path] {
			missing = append(missing, path)
		}
	}
	return nil, fmt.Errorf(
		"unsupported partition layout (missing %s); expected dual p1..p5 or legacy single p1..p3",
		strings.Join(missing, ", "),
	)
}

func (c *LayoutChecker) checkDualMounts(currentCopy, targetCopy string) error {
	currentRoot := c.Layout.Root(currentCopy)
	targetBoot := c.Layout.Boot(targetCopy)
	targetRoot := c.Layout.Root(targetCopy)
	if currentRoot == "" || targetBoot == "" || targetRoot == "" {
		return fmt.Errorf("invalid A/B copy selection: current=%q target=%q", currentCopy, targetCopy)
	}
	if c.RootSource != nil {
		source, err := c.RootSource()
		if err != nil {
			return fmt.Errorf("cannot identify mounted root filesystem: %w", err)
		}
		source = resolveDevicePath(source)
		// UUID=/PARTUUID= sources cannot be compared without probing udev. Exact
		// device paths can and should match the copy reported by Hailo's script.
		if strings.HasPrefix(source, "/dev/") && source != currentRoot {
			return fmt.Errorf(
				"boot copy mismatch: get_sw_image reports copy %s (%s), but / is mounted from %s",
				currentCopy, currentRoot, source,
			)
		}
	}
	if c.IsMounted != nil {
		for _, path := range []string{targetBoot, targetRoot} {
			mounted, err := c.IsMounted(path)
			if err != nil {
				return fmt.Errorf("cannot check whether target partition %s is mounted: %w", path, err)
			}
			if mounted {
				return fmt.Errorf("inactive target partition %s is mounted; refusing to overwrite it", path)
			}
		}
	}
	return nil
}

func (c *LayoutChecker) checkSingleMounts() error {
	if c.RootSource != nil {
		source, err := c.RootSource()
		if err != nil {
			return fmt.Errorf("cannot identify mounted root filesystem: %w", err)
		}
		source = resolveDevicePath(source)
		if strings.HasPrefix(source, "/dev/") && source != c.Layout.RootA {
			return fmt.Errorf("single-copy root must be %s, but / is mounted from %s", c.Layout.RootA, source)
		}
	}
	return nil
}

// isCopyAOnly reports whether the device carries the dual partition table but
// is pinned to copy A by the bootloader — i.e. it should use single-copy
// recovery upgrades rather than A/B slot switching. It inspects the
// swupdate_update_modes U-Boot env var (e.g. "init-partitions-dual,copy-a")
// and is true only when copy-a is present and copy-b is absent.
func (c *LayoutChecker) isCopyAOnly() bool {
	modes := c.probeSwupdateModes()
	if modes == "" {
		return false
	}
	return strings.Contains(modes, "copy-a") && !strings.Contains(modes, "copy-b")
}

// probeSwupdateModes returns the raw swupdate_update_modes env value, or "" on
// any error or when ModeProbe is unset. It is deliberately defensive: a probe
// failure keeps the layout-based classification intact (no regression).
func (c *LayoutChecker) probeSwupdateModes() string {
	if c == nil || c.ModeProbe == nil {
		return ""
	}
	out, err := c.ModeProbe()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(out)
}

// defaultSwupdateModesProbe reads swupdate_update_modes via fw_printenv, matching
// the on-device U-Boot env layout. The binary path is overridable for tests via
// the AIPC_FW_PRINTENV env var.
func defaultSwupdateModesProbe() func() (string, error) {
	binary := envOrDefault("AIPC_FW_PRINTENV", "fw_printenv")
	return func() (string, error) {
		out, err := exec.Command(binary, "-n", "swupdate_update_modes").CombinedOutput()
		if err != nil {
			return "", fmt.Errorf("read swupdate_update_modes via %s: %w: %s",
				binary, err, strings.TrimSpace(string(out)))
		}
		return strings.TrimSpace(string(out)), nil
	}
}

func findRootSource() (string, error) {
	output, err := exec.Command("findmnt", "-n", "-o", "SOURCE", "/").CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("%s: %w", strings.TrimSpace(string(output)), err)
	}
	return strings.TrimSpace(string(output)), nil
}

func findMountedSource(device string) (bool, error) {
	output, err := exec.Command("findmnt", "-r", "-n", "-S", device).CombinedOutput()
	if err == nil {
		return strings.TrimSpace(string(output)) != "", nil
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) && exitErr.ExitCode() == 1 {
		return false, nil
	}
	return false, fmt.Errorf("%s: %w", strings.TrimSpace(string(output)), err)
}

func resolveDevicePath(source string) string {
	source = strings.TrimSpace(source)
	if !strings.HasPrefix(source, "/dev/") {
		return source
	}
	resolved, err := filepath.EvalSymlinks(source)
	if err == nil {
		return resolved
	}
	return source
}
