package storage

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"io"
	"testing"
)

func testMeta() *PackageMeta {
	return &PackageMeta{
		ModelID:      "fire_smoke_detector",
		Name:         "Fire & Smoke Detector",
		ModelType:    "detection",
		OutputMode:   "platform",
		Config:       json.RawMessage(`{"threshold":0.4,"max_detections":64,"labels":"fire,smoke"}`),
		HEF:          PackageHEF{Filename: "fire_smoke.hef"},
		Network:      PackageNetwork{Name: "yolov8n_fire_smoke", InputWidth: 640, InputHeight: 384},
		OutputFormat: "nms",
	}
}

func testHEF() []byte {
	hef := make([]byte, 4096)
	for i := range hef {
		hef[i] = byte(i % 251)
	}
	return hef
}

func writePackage(t *testing.T, meta *PackageMeta, hef []byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := WritePackage(&buf, meta, bytes.NewReader(hef)); err != nil {
		t.Fatalf("WritePackage: %v", err)
	}
	return buf.Bytes()
}

// writeRawPackage builds a package from arbitrary JSON/HEF sections so tests
// can hand-craft malformed metadata that WritePackage would never emit.
func writeRawPackage(t *testing.T, jsonBytes, hef []byte) []byte {
	t.Helper()
	digest := sha256.New()
	digest.Write(jsonBytes)
	digest.Write(hef)

	header := make([]byte, packageHeaderSize)
	copy(header, packageMagic)
	binary.BigEndian.PutUint16(header[4:6], packageVersion)
	binary.BigEndian.PutUint32(header[8:12], uint32(len(jsonBytes)))
	binary.BigEndian.PutUint64(header[12:20], uint64(len(hef)))
	digest.Sum(header[20:20])

	return append(append(header, jsonBytes...), hef...)
}

func openAndDrain(t *testing.T, pkg []byte) (*PackageMeta, error) {
	t.Helper()
	pr, err := OpenPackage(bytes.NewReader(pkg))
	if err != nil {
		return nil, err
	}
	if _, err := io.Copy(io.Discard, pr.HEF()); err != nil {
		t.Fatalf("drain HEF: %v", err)
	}
	return pr.Meta(), pr.Verify()
}

func TestPackageRoundTrip(t *testing.T) {
	meta := testMeta()
	pkg := writePackage(t, meta, testHEF())

	if !IsPackageFile(pkg[:4]) {
		t.Fatal("IsPackageFile should recognize the written package")
	}

	got, err := openAndDrain(t, pkg)
	if err != nil {
		t.Fatalf("round-trip failed: %v", err)
	}
	// WritePackage stamps format_version on its own copy; expect the stamped value.
	stamped := *meta
	stamped.FormatVersion = packageVersion
	want, _ := json.Marshal(stamped)
	have, _ := json.Marshal(got)
	if !bytes.Equal(want, have) {
		t.Errorf("metadata mismatch:\n want %s\n have %s", want, have)
	}
}

func TestPackageWrittenHEFBytesIdentical(t *testing.T) {
	hef := testHEF()
	pkg := writePackage(t, testMeta(), hef)

	pr, err := OpenPackage(bytes.NewReader(pkg))
	if err != nil {
		t.Fatalf("OpenPackage: %v", err)
	}
	gotHEF, err := io.ReadAll(pr.HEF())
	if err != nil {
		t.Fatalf("read HEF: %v", err)
	}
	if !bytes.Equal(gotHEF, hef) {
		t.Error("embedded HEF bytes differ from the original file")
	}
	if err := pr.Verify(); err != nil {
		t.Errorf("Verify after full drain: %v", err)
	}
}

func TestPackageTamperDetected(t *testing.T) {
	pkg := writePackage(t, testMeta(), testHEF())
	pkg[len(pkg)-1] ^= 0xFF // flip a byte inside the HEF section

	if _, err := openAndDrain(t, pkg); err == nil {
		t.Error("tampered package must fail Verify")
	}
}

func TestPackageUnknownJSONKeyRejected(t *testing.T) {
	jsonBytes := []byte(`{"format_version":1,"model_id":"m","hef":{"filename":"a.hef"},"backend_lib_path":"/tmp/evil.so"}`)
	pkg := writeRawPackage(t, jsonBytes, testHEF())

	if _, err := openAndDrain(t, pkg); err == nil {
		t.Error("unknown top-level key (backend_lib_path) must be rejected")
	}
}

