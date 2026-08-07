package osupgrade

import (
	"bufio"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"
)

const maxDescriptionSize = 2 << 20
const SingleRecoveryMarker = "AIPC_LOCAL_RECOVERY_V1"

type ValidationOptions struct {
	ExpectedSHA256       string
	ExpectedMachine      string
	ExpectedProduct      string
	ExpectedHW           string
	ExpectedDevice       string
	RequireBuildTime     bool
	RequireAB            bool
	RequireCompatibility bool
	RequireSignature     bool
}

type ValidationResult struct {
	SHA256                  string
	Version                 string
	BuildTime               string
	Machine                 string
	Product                 string
	HardwareVersion         string
	RootFSName              string
	FilesystemDevice        string
	UpdateModes             []string
	SupportsAB              bool
	SupportsStandardCopyA   bool
	SupportsStandardCopyB   bool
	SupportsFullCopyA       bool
	SupportsFullCopyB       bool
	SupportsStandardUpgrade bool
	SupportsFullUpgrade     bool
	SingleRecoveryCapable   bool
	MinRecoveryVersion      string
	SecureBootKeyID         string
	CompatLevel             int
	DataSchema              int
	Entries                 int
	HasSignature            bool
}

var (
	versionRE     = regexp.MustCompile(`(?m)\bversion\s*=\s*"([^"]+)"`)
	buildRE       = regexp.MustCompile(`(?m)\b(?:build[_-]?time|build[_-]?date)\s*=\s*"([^"]+)"`)
	productRE     = regexp.MustCompile(`(?m)\bproduct\s*=\s*"([^"]+)"`)
	hwRE          = regexp.MustCompile(`(?m)\bhardware-compatibility\s*:\s*\[\s*"([^"]+)"`)
	deviceRE      = regexp.MustCompile(`(?m)\bFILESYSTEM_DEVICE\s*=\s*['"]([^'"]+)['"]`)
	minRecoveryRE = regexp.MustCompile(`(?m)\bmin-recovery-version\s*=\s*"([^"]+)"`)
	keyIDRE       = regexp.MustCompile(`(?m)\bsecure-boot-key-id\s*=\s*"([^"]+)"`)
	compatLevelRE = regexp.MustCompile(`(?m)\baipc-compat-level\s*=\s*"([0-9]+)"`)
	dataSchemaRE  = regexp.MustCompile(`(?m)\bdata-schema\s*=\s*"([0-9]+)"`)
)

