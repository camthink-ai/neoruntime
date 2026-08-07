package osupgrade

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// ExtractRecoveryArtifacts copies only the kernel and recovery rootfs from an
// SWU CPIO archive. The main rootfs is skipped without buffering or extracting.
func ExtractRecoveryArtifacts(packagePath, destination string) error {
	if err := os.MkdirAll(destination, 0755); err != nil {
		return err
	}
	file, err := os.Open(packagePath)
	if err != nil {
		return err
	}
	defer file.Close()

	foundFit := false
	foundRecovery := false
	reader := bufio.NewReaderSize(file, 128*1024)
	for {
		header := make([]byte, 110)
		if _, err := io.ReadFull(reader, header); err != nil {
			return fmt.Errorf("invalid CPIO header: %w", err)
		}
		fileSize, err := parseHex(header[54:62])
		if err != nil {
			return err
		}
		nameSize, err := parseHex(header[94:102])
		if err != nil || nameSize < 1 || nameSize > 4096 {
			return fmt.Errorf("invalid CPIO name size")
		}
		nameBytes := make([]byte, nameSize)
		if _, err := io.ReadFull(reader, nameBytes); err != nil {
			return err
		}
		name := strings.TrimSuffix(string(nameBytes), "\x00")
		if err := discardPadding(reader, 110+nameSize); err != nil {
			return err
		}
		if name == "TRAILER!!!" {
			break
		}
		limited := &io.LimitedReader{R: reader, N: int64(fileSize)}
		base := filepath.Base(name)
		switch {
		case base == "fitImage":
			if err := writeAtomicStream(filepath.Join(destination, base), limited, 0644); err != nil {
				return err
			}
			foundFit = true
		case strings.HasPrefix(base, "swupdate-image-") && strings.HasSuffix(base, ".ext4.gz"):
			if err := writeAtomicStream(filepath.Join(destination, base), limited, 0644); err != nil {
				return err
			}
			foundRecovery = true
		default:
			if _, err := io.Copy(io.Discard, limited); err != nil {
				return err
			}
		}
		if limited.N != 0 {
			return fmt.Errorf("truncated CPIO entry %s", name)
		}
		if err := discardPadding(reader, fileSize); err != nil {
			return err
		}
	}
	if !foundFit || !foundRecovery {
		return fmt.Errorf("SWU is missing recovery boot artifacts: fitImage=%t recovery=%t", foundFit, foundRecovery)
	}
	return nil
}

func writeAtomicStream(path string, source io.Reader, mode os.FileMode) error {
	dir := filepath.Dir(path)
	temp, err := os.CreateTemp(dir, ".aipc-os-recovery-*")
	if err != nil {
		return err
	}
	tempPath := temp.Name()
	defer os.Remove(tempPath)
	if err := temp.Chmod(mode); err != nil {
		temp.Close()
		return err
	}
	if _, err := io.Copy(temp, source); err != nil {
		temp.Close()
		return err
	}
	if err := temp.Sync(); err != nil {
		temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	return os.Rename(tempPath, path)
}
