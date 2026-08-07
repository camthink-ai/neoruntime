package systemparams

import (
	"context"
	"errors"
	"sync"
	"testing"
)

// fakeStore is an in-memory settingStore for unit-testing the adapter without
// a real database.
type fakeStore struct {
	mu   sync.Mutex
	data map[string]string

	failGet error
	failSet error
}

func newFakeStore() *fakeStore {
	return &fakeStore{data: make(map[string]string)}
}

func (s *fakeStore) Get(key string) (string, error) {
	if s.failGet != nil {
		return "", s.failGet
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.data[key], nil
}

func (s *fakeStore) Set(key, value string) error {
	if s.failSet != nil {
		return s.failSet
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.data[key] = value
	return nil
}

func ctx() context.Context { return context.Background() }

func TestValidate(t *testing.T) {
	a := &Adapter{store: newFakeStore()}
	if err := a.Validate(ctx(), "k", `{"value":"v1"}`); err != nil {
		t.Fatalf("Validate good: %v", err)
	}
	if err := a.Validate(ctx(), "k", `{not json`); !errors.Is(err, ErrInvalidJSON) {
		t.Fatalf("Validate bad = %v, want ErrInvalidJSON", err)
	}
}

func TestBackup_NewKey(t *testing.T) {
	a := &Adapter{store: newFakeStore()}
	b, err := a.Backup(ctx(), "k")
	if err != nil {
		t.Fatalf("Backup: %v", err)
	}
	if b != "" {
		t.Fatalf("Backup new key = %v, want empty", b)
	}
}

func TestBackup_ExistingKey(t *testing.T) {
	s := newFakeStore()
	s.data["k"] = "old"
	a := &Adapter{store: s}
	b, err := a.Backup(ctx(), "k")
	if err != nil {
		t.Fatalf("Backup: %v", err)
	}
	if b != "old" {
		t.Fatalf("Backup = %v, want old", b)
	}
}

func TestApply_AndVerify(t *testing.T) {
	s := newFakeStore()
	a := &Adapter{store: s}

	rendered, err := a.Render(ctx(), "k", `{"value":"v1"}`)
	if err != nil {
		t.Fatalf("Render: %v", err)
	}
	if rendered != `{"value":"v1"}` {
		t.Fatalf("Render = %v", rendered)
	}

	if err := a.Apply(ctx(), "k", rendered); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if s.data["k"] != "v1" {
		t.Fatalf("after Apply data[k]=%q, want v1", s.data["k"])
	}
	if err := a.Verify(ctx(), "k", `{"value":"v1"}`); err != nil {
		t.Fatalf("Verify: %v", err)
	}
}

func TestVerify_Mismatch(t *testing.T) {
	s := newFakeStore()
	s.data["k"] = "different"
	a := &Adapter{store: s}
	err := a.Verify(ctx(), "k", `{"value":"v1"}`)
	if err == nil {
		t.Fatal("Verify succeeded; want mismatch error")
	}
}

func TestRestore(t *testing.T) {
	s := newFakeStore()
	s.data["k"] = "new"
	a := &Adapter{store: s}

	if err := a.Restore(ctx(), "k", "old"); err != nil {
		t.Fatalf("Restore: %v", err)
	}
	if s.data["k"] != "old" {
		t.Fatalf("after Restore data[k]=%q, want old", s.data["k"])
	}
}

func TestApply_FailSet(t *testing.T) {
	s := newFakeStore()
	s.failSet = errors.New("db locked")
	a := &Adapter{store: s}
	if err := a.Apply(ctx(), "k", `{"value":"v1"}`); err == nil {
		t.Fatal("Apply succeeded; want failSet")
	}
}

func TestApply_BadRenderedType(t *testing.T) {
	a := &Adapter{store: newFakeStore()}
	if err := a.Apply(ctx(), "k", 123); err == nil {
		t.Fatal("Apply succeeded; want type error")
	}
}

func TestBackup_FailGet(t *testing.T) {
	s := newFakeStore()
	s.failGet = errors.New("db locked")
	a := &Adapter{store: s}
	if _, err := a.Backup(ctx(), "k"); err == nil {
		t.Fatal("Backup succeeded; want failGet")
	}
}

func TestVerify_FailGet(t *testing.T) {
	s := newFakeStore()
	s.failGet = errors.New("db locked")
	a := &Adapter{store: s}
	if err := a.Verify(ctx(), "k", `{"value":"v1"}`); err == nil {
		t.Fatal("Verify succeeded; want failGet")
	}
}

func TestNewReturnsAdapter(t *testing.T) {
	a := New(nil)
	if a == nil {
		t.Fatal("New returned nil")
	}
}
