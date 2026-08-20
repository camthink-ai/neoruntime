package cmd

import (
	"bytes"
	"encoding/csv"
	"path/filepath"
	"strings"
	"testing"
)

func TestPerDeviceRecordPathSeparatesDuplicateSNByIP(t *testing.T) {
	a := perDeviceRecordPath("/tmp/metrics.csv", "csv", "SAME-SN", "", "192.168.1.10", "http://192.168.1.10:8080")
	b := perDeviceRecordPath("/tmp/metrics.csv", "csv", "SAME-SN", "", "192.168.1.11", "http://192.168.1.11:8080")

	if a == b {
		t.Fatalf("duplicate SN devices should not share a record file: %s", a)
	}
}

func TestPerDeviceRecordPathUsesIP(t *testing.T) {
	got := perDeviceRecordPath("/tmp/metrics.csv", "csv", "SN001", "AA:BB:CC:DD:EE:FF", "192.168.1.10", "http://192.168.1.10:8080")
	want := "/tmp/metrics_192-168-1-10.csv"

	if got != want {
		t.Fatalf("unexpected record path: got %q want %q", got, want)
	}
}

func TestPerDeviceRecordPathStaysInBaseDirForHostileSN(t *testing.T) {
	got := perDeviceRecordPath("/tmp/out/metrics.csv", "csv", "../../escape", "", "", "")
	if filepath.Dir(got) != "/tmp/out" {
		t.Fatalf("record path escaped base dir: %s", got)
	}
	if filepath.Base(got) != "metrics_escape.csv" {
		t.Fatalf("unexpected sanitized file name: %s", filepath.Base(got))
	}
}

func TestNormalizeAPIURLDefaultsSchemeAndTrimsSlash(t *testing.T) {
	apiURL, host, err := normalizeAPIURL("192.168.1.10:8080/")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if apiURL != "http://192.168.1.10:8080" {
		t.Fatalf("unexpected normalized URL: %q", apiURL)
	}
	if host != "192.168.1.10" {
		t.Fatalf("unexpected host: %q", host)
	}
}

func TestNormalizeAPIURLStripsQueryAndFragment(t *testing.T) {
	apiURL, _, err := normalizeAPIURL("https://device.example.com/api/?x=1#frag")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if apiURL != "https://device.example.com/api" {
		t.Fatalf("query/fragment should be stripped, got %q", apiURL)
	}
}

func TestNormalizeAPIURLRejectsNonHTTPScheme(t *testing.T) {
	if _, _, err := normalizeAPIURL("ftp://device.example.com"); err == nil {
		t.Fatalf("expected error for non-http scheme")
	}
}

func TestNormalizeAPIURLRejectsEmptyAddress(t *testing.T) {
	if _, _, err := normalizeAPIURL("   "); err == nil {
		t.Fatalf("expected error for empty address")
	}
}

func TestSanitizeRecordFilePartFlattensSeparators(t *testing.T) {
	if got := sanitizeRecordFilePart("../../etc/passwd"); got != "etc-passwd" {
		t.Fatalf("path separators should be flattened to dashes, got %q", got)
	}
}

func TestSanitizeRecordFilePartFallsBackToDevice(t *testing.T) {
	if got := sanitizeRecordFilePart("///"); got != "device" {
		t.Fatalf("symbol-only input should fall back to \"device\", got %q", got)
	}
}

func TestUnwrapAPIResponseUnwrapsEnvelope(t *testing.T) {
	data, err := unwrapAPIResponse([]byte(`{"code":0,"message":"ok","data":{"cpu":12.5}}`))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if string(data) != `{"cpu":12.5}` {
		t.Fatalf("expected inner data payload, got %s", data)
	}
}

func TestUnwrapAPIResponsePropagatesErrorCode(t *testing.T) {
	if _, err := unwrapAPIResponse([]byte(`{"code":500,"message":"boom","data":{}}`)); err == nil {
		t.Fatalf("expected error for non-zero envelope code")
	}
}

func TestUnwrapAPIResponsePassesThroughBareJSON(t *testing.T) {
	bare := []byte(`{"cpu":12.5}`)
	data, err := unwrapAPIResponse(bare)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !bytes.Equal(data, bare) {
		t.Fatalf("bare JSON should pass through unchanged, got %s", data)
	}
}

func TestSanitizeCSVFieldPrefixesFormulaStarters(t *testing.T) {
	for _, in := range []string{`=HYPERLINK("http://evil")`, "+1|cmd", "-2+3|cmd", "@SUM(A1:A2)", "\tx", "\ry"} {
		if got := sanitizeCSVField(in); !strings.HasPrefix(got, "'") || strings.TrimPrefix(got, "'") != in {
			t.Fatalf("formula-leading value should be quoted, got %q", got)
		}
	}
}

func TestSanitizeCSVFieldLeavesNormalValues(t *testing.T) {
	for _, in := range []string{"", "SN001", "192.168.1.10", "a=b", "x-y", "v1.3.8"} {
		if got := sanitizeCSVField(in); got != in {
			t.Fatalf("normal value should pass through, got %q", got)
		}
	}
}

func TestCSVRecordWriterNeutralizesHostileDeviceFields(t *testing.T) {
	var buf bytes.Buffer
	w := &csvRecordWriter{writer: csv.NewWriter(&buf)}
	rec := resourceRecord{
		Timestamp:   "2026-08-19T00:00:00Z",
		UnixMillis:  1,
		SN:          `=HYPERLINK("http://evil")`,
		IP:          "192.168.1.10",
		MetricsOK:   true,
		DiskMountpoint: "/data",
	}
	if err := w.Write(rec); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	w.writer.Flush()
	out := buf.String()
	if !strings.Contains(out, "'=HYPERLINK") {
		t.Fatalf("hostile SN should be quoted in CSV output: %q", out)
	}
	if !strings.Contains(out, "192.168.1.10") {
		t.Fatalf("normal IP should be written unchanged: %q", out)
	}
}
