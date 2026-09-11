package server

import (
	"fmt"
	"os"
	"path/filepath"

	"aipc/platform/common/constants"
)

func appStagingDirFor(path string, tokenPattern func(string) bool) (string, bool) {
	if path == "" || !filepath.IsAbs(path) {
		return "", false
	}
	root := filepath.Join(constants.RootPath(), "apps", "staging")
	rel, err := filepath.Rel(root, filepath.Clean(path))
	if err != nil {
		return "", false
	}
	dir, file := filepath.Split(rel)
	token := filepath.Base(filepath.Clean(dir))
	if file == "" || filepath.Dir(filepath.Clean(dir)) != "." || !tokenPattern(token) {
		return "", false
	}
	tokenDir := filepath.Join(root, token)
	info, err := os.Lstat(tokenDir)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return "", false
	}
	return tokenDir, true
}

// appStagingDir recognizes only unclaimed platform-api staging. External CLI
// paths and inflight task directories are deliberately excluded.
func appStagingDir(path string) (string, bool) {
	return appStagingDirFor(path, appStagingTokenPattern.MatchString)
}

func appInflightDir(path string) (string, bool) {
	return appStagingDirFor(path, appInflightTokenPattern.MatchString)
}

// claimAppStaging atomically transfers platform-api staging ownership to an
// accepted task by renaming each token directory. Abandon/TTL only recognize
// ordinary token names, so they cannot delete artifacts while app-manager is
// waiting on its per-app lock or importing an image.
func claimAppStaging(taskID string, paths ...string) ([]string, error) {
	claimed := append([]string(nil), paths...)
	oldByIndex := make([]string, len(paths))
	byOld := make(map[string]string)

	// Discover every source before moving any directory: package uploads keep
	// app.yaml and image.tar under one token, so recognizing the second path
	// after moving the first would fail and leave it pointing at the old name.
	for i, path := range paths {
		oldDir, ok := appStagingDir(path)
		if !ok {
			continue // external CLI path: caller-owned, never renamed/deleted
		}
		oldByIndex[i] = oldDir
		if _, seen := byOld[oldDir]; !seen {
			byOld[oldDir] = ""
		}
	}

	type move struct{ oldDir, newDir string }
	moves := make([]move, 0, len(byOld))
	for oldDir := range byOld {
		newDir := filepath.Join(filepath.Dir(oldDir), fmt.Sprintf("inflight-%s-%d", taskID, len(moves)))
		if err := os.Rename(oldDir, newDir); err != nil {
			for j := len(moves) - 1; j >= 0; j-- {
				_ = os.Rename(moves[j].newDir, moves[j].oldDir)
			}
			return nil, fmt.Errorf("claim app staging %s: %w", oldDir, err)
		}
		byOld[oldDir] = newDir
		moves = append(moves, move{oldDir: oldDir, newDir: newDir})
	}
	for i, oldDir := range oldByIndex {
		if oldDir != "" {
			claimed[i] = filepath.Join(byOld[oldDir], filepath.Base(paths[i]))
		}
	}
	return claimed, nil
}

func cleanupOwnedAppStaging(paths ...string) {
	dirs := make(map[string]struct{})
	for _, path := range paths {
		if dir, ok := appInflightDir(path); ok {
			dirs[dir] = struct{}{}
		}
	}
	for dir := range dirs {
		_ = os.RemoveAll(dir)
	}
}
