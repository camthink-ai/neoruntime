package cmd

import (
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"aipc/tools/aipc-cli/pkg/output"
)

// The CLI must manage exactly the unit set that aipc-autostart.sh enables and
// starts at boot. If the lists drift, `aipc-cli system stop/disable` silently
// leaves units behind (list too short) or hard-fails on installs that lack a
// unit (list too long).
func TestAipcServicesMatchAutostartScript(t *testing.T) {
	raw, err := os.ReadFile("../../../scripts/aipc-autostart.sh")
	if err != nil {
		t.Fatalf("read aipc-autostart.sh: %v", err)
	}
	match := regexp.MustCompile(`(?s)SERVICES=\((.*?)\)`).FindStringSubmatch(string(raw))
	if match == nil {
		t.Fatal("SERVICES=(...) block not found in aipc-autostart.sh")
	}
	autostart := strings.Fields(match[1])
	if len(autostart) == 0 {
		t.Fatal("empty SERVICES block in aipc-autostart.sh")
	}
	if len(aipcServices) != len(autostart) {
		t.Fatalf("unit list drift: CLI has %d units %v, autostart has %d %v",
			len(aipcServices), aipcServices, len(autostart), autostart)
	}
	for i, name := range autostart {
		if aipcServices[i] != name {
			t.Fatalf("unit list drift at index %d: CLI=%q autostart=%q (full CLI list %v)",
				i, aipcServices[i], name, aipcServices)
		}
	}
}

func TestSystemDisableQuiescesAutostartBeforeRuntime(t *testing.T) {
	oldPrinter := printer
	printer = output.NewPrinter("table", false)
	printer.SetWriter(io.Discard)
	t.Cleanup(func() { printer = oldPrinter })

	tmp := t.TempDir()
	logPath := filepath.Join(tmp, "systemctl.log")
	fakeSystemctl := filepath.Join(tmp, "systemctl")
	if err := os.WriteFile(fakeSystemctl, []byte(`#!/bin/sh
printf '%s\n' "$*" >> "$SYSTEMCTL_LOG"
exit 0
`), 0755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", tmp+string(os.PathListSeparator)+os.Getenv("PATH"))
	t.Setenv("SYSTEMCTL_LOG", logPath)

	if err := serviceDisableCmd.RunE(serviceDisableCmd, nil); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(logPath)
	if err != nil {
		t.Fatal(err)
	}
	var actions []string
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n") {
		if !strings.HasPrefix(line, "cat ") {
			actions = append(actions, line)
		}
	}
	if len(actions) == 0 {
		t.Fatal("system disable did not invoke systemctl")
	}
	if actions[0] != "disable --now aipc-autostart.service" {
		t.Fatalf("first systemctl action = %q, want autostart disable --now; all actions: %v", actions[0], actions)
	}
	for _, action := range actions[1:] {
		if strings.Contains(action, "aipc-autostart.service") {
			t.Fatalf("autostart was managed again after runtime operations: %v", actions)
		}
	}
}