func ValidatePackage(path string, opts ValidationOptions) (*ValidationResult, error) {
	if !strings.HasSuffix(strings.ToLower(path), ".swu") {
		return nil, fmt.Errorf("package must use .swu extension")
	}
	sum, err := fileSHA256(path)
	if err != nil {
		return nil, err
	}
	if expected := strings.ToLower(strings.TrimSpace(opts.ExpectedSHA256)); expected != "" && expected != sum {
		return nil, fmt.Errorf("SHA-256 mismatch: expected %s, got %s", expected, sum)
	}

	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	result := &ValidationResult{SHA256: sum}
	var description string
	reader := bufio.NewReaderSize(f, 128*1024)
	for {
		header := make([]byte, 110)
		if _, err := io.ReadFull(reader, header); err != nil {
			return nil, fmt.Errorf("invalid CPIO header: %w", err)
		}
		magic := string(header[:6])
		if magic != "070701" && magic != "070702" {
			return nil, fmt.Errorf("unsupported CPIO format %q", magic)
		}
		fileSize, err := parseHex(header[54:62])
		if err != nil {
			return nil, fmt.Errorf("invalid CPIO file size: %w", err)
		}
		nameSize, err := parseHex(header[94:102])
		if err != nil || nameSize < 1 || nameSize > 4096 {
			return nil, fmt.Errorf("invalid CPIO name size")
		}
		nameBytes := make([]byte, nameSize)
		if _, err := io.ReadFull(reader, nameBytes); err != nil {
			return nil, fmt.Errorf("truncated CPIO file name: %w", err)
		}
		name := strings.TrimSuffix(string(nameBytes), "\x00")
		if err := discardPadding(reader, 110+nameSize); err != nil {
			return nil, err
		}
		if name == "TRAILER!!!" {
			break
		}
		result.Entries++
		if name == "sw-description.sig" || strings.HasSuffix(name, "/sw-description.sig") {
			result.HasSignature = true
		}

		limited := &io.LimitedReader{R: reader, N: int64(fileSize)}
		switch {
		case name == "sw-description" || strings.HasSuffix(name, "/sw-description"):
			if fileSize > maxDescriptionSize {
				return nil, fmt.Errorf("sw-description is too large")
			}
			data, err := io.ReadAll(limited)
			if err != nil {
				return nil, fmt.Errorf("cannot read sw-description: %w", err)
			}
			description = string(data)
		case strings.HasSuffix(strings.ToLower(name), ".ext4.gz"):
			recoveryImage := strings.HasPrefix(strings.ToLower(name), "swupdate-image-")
			if !recoveryImage {
				result.RootFSName = name
			}
			gz, err := gzip.NewReader(limited)
			if err != nil {
				return nil, fmt.Errorf("rootfs %s is not valid gzip: %w", name, err)
			}
			writer := io.Writer(io.Discard)
			var marker *markerWriter
			if recoveryImage {
				marker = newMarkerWriter([]byte(SingleRecoveryMarker))
				writer = marker
			}
			if _, err := io.Copy(writer, gz); err != nil {
				_ = gz.Close()
				return nil, fmt.Errorf("rootfs %s is corrupt: %w", name, err)
			}
			if err := gz.Close(); err != nil {
				return nil, fmt.Errorf("rootfs %s is corrupt: %w", name, err)
			}
			if marker != nil {
				result.SingleRecoveryCapable = marker.Found()
			}
			if _, err := io.Copy(io.Discard, limited); err != nil {
				return nil, err
			}
		default:
			if _, err := io.Copy(io.Discard, limited); err != nil {
				return nil, fmt.Errorf("truncated CPIO entry %s: %w", name, err)
			}
		}
		if limited.N != 0 {
			return nil, fmt.Errorf("truncated CPIO entry %s", name)
		}
		if err := discardPadding(reader, fileSize); err != nil {
			return nil, err
		}
	}

	if description == "" {
		return nil, fmt.Errorf("package does not contain sw-description")
	}
	if result.RootFSName == "" {
		return nil, fmt.Errorf("package does not contain a readable rootfs image")
	}
	result.Version = firstMatch(versionRE, description)
	result.BuildTime = firstMatch(buildRE, description)
	result.Product = firstMatch(productRE, description)
	result.HardwareVersion = firstMatch(hwRE, description)
	result.FilesystemDevice = firstMatch(deviceRE, description)
	result.UpdateModes = parseUpdateModes(description)
	result.SupportsFullCopyA = stringInSlice(result.UpdateModes, "copy-a")
	result.SupportsFullCopyB = stringInSlice(result.UpdateModes, "copy-b")
	result.SupportsStandardCopyA = stringInSlice(result.UpdateModes, "copy-a")
	result.SupportsStandardCopyB = stringInSlice(result.UpdateModes, "copy-b")
	result.SupportsFullUpgrade = result.SupportsFullCopyA && result.SupportsFullCopyB
	result.SupportsStandardUpgrade = result.SupportsStandardCopyA
	result.SupportsAB = result.SupportsFullUpgrade || result.SupportsStandardUpgrade
	result.MinRecoveryVersion = firstMatch(minRecoveryRE, description)
	result.SecureBootKeyID = firstMatch(keyIDRE, description)
	result.CompatLevel, _ = strconv.Atoi(firstMatch(compatLevelRE, description))
	result.DataSchema, _ = strconv.Atoi(firstMatch(dataSchemaRE, description))
	if result.Version == "" {
		return nil, fmt.Errorf("sw-description does not contain a target version")
	}
	if result.BuildTime == "" && opts.RequireBuildTime {
		return nil, fmt.Errorf("sw-description does not contain a build time")
	}
	if result.BuildTime != "" && !validBuildTime(result.BuildTime) {
		return nil, fmt.Errorf("invalid build time %q", result.BuildTime)
	}
	if opts.RequireAB && !result.SupportsAB {
		return nil, fmt.Errorf("sw-description does not define stable/copy-a")
	}
	if opts.RequireCompatibility && (result.CompatLevel <= 0 || result.DataSchema <= 0) {
		return nil, fmt.Errorf("sw-description does not contain valid aipc-compat-level and data-schema")
	}
	if opts.RequireSignature && !result.HasSignature {
		return nil, fmt.Errorf("package does not contain sw-description.sig")
	}
	if expected := strings.TrimSpace(opts.ExpectedDevice); expected != "" &&
		result.FilesystemDevice != expected {
		return nil, fmt.Errorf(
			"filesystem device mismatch: package=%q device=%q",
			result.FilesystemDevice,
			expected,
		)
	}
	result.Machine = detectMachine(description + "\n" + result.RootFSName)
	if expected := strings.TrimSpace(opts.ExpectedMachine); expected != "" && result.Machine != expected {
		return nil, fmt.Errorf("machine mismatch: package=%q device=%q", result.Machine, expected)
	}
	if expected := strings.TrimSpace(opts.ExpectedProduct); expected != "" {
		if result.Product == "" || !strings.EqualFold(result.Product, expected) {
			return nil, fmt.Errorf("product mismatch: package=%q device=%q", result.Product, expected)
		}
	}
	if expected := strings.TrimSpace(opts.ExpectedHW); expected != "" {
		if result.HardwareVersion == "" || result.HardwareVersion != expected {
			return nil, fmt.Errorf("hardware mismatch: package=%q device=%q", result.HardwareVersion, expected)
		}
	}
	return result, nil
}

