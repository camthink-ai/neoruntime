package utils

import (
	"archive/tar"
	"encoding/json"
	"io"
	"os"
)

// ExtractImageNameFromTar reads manifest.json from a docker/OCI image tar archive
// and returns the first RepoTag (e.g. "parking-lot:1.0.0").
//
// Docker `save` produces a tar whose top-level manifest.json is an array of
// entries, each with a RepoTags field. We return the first tag of the first
// entry, or "" if the archive has no usable tag.
//
// This is shared between platform-api (upload-image, to surface the tag to the
// UI) and app-manager (install, to reconcile the tar's tag against
// manifest.image). Read-only: it never writes or executes archive contents.
func ExtractImageNameFromTar(tarPath string) string {
	f, err := os.Open(tarPath)
	if err != nil {
		return ""
	}
	defer f.Close()

	tr := tar.NewReader(f)
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return ""
		}
		if hdr.Name != "manifest.json" {
			continue
		}
		data, err := io.ReadAll(tr)
		if err != nil {
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
