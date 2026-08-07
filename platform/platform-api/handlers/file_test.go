package handlers

import "testing"

func TestFileHandlerDefaultAllowsDataRoot(t *testing.T) {
	h := NewFileHandler(nil, "/data/aipc")

	got, err := h.validatePath("/data")
	if err != nil {
		t.Fatalf("validate /data: %v", err)
	}
	if got != "/data" {
		t.Fatalf("validate /data = %q, want /data", got)
	}

	got, err = h.validatePath("/data/aipc/etc")
	if err != nil {
		t.Fatalf("validate /data/aipc/etc: %v", err)
	}
	if got != "/data/aipc/etc" {
		t.Fatalf("validate /data/aipc/etc = %q, want /data/aipc/etc", got)
	}
}

func TestFileHandlerRejectsPrefixSibling(t *testing.T) {
	h := NewFileHandler([]string{"/data"}, "/data/aipc")

	if _, err := h.validatePath("/datax"); err == nil {
		t.Fatal("validate /datax succeeded, want access denied")
	}
}