type markerWriter struct {
	marker []byte
	match  int
	found  bool
}

func newMarkerWriter(marker []byte) *markerWriter {
	return &markerWriter{marker: marker}
}

func (w *markerWriter) Write(data []byte) (int, error) {
	for _, value := range data {
		if w.found {
			continue
		}
		if value == w.marker[w.match] {
			w.match++
			if w.match == len(w.marker) {
				w.found = true
			}
			continue
		}
		if value == w.marker[0] {
			w.match = 1
		} else {
			w.match = 0
		}
	}
	return len(data), nil
}

func (w *markerWriter) Found() bool { return w.found }

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func parseHex(raw []byte) (uint64, error) {
	return strconv.ParseUint(string(raw), 16, 64)
}

func discardPadding(r io.Reader, size uint64) error {
	padding := (4 - (size % 4)) % 4
	if padding == 0 {
		return nil
	}
	_, err := io.CopyN(io.Discard, r, int64(padding))
	return err
}

func firstMatch(re *regexp.Regexp, value string) string {
	match := re.FindStringSubmatch(value)
	if len(match) == 2 {
		return strings.TrimSpace(match[1])
	}
	return ""
}

func parseUpdateModes(description string) []string {
	stable, ok := findConfigBlock(description, "stable")
	if !ok {
		return nil
	}
	masked := maskConfigLiterals(stable)
	modes := make([]string, 0)
	seen := make(map[string]struct{})
	depth := 0
	for index := 0; index < len(masked); {
		switch masked[index] {
		case '{':
			depth++
			index++
			continue
		case '}':
			if depth > 0 {
				depth--
			}
			index++
			continue
		}
		if depth != 0 || !isConfigIdentChar(masked[index]) {
			index++
			continue
		}
		start := index
		for index < len(masked) && isConfigIdentChar(masked[index]) {
			index++
		}
		mode := masked[start:index]
		next := skipConfigSpaces(masked, index)
		if next >= len(masked) || masked[next] != ':' {
			continue
		}
		open := skipConfigSpaces(masked, next+1)
		if open >= len(masked) || masked[open] != '{' {
			continue
		}
		if _, exists := seen[mode]; !exists {
			seen[mode] = struct{}{}
			modes = append(modes, mode)
		}
		index = open
	}
	return modes
}

