package storage

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"hash"
	"io"
)

// Single-file model distribution packages (.bin, magic "AMPK"): the platform
// registration metadata as a strictly-parsed JSON section, followed by the HEF
// bytes byte-identical to the original file. The HEF is unpacked before use,
// so HailoRT never sees wrapper bytes. A sha256 over the JSON+HEF payload
// makes corruption detectable before the HEF is staged anywhere permanent.
//
// Layout (all integers big-endian):
//
//	0   4   magic "AMPK"
//	4   2   version (currently 1)
//	6   2   flags (reserved, must be 0)
//	8   4   json_len
//	12  8   hef_len
//	20  32  sha256(json_bytes || hef_bytes)
//	52  ..  json_bytes, then hef_bytes
//
// Security: the metadata schema is closed — unknown top-level keys are
// rejected — and deliberately has no field that can reach a postprocess
// variant passthrough. backend_lib_path-style arbitrary-dlopen vectors must
// never travel inside a distribution package; function selection only rides
// the constrained postprocess_profile config key, validated by the caller
// against the profile table.

const (
	packageMagic        = "AMPK"
	packageVersion      = uint16(1)
	packageHeaderSize   = 4 + 2 + 2 + 4 + 8 + sha256.Size
	maxPackageJSONBytes = 1 << 20 // 1 MiB is far beyond any sane metadata payload
)

// PackageHEF identifies the embedded model binary.
type PackageHEF struct {
	Filename string `json:"filename"`         // original filename, display only
	SHA256   string `json:"sha256,omitempty"` // hex digest of the HEF bytes, checked when present
}

// PackageNetwork is advisory context, re-sniffed from the HEF at import time.
type PackageNetwork struct {
	Name        string `json:"name,omitempty"`
	InputWidth  int    `json:"input_width,omitempty"`
	InputHeight int    `json:"input_height,omitempty"`
}

// PackageMeta is the JSON section. Config is carried raw (a JSON object of
// schema-driven key→value pairs) because its keys evolve per model type with
// SupportedModelTypes; the caller validates every key against the declared
// type's field definitions. Everything around Config is a closed schema.
type PackageMeta struct {
	FormatVersion uint16          `json:"format_version"`
	ModelID       string          `json:"model_id"`
	Name          string          `json:"name,omitempty"`
	ModelType     string          `json:"model_type,omitempty"`
	OutputMode    string          `json:"output_mode,omitempty"`
	Config        json.RawMessage `json:"config,omitempty"`
	HEF           PackageHEF      `json:"hef"`
	Network       PackageNetwork  `json:"network"`
	OutputFormat  string          `json:"output_format,omitempty"` // advisory: nms | feature_map
}

// IsPackageFile reports whether the leading bytes of a file look like an
// AMPK package. Callers sniff uploads with this before choosing the package
// import path over the plain-HEF path.
func IsPackageFile(header []byte) bool {
	return len(header) >= len(packageMagic) && string(header[:len(packageMagic)]) == packageMagic
}

// WritePackage emits a v1 package. The HEF is read twice (digest pass, then
// copy pass), so hef must be seekable — call sites pass an *os.File blob.
func WritePackage(w io.Writer, meta *PackageMeta, hef io.ReadSeeker) error {
	out := *meta // never mutate the caller's struct
	out.FormatVersion = packageVersion

	jsonBytes, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		return fmt.Errorf("failed to marshal package metadata: %w", err)
	}
	if len(jsonBytes) > maxPackageJSONBytes {
		return fmt.Errorf("package metadata too large (%d bytes)", len(jsonBytes))
	}

	// Pass 1: digest over json||hef, and the HEF length for the header.
	digest := sha256.New()
	digest.Write(jsonBytes)
	hefLen, err := io.Copy(digest, hef)
	if err != nil {
		return fmt.Errorf("failed to digest HEF section: %w", err)
	}

	// Pass 2: header + payload.
	if _, err := hef.Seek(0, io.SeekStart); err != nil {
		return fmt.Errorf("failed to rewind HEF reader: %w", err)
	}
	header := make([]byte, packageHeaderSize)
	copy(header, packageMagic)
	binary.BigEndian.PutUint16(header[4:6], packageVersion)
	binary.BigEndian.PutUint16(header[6:8], 0) // flags
	binary.BigEndian.PutUint32(header[8:12], uint32(len(jsonBytes)))
	binary.BigEndian.PutUint64(header[12:20], uint64(hefLen))
	digest.Sum(header[20:20])

	if _, err := w.Write(header); err != nil {
		return fmt.Errorf("failed to write package header: %w", err)
	}
	if _, err := w.Write(jsonBytes); err != nil {
		return fmt.Errorf("failed to write package metadata: %w", err)
	}
	if _, err := io.Copy(w, hef); err != nil {
		return fmt.Errorf("failed to write HEF section: %w", err)
	}
	return nil
}

