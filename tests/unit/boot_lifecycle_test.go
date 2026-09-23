package unit

import (
	"os"
	"strings"
	"testing"
)

func TestOSVerifyDoesNotPullRuntimeBeforeCondition(t *testing.T) {
	raw, err := os.ReadFile("../../systemd/aipc-os-verify.service")
	if err != nil {
		t.Fatal(err)
	}
	unit := string(raw)

	for _, forbidden := range []string{
		"Wants=aipc-autostart.service",
		"Wants=aipc-platform.target",
		"Requires=aipc-autostart.service",
		"Requires=aipc-platform.target",
	} {
		if strings.Contains(unit, forbidden) {
			t.Fatalf("verify unit unconditionally pulls the runtime through %q", forbidden)
		}
	}
	if !strings.Contains(unit, "ExecCondition=/usr/libexec/aipc-os-updater needs-verify") {
		t.Fatal("verify unit is missing the upgrade-state ExecCondition")
	}
	if !strings.Contains(unit, "ExecStartPre=/usr/bin/systemctl restart aipc-autostart.service") {
		t.Fatal("verify unit does not restore the runtime for a real verify boot")
	}
}

func TestFirstbootDoesNotEnableRuntimeServices(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-firstboot.sh")
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(raw), "systemctl enable aipc-healthmon.service") {
		t.Fatal("firstboot must not override aipc-cli system disable")
	}
}

func TestCurrentRootInstallerPreservesRuntimeDisable(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-install-current-root.sh")
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)

	if !strings.Contains(script, `unit_enable_state["$name"]="$("$SYSTEMCTL" is-enabled "$name"`) {
		t.Fatal("current-root installer does not snapshot existing unit enable state")
	}
	if !strings.Contains(script, `disabled|masked|masked-runtime)`) {
		t.Fatal("current-root installer does not suppress explicitly disabled boot units")
	}
	if strings.Contains(script, "        aipc-platform.target\n") {
		t.Fatal("current-root installer must not explicitly start aipc-platform.target")
	}
}