func findConfigBlock(source, key string) (string, bool) {
	masked := maskConfigLiterals(source)
	for offset := 0; offset < len(masked); {
		position := strings.Index(masked[offset:], key)
		if position < 0 {
			return "", false
		}
		start := offset + position
		end := start + len(key)
		if !isConfigBoundary(masked, start-1) || !isConfigBoundary(masked, end) {
			offset = end
			continue
		}
		assign := skipConfigSpaces(masked, end)
		if assign >= len(masked) || masked[assign] != '=' {
			offset = end
			continue
		}
		open := skipConfigSpaces(masked, assign+1)
		if open >= len(masked) || masked[open] != '{' {
			offset = end
			continue
		}
		depth := 1
		for index := open + 1; index < len(masked); index++ {
			switch masked[index] {
			case '{':
				depth++
			case '}':
				depth--
				if depth == 0 {
					return source[open+1 : index], true
				}
			}
		}
		return "", false
	}
	return "", false
}

func maskConfigLiterals(source string) string {
	masked := []byte(source)
	for index := 0; index < len(masked); {
		switch masked[index] {
		case '"', '\'':
			quote := masked[index]
			index++
			for index < len(masked) {
				if masked[index] == '\\' {
					masked[index] = ' '
					if index+1 < len(masked) {
						masked[index+1] = ' '
					}
					index += 2
					continue
				}
				if masked[index] == quote {
					index++
					break
				}
				if masked[index] != '\n' && masked[index] != '\r' {
					masked[index] = ' '
				}
				index++
			}
		case '/':
			if index+1 >= len(masked) {
				index++
				continue
			}
			switch masked[index+1] {
			case '/':
				masked[index], masked[index+1] = ' ', ' '
				index += 2
				for index < len(masked) && masked[index] != '\n' && masked[index] != '\r' {
					masked[index] = ' '
					index++
				}
			case '*':
				masked[index], masked[index+1] = ' ', ' '
				index += 2
				for index+1 < len(masked) {
					if masked[index] == '*' && masked[index+1] == '/' {
						masked[index], masked[index+1] = ' ', ' '
						index += 2
						break
					}
					if masked[index] != '\n' && masked[index] != '\r' {
						masked[index] = ' '
					}
					index++
				}
			default:
				index++
			}
		case '#':
			masked[index] = ' '
			index++
			for index < len(masked) && masked[index] != '\n' && masked[index] != '\r' {
				masked[index] = ' '
				index++
			}
		default:
			index++
		}
	}
	return string(masked)
}

func skipConfigSpaces(value string, index int) int {
	for index < len(value) && (value[index] == ' ' || value[index] == '\t' || value[index] == '\n' || value[index] == '\r') {
		index++
	}
	return index
}

func isConfigBoundary(value string, index int) bool {
	return index < 0 || index >= len(value) || !isConfigIdentChar(value[index])
}

func isConfigIdentChar(value byte) bool {
	return value >= 'a' && value <= 'z' ||
		value >= 'A' && value <= 'Z' ||
		value >= '0' && value <= '9' ||
		value == '_' || value == '-' || value == '.'
}

func stringInSlice(values []string, target string) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}

func detectMachine(value string) string {
	for _, machine := range []string{"hailo15-ne503", "hailo15-sbc"} {
		if strings.Contains(strings.ToLower(value), machine) {
			return machine
		}
	}
	return ""
}

func validBuildTime(value string) bool {
	for _, layout := range []string{
		time.RFC3339,
		"2006-01-02 15:04:05",
		"20060102-150405",
		"20060102150405",
		"2006-01-02",
	} {
		if _, err := time.Parse(layout, value); err == nil {
			return true
		}
	}
	return false
}
