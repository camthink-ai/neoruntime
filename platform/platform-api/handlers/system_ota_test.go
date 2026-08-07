package handlers

import (
	"path/filepath"
	"testing"
	"time"
)

func withOTAStatusTestFile(t *testing.T) {
	t.Helper()

	oldStatus := otaStatus
	oldStatusFile := otaStatusFile
	t.Cleanup(func() {
		otaStatusMu.Lock()
		defer otaStatusMu.Unlock()
		otaStatus = oldStatus
		otaStatusFile = oldStatusFile
	})

	otaStatusFile = filepath.Join(t.TempDir(), "ota_status.json")
}

func TestClearCompletedOTARebootKeepsPendingInSameBoot(t *testing.T) {
	bootID := currentBootID()
	if bootID == "" {
		t.Skip("boot_id unavailable on this host")
	}
	withOTAStatusTestFile(t)

	otaStatusMu.Lock()
	otaStatus = OTAStatus{
		Status:       "success",
		Message:      "Firmware upgrade completed; rebooting",
		FinishedAt:   time.Now().Unix() - 3600,
		RebootNeeded: true,
		BootID:       bootID,
	}
	clearCompletedOTARebootIfSettledLocked()
	got := otaStatus
	otaStatusMu.Unlock()

	if !got.RebootNeeded {
		t.Fatal("same-boot OTA status cleared reboot_needed")
	}
	if got.RebootConfirmed {
		t.Fatal("same-boot OTA status was marked reboot_confirmed")
	}
}

func TestClearCompletedOTARebootConfirmsAfterBootIDChanges(t *testing.T) {
	if currentBootID() == "" {
		t.Skip("boot_id unavailable on this host")
	}
	withOTAStatusTestFile(t)

	otaStatusMu.Lock()
	otaStatus = OTAStatus{
		Status:       "success",
		Message:      "Firmware upgrade completed; rebooting",
		FinishedAt:   time.Now().Unix(),
		RebootNeeded: true,
		BootID:       "previous-boot",
	}
	clearCompletedOTARebootIfSettledLocked()
	got := otaStatus
	otaStatusMu.Unlock()

	if got.RebootNeeded {
		t.Fatal("changed-boot OTA status kept reboot_needed")
	}
	if !got.RebootConfirmed {
		t.Fatal("changed-boot OTA status did not mark reboot_confirmed")
	}
	if got.Message != "Firmware upgrade completed" {
		t.Fatalf("message = %q, want Firmware upgrade completed", got.Message)
	}
}
