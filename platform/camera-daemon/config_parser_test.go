package cameradaemon_test

import (
	"os"
	"strings"
	"testing"
)

func TestAudioEnabledParserUsesExactKeyMatch(t *testing.T) {
	src, err := os.ReadFile("src/main.cpp")
	if err != nil {
		t.Fatalf("read main.cpp: %v", err)
	}

	body := string(src)
	start := strings.Index(body, `} else if (section == "audio") {`)
	if start < 0 {
		t.Fatal("audio parser section not found")
	}
	end := strings.Index(body[start:], `} else if (section == "autofocus") {`)
	if end < 0 {
		t.Fatal("autofocus parser section not found after audio section")
	}
	audioSection := body[start : start+end]

	if !strings.Contains(audioSection, `config_key_is(trimmed, "enabled")`) {
		t.Fatal("audio.enabled must be parsed with exact key matching")
	}
	if strings.Contains(audioSection, `find("enabled:")`) {
		t.Fatal(`audio parser must not use substring matching for "enabled:"; playback_enabled can otherwise disable capture`)
	}
}