// PackageReader holds a partially consumed package: the metadata is decoded
// and the HEF section streams through HEF() (hashing into the package digest
// as it goes). r must be the raw upload stream — not wrapped in a buffered
// reader, which would swallow HEF bytes.
type PackageReader struct {
	meta         *PackageMeta
	hef          io.Reader // single shared TeeReader so the digest sees each byte once
	hasher       hash.Hash
	headerDigest [sha256.Size]byte
}

// OpenPackage validates the header and JSON section and returns a reader
// positioned at the HEF section. The metadata parses before the payload
// digest is confirmed — that is safe because parsing is pure CPU on a
// bounded, closed-schema section — but the HEF bytes must only be staged
// temporarily until Verify succeeds (callers delete any staged blob if it
// fails).
func OpenPackage(r io.Reader) (*PackageReader, error) {
	head := make([]byte, packageHeaderSize)
	if _, err := io.ReadFull(r, head); err != nil {
		return nil, fmt.Errorf("not a model package (header too short): %w", err)
	}
	if !IsPackageFile(head[:4]) {
		return nil, fmt.Errorf("not a model package (bad magic)")
	}
	version := binary.BigEndian.Uint16(head[4:6])
	if version != packageVersion {
		return nil, fmt.Errorf("unsupported model package version %d (supported: %d)", version, packageVersion)
	}
	if flags := binary.BigEndian.Uint16(head[6:8]); flags != 0 {
		return nil, fmt.Errorf("unsupported model package flags %d", flags)
	}
	jsonLen := binary.BigEndian.Uint32(head[8:12])
	hefLen := binary.BigEndian.Uint64(head[12:20])
	if jsonLen == 0 || jsonLen > maxPackageJSONBytes {
		return nil, fmt.Errorf("model package metadata length %d out of range", jsonLen)
	}
	if hefLen == 0 {
		return nil, fmt.Errorf("model package contains no HEF data")
	}

	jsonBytes := make([]byte, jsonLen)
	if _, err := io.ReadFull(r, jsonBytes); err != nil {
		return nil, fmt.Errorf("model package metadata truncated: %w", err)
	}

	// Closed schema: reject unknown keys rather than guessing what they
	// might mean. json.Unmarshal cannot report them, so decode strictly.
	meta := &PackageMeta{}
	dec := json.NewDecoder(bytes.NewReader(jsonBytes))
	dec.DisallowUnknownFields()
	if err := dec.Decode(meta); err != nil {
		return nil, fmt.Errorf("model package metadata rejected (strict schema): %w", err)
	}
	if err := dec.Decode(&struct{}{}); err != io.EOF {
		return nil, fmt.Errorf("model package metadata must be exactly one JSON object")
	}
	if meta.FormatVersion != packageVersion {
		return nil, fmt.Errorf("unsupported metadata format_version %d (supported: %d)", meta.FormatVersion, packageVersion)
	}
	if meta.HEF.Filename == "" {
		return nil, fmt.Errorf("model package metadata missing hef.filename")
	}

	hasher := sha256.New()
	hasher.Write(jsonBytes) // the digest covers json||hef, in that order

	var headerDigest [sha256.Size]byte
	copy(headerDigest[:], head[20:packageHeaderSize])
	return &PackageReader{
		meta:         meta,
		hef:          io.TeeReader(io.LimitReader(r, int64(hefLen)), hasher),
		hasher:       hasher,
		headerDigest: headerDigest,
	}, nil
}

// Meta returns the decoded package metadata.
func (p *PackageReader) Meta() *PackageMeta { return p.meta }

// HEF streams the embedded HEF bytes. Every read feeds the package digest;
// consume the stream fully, then call Verify. The same reader is returned on
// repeated calls so bytes are never hashed twice.
func (p *PackageReader) HEF() io.Reader { return p.hef }

// Verify compares the running sha256(json||hef) digest against the header
// and must be called after HEF() has been fully consumed. Truncated uploads
// surface here as digest mismatches (fewer bytes hashed than declared).
func (p *PackageReader) Verify() error {
	var probe [1]byte
	if n, _ := p.hef.Read(probe[:]); n > 0 {
		return fmt.Errorf("model package HEF section was not fully consumed before Verify")
	}
	sum := p.hasher.Sum(nil)
	if !bytes.Equal(sum, p.headerDigest[:]) {
		return fmt.Errorf("model package integrity check failed (sha256 mismatch — file corrupted or truncated)")
	}
	return nil
}
