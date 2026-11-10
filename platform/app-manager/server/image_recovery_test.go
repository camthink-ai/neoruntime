package server

import (
	"context"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	"aipc/platform/app-manager/stopbudget"
)

func TestImageRecoveryContextSurvivesCallerCancellation(t *testing.T) {
	type contextKey string
	const key contextKey = "namespace"

	parent, cancelParent := context.WithCancel(context.WithValue(context.Background(), key, "aipc"))
	cancelParent()

	ctx, cancel := imageRecoveryContext(parent)
	defer cancel()

	if err := ctx.Err(); err != nil {
		t.Fatalf("recovery context inherited caller cancellation: %v", err)
	}
	if got := ctx.Value(key); got != "aipc" {
		t.Fatalf("recovery context lost caller value: got %v", got)
	}
	deadline, ok := ctx.Deadline()
	if !ok {
		t.Fatal("recovery context has no deadline")
	}
	remaining := time.Until(deadline)
	if remaining <= stopbudget.ImageRecoveryBudget-time.Second || remaining > stopbudget.ImageRecoveryBudget {
		t.Fatalf("recovery deadline remaining = %s, want approximately %s", remaining, stopbudget.ImageRecoveryBudget)
	}
}

func TestRecoverySourceCandidates(t *testing.T) {
	tests := []struct {
		name            string
		requiredRef     string
		primary         bool
		archivedTags    []string
		importedPrimary string
		want            []string
	}{
		{
			name:            "unqualified Docker tag matches normalized manifest ref",
			requiredRef:     "docker.io/neoruntime/model-showcase:1.4.7",
			primary:         true,
			archivedTags:    []string{"neoruntime/model-showcase:1.4.7"},
			importedPrimary: "neoruntime/model-showcase:1.4.7",
			want: []string{
				"neoruntime/model-showcase:1.4.7",
				"docker.io/neoruntime/model-showcase:1.4.7",
			},
		},
		{
			name:            "primary preserves install-time retag behavior",
			requiredRef:     "docker.io/acme/renamed:2.0",
			primary:         true,
			archivedTags:    []string{"acme/original:1.0"},
			importedPrimary: "acme/original:1.0",
			want:            []string{"acme/original:1.0"},
		},
		{
			name:            "secondary cannot use an unrelated primary image",
			requiredRef:     "docker.io/acme/worker:2.0",
			primary:         false,
			archivedTags:    []string{"acme/main:2.0"},
			importedPrimary: "acme/main:2.0",
			want:            nil,
		},
		{
			name:            "secondary selects only its matching archived image",
			requiredRef:     "docker.io/acme/worker:2.0",
			primary:         false,
			archivedTags:    []string{"acme/main:2.0", "acme/worker:2.0"},
			importedPrimary: "acme/main:2.0",
			want: []string{
				"acme/worker:2.0",
				"docker.io/acme/worker:2.0",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := recoverySourceCandidates(tt.requiredRef, tt.primary, tt.archivedTags, tt.importedPrimary)
			if !reflect.DeepEqual(got, tt.want) {
				t.Fatalf("recoverySourceCandidates() = %v, want %v", got, tt.want)
			}
		})
	}
}

func TestSameImageReferences(t *testing.T) {
	if !sameImageReferences(
		[]string{"docker.io/acme/main:2", "docker.io/acme/worker:2"},
		[]string{"docker.io/acme/main:2", "docker.io/acme/worker:2"},
	) {
		t.Fatal("identical image references were treated as changed")
	}
	for _, changed := range [][]string{
		{"docker.io/acme/main:3", "docker.io/acme/worker:2"},
		{"docker.io/acme/worker:2", "docker.io/acme/main:2"},
		{"docker.io/acme/main:2"},
	} {
		if sameImageReferences([]string{"docker.io/acme/main:2", "docker.io/acme/worker:2"}, changed) {
			t.Fatalf("changed image references were treated as identical: %v", changed)
		}
	}
}

