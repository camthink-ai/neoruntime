package utils

import (
	"archive/tar"
	"bufio"
	"compress/gzip"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
)

// dockerSaveManifest mirrors one entry of the root manifest.json written by
// `docker save` (the legacy layout the containerd importer consumes).
type dockerSaveManifest struct {
	Config   string   `json:"Config"`
	RepoTags []string `json:"RepoTags"`
	Layers   []string `json:"Layers"`
}

// maxDockerManifestBytes caps the manifest.json entry read out of an
// untrusted image archive (both readers below parse uploaded files, never
// files the platform produced). Real docker-save manifests are a few KB of
// config/layers references; a larger entry is a crafted archive trying to
// balloon daemon memory, not an image.
const maxDockerManifestBytes int64 = 1 << 20

// openTarStream opens tarPath and returns a tar.Reader over it, transparently
// decompressing gzip archives (uploads of .tar.gz / .tgz). The returned closer
// releases every resource opened here.
func openTarStream(path string) (*tar.Reader, io.Closer, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, nil, fmt.Errorf("open archive: %w", err)
	}

	closers := multiCloser{f}
	br := bufio.NewReader(f)
	var src io.Reader = br

	// gzip magic: 1f 8b
	if magic, _ := br.Peek(2); len(magic) == 2 && magic[0] == 0x1f && magic[1] == 0x8b {
		zr, zerr := gzip.NewReader(br)
		if zerr != nil {
			f.Close()
			return nil, nil, fmt.Errorf("open gzip stream: %w", zerr)
		}
		closers = append(closers, zr)
		src = zr
	}

	return tar.NewReader(src), closers, nil
}

type multiCloser []io.Closer

func (m multiCloser) Close() error {
	var firstErr error
	for _, c := range m {
		if err := c.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

// ValidateDockerSaveTar checks that path is a docker-save (legacy layout)
// image archive the containerd importer can actually consume:
//
//   - the root manifest.json exists and parses as a JSON array;
//   - that entry is capped at maxDockerManifestBytes — an oversized
//     "manifest" is a crafted archive, not an image;
//   - every entry's Config and Layers[] reference members that exist in the
//     archive. Digest-style references ("sha256:<digest>") fail here with a
//     "not found in archive" error — the importer resolves layers by member
//     path, not digest, and only reports that at install time otherwise;
//   - empty RepoTags is legal (install-time retag from manifest.image covers
//     untagged images).
func ValidateDockerSaveTar(path string) error {
	tr, closer, err := openTarStream(path)
	if err != nil {
		return err
	}
	defer closer.Close()

	members := map[string]bool{}
	var manifests []dockerSaveManifest
	foundManifest := false
	for {
		hdr, err := tr.Next()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return fmt.Errorf("read archive: %w", err)
		}
		name := strings.TrimPrefix(hdr.Name, "./")
		if strings.HasSuffix(name, "/") {
			continue // directory entry
		}
		members[name] = true
		if name != "manifest.json" {
			continue
		}
		data, err := io.ReadAll(io.LimitReader(tr, maxDockerManifestBytes+1))
		if err != nil {
			return fmt.Errorf("read manifest.json: %w", err)
		}
		if int64(len(data)) > maxDockerManifestBytes {
			return fmt.Errorf("manifest.json exceeds %dMB (not a docker-save archive)", maxDockerManifestBytes>>20)
		}
		if err := json.Unmarshal(data, &manifests); err != nil {
			return fmt.Errorf("parse manifest.json: %w", err)
		}
		foundManifest = true
	}
	if !foundManifest {
		return errors.New("manifest.json not found in archive")
	}
	if len(manifests) == 0 {
		return errors.New("manifest.json contains no entries")
	}

	for i, m := range manifests {
		if m.Config == "" {
			return fmt.Errorf("manifest entry %d: missing Config", i)
		}
		if !members[m.Config] {
			return fmt.Errorf("manifest entry %d: config %q not found in archive", i, m.Config)
		}
		if len(m.Layers) == 0 {
			return fmt.Errorf("manifest entry %d: no layers declared", i)
		}
		for _, layer := range m.Layers {
			if !members[layer] {
				return fmt.Errorf("manifest entry %d: layer %q not found in archive", i, layer)
			}
		}
	}
	return nil
}

// ExtractImageNameFromTar reads manifest.json from a docker/OCI image tar archive
// and returns the first RepoTag (e.g. "parking-lot:1.0.0").
//
// Docker `save` produces a tar whose top-level manifest.json is an array of
// entries, each with a RepoTags field. We return the first tag of the first
// entry, or "" if the archive has no usable tag. Gzip-compressed archives are
// handled transparently.
//
// This is shared between platform-api (upload-image, to surface the tag to the
// UI) and app-manager (install, to reconcile the tar's tag against
// manifest.image). Read-only: it never writes or executes archive contents.
func ExtractImageNameFromTar(tarPath string) string {
	tr, closer, err := openTarStream(tarPath)
	if err != nil {
		return ""
	}
	defer closer.Close()

	for {
		hdr, err := tr.Next()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return ""
		}
		if strings.TrimPrefix(hdr.Name, "./") != "manifest.json" {
			continue
		}
		data, err := io.ReadAll(io.LimitReader(tr, maxDockerManifestBytes+1))
		if err != nil || int64(len(data)) > maxDockerManifestBytes {
			return ""
		}
		// manifest.json is an array of objects.
		var manifests []struct {
			RepoTags []string `json:"RepoTags"`
		}
		if err := json.Unmarshal(data, &manifests); err != nil {
			return ""
		}
		if len(manifests) > 0 && len(manifests[0].RepoTags) > 0 {
			return manifests[0].RepoTags[0]
		}
	}
	return ""
}

// ExtractAllImageNamesFromTar returns every RepoTag of every image in the
// archive's manifest.json — `docker save img1 img2` bundles several images
// in one tar, and containerd's importer brings them all in on a single
// Import. The install reconcile uses this to tell "the archive carries this
// reference" (bind it to its own archived image) from "the archive lacks it"
// (must fail instead of being papered over by a retag from an unrelated
// image). Order follows the archive. Gzip archives are handled
// transparently. Read-only.
func ExtractAllImageNamesFromTar(tarPath string) []string {
	tr, closer, err := openTarStream(tarPath)
	if err != nil {
		return nil
	}
	defer closer.Close()

	for {
		hdr, err := tr.Next()
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return nil
		}
		if strings.TrimPrefix(hdr.Name, "./") != "manifest.json" {
			continue
		}
		data, err := io.ReadAll(io.LimitReader(tr, maxDockerManifestBytes+1))
		if err != nil || int64(len(data)) > maxDockerManifestBytes {
			return nil
		}
		var manifests []struct {
			RepoTags []string `json:"RepoTags"`
		}
		if err := json.Unmarshal(data, &manifests); err != nil {
			return nil
		}
		var tags []string
		for _, m := range manifests {
			tags = append(tags, m.RepoTags...)
		}
		return tags
	}
}