func TestPackageUnknownHEFKeyRejected(t *testing.T) {
	jsonBytes := []byte(`{"format_version":1,"model_id":"m","hef":{"filename":"a.hef","backend_config_path":"/tmp/evil.json"}}`)
	pkg := writeRawPackage(t, jsonBytes, testHEF())

	if _, err := openAndDrain(t, pkg); err == nil {
		t.Error("unknown key inside the closed hef section must be rejected")
	}
}

func TestPackageTruncatedHERejected(t *testing.T) {
	pkg := writePackage(t, testMeta(), testHEF())
	// Cut mid-HEF: header stays intact, fewer bytes follow than declared.
	cut := pkg[:packageHeaderSize+100+len(pkg)/2]

	pr, err := OpenPackage(bytes.NewReader(cut))
	if err != nil {
		t.Fatalf("OpenPackage on truncated package: %v", err)
	}
	if _, err := io.Copy(io.Discard, pr.HEF()); err != nil {
		t.Fatalf("drain short HEF: %v", err)
	}
	if err := pr.Verify(); err == nil {
		t.Error("truncated HEF section must fail Verify (digest mismatch)")
	}
}

func TestPackageVersionRejected(t *testing.T) {
	pkg := writePackage(t, testMeta(), testHEF())
	binary.BigEndian.PutUint16(pkg[4:6], 2)
	// The digest covers only json||hef, so patching version alone is enough.

	if _, err := OpenPackage(bytes.NewReader(pkg)); err == nil {
		t.Error("version 2 must be rejected")
	}
}

func TestPackageBadMagicRejected(t *testing.T) {
	pkg := writePackage(t, testMeta(), testHEF())
	copy(pkg[:4], "NOTP")

	if _, err := OpenPackage(bytes.NewReader(pkg)); err == nil {
		t.Error("bad magic must be rejected")
	}
	if IsPackageFile(pkg[:4]) {
		t.Error("IsPackageFile must not match a foreign magic")
	}
}

func TestPackageFormatVersionMismatchRejected(t *testing.T) {
	jsonBytes := []byte(`{"format_version":99,"model_id":"m","hef":{"filename":"a.hef"}}`)
	pkg := writeRawPackage(t, jsonBytes, testHEF())

	if _, err := openAndDrain(t, pkg); err == nil {
		t.Error("metadata format_version mismatch must be rejected")
	}
}

func TestPackageVerifyBeforeDrainRejected(t *testing.T) {
	pkg := writePackage(t, testMeta(), testHEF())

	pr, err := OpenPackage(bytes.NewReader(pkg))
	if err != nil {
		t.Fatalf("OpenPackage: %v", err)
	}
	if err := pr.Verify(); err == nil {
		t.Error("Verify before consuming the HEF section must fail")
	}
}

func TestPackageDoubleHEFReaderHashesOnce(t *testing.T) {
	pkg := writePackage(t, testMeta(), testHEF())

	pr, err := OpenPackage(bytes.NewReader(pkg))
	if err != nil {
		t.Fatalf("OpenPackage: %v", err)
	}
	// The same reader instance must come back, so partial reads followed by
	// a full drain still hash each byte exactly once.
	first := pr.HEF()
	second := pr.HEF()
	if first != second {
		t.Fatal("HEF() must return the same reader instance")
	}
	head := make([]byte, 16)
	if _, err := io.ReadFull(first, head); err != nil {
		t.Fatalf("partial read: %v", err)
	}
	if _, err := io.Copy(io.Discard, second); err != nil {
		t.Fatalf("drain: %v", err)
	}
	if err := pr.Verify(); err != nil {
		t.Errorf("Verify after partial+full read via shared reader: %v", err)
	}
}

func TestWritePackageDoesNotMutateMeta(t *testing.T) {
	meta := testMeta()
	meta.FormatVersion = 0 // WritePackage must stamp its own copy only
	_ = writePackage(t, meta, testHEF())
	if meta.FormatVersion != 0 {
		t.Error("WritePackage must not mutate the caller's meta")
	}
}