func TestShouldDiscardSavedImageArchive(t *testing.T) {
	oldRefs := []string{"docker.io/acme/main:2"}
	tests := []struct {
		name          string
		isUpdate      bool
		hasNewArchive bool
		bindingProven bool
		newRefs       []string
		want          bool
	}{
		{name: "no-upload changed image", isUpdate: true, bindingProven: true, newRefs: []string{"docker.io/acme/main:3"}, want: true},
		{name: "no-upload unchanged image with proven digests", isUpdate: true, bindingProven: true, newRefs: oldRefs, want: false},
		{name: "no-upload unchanged name with re-pushed digest", isUpdate: true, bindingProven: false, newRefs: oldRefs, want: true},
		{name: "unverifiable old binding", isUpdate: true, bindingProven: false, newRefs: oldRefs, want: true},
		{name: "new local archive replaces old binding", isUpdate: true, hasNewArchive: true, bindingProven: false, newRefs: []string{"docker.io/acme/main:3"}, want: false},
		{name: "fresh install has no stale binding", isUpdate: false, bindingProven: false, newRefs: []string{"docker.io/acme/main:3"}, want: false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := shouldDiscardSavedImageArchive(tt.isUpdate, tt.hasNewArchive, tt.bindingProven, oldRefs, tt.newRefs)
			if got != tt.want {
				t.Fatalf("got %v, want %v", got, tt.want)
			}
		})
	}
}

func TestSavedImageDigestsSidecar(t *testing.T) {
	path := filepath.Join(t.TempDir(), "app.tar.digests.json")
	if _, ok := readSavedImageDigestsAt(path); ok {
		t.Fatal("missing sidecar must read as unproven")
	}
	digests := map[string]string{
		"docker.io/acme/main:2":   "sha256:aaaaaaaa",
		"docker.io/acme/worker:2": "sha256:bbbbbbbb",
	}
	if err := writeSavedImageDigestsAt(path, digests); err != nil {
		t.Fatal(err)
	}
	if entries, err := os.ReadDir(filepath.Dir(path)); err != nil {
		t.Fatal(err)
	} else if len(entries) != 1 {
		t.Fatalf("write must leave no temporary file behind, dir has %d entries", len(entries))
	}
	got, ok := readSavedImageDigestsAt(path)
	if !ok || !reflect.DeepEqual(got, digests) {
		t.Fatalf("readSavedImageDigestsAt() = %v, %v; want %v", got, ok, digests)
	}
	// Hand-corrupted content must read as unproven, never as a match.
	if err := os.WriteFile(path, []byte(`{"digests":`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, ok := readSavedImageDigestsAt(path); ok {
		t.Fatal("corrupt sidecar must read as unproven")
	}
}

func TestPublishSavedImageDigestsRemovesStaleBindingOnFailure(t *testing.T) {
	path := filepath.Join(t.TempDir(), "app.tar.digests.json")
	if err := writeSavedImageDigestsAt(path, map[string]string{"docker.io/acme/main:2": "sha256:aaaaaaaa"}); err != nil {
		t.Fatal(err)
	}
	// Force the same-dir temporary write to fail for any user, including
	// root: opening a directory for writing fails with EISDIR.
	if err := os.Mkdir(path+".tmp", 0755); err != nil {
		t.Fatal(err)
	}
	if err := publishSavedImageDigestsAt(path, map[string]string{"docker.io/acme/main:2": "sha256:bbbbbbbb"}); err == nil {
		t.Fatal("publish unexpectedly succeeded against a blocking temporary path")
	}
	if _, err := os.Lstat(path); !os.IsNotExist(err) {
		t.Fatal("failed publish must remove the stale binding instead of leaving it beside the new archive")
	}
	if _, ok := readSavedImageDigestsAt(path); ok {
		t.Fatal("stale sidecar must read as unproven after a failed publish")
	}
}
