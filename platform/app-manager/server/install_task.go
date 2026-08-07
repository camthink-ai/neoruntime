package server

import (
	"fmt"
	"sync"
	"time"

	"github.com/google/uuid"
)

type InstallTask struct {
	ID        string
	Phase     string // validating, pulling, unpacking, registering, complete, error
	Percent   float64
	Message   string
	AppID     string
	Error     string
	CreatedAt time.Time
	mu        sync.RWMutex
}

type InstallTaskStore struct {
	mu    sync.RWMutex
	tasks map[string]*InstallTask
}

func NewInstallTaskStore() *InstallTaskStore {
	s := &InstallTaskStore{tasks: make(map[string]*InstallTask)}
	go s.cleanupLoop()
	return s
}

func (s *InstallTaskStore) Create() *InstallTask {
	t := &InstallTask{
		ID:        uuid.New().String()[:8],
		Phase:     "validating",
		Percent:   0,
		Message:   "Preparing installation...",
		CreatedAt: time.Now(),
	}
	s.mu.Lock()
	s.tasks[t.ID] = t
	s.mu.Unlock()
	return t
}

func (s *InstallTaskStore) Get(id string) (*InstallTask, bool) {
	s.mu.RLock()
	t, ok := s.tasks[id]
	s.mu.RUnlock()
	return t, ok
}

func (t *InstallTask) Update(phase string, percent float64, message string) {
	t.mu.Lock()
	t.Phase = phase
	t.Percent = percent
	t.Message = message
	t.mu.Unlock()
}

func (t *InstallTask) Complete(appID string) {
	t.mu.Lock()
	t.Phase = "complete"
	t.Percent = 100
	t.AppID = appID
	t.Message = "Installation complete"
	t.mu.Unlock()
}

func (t *InstallTask) Fail(errMsg string) {
	t.mu.Lock()
	t.Phase = "error"
	t.Error = errMsg
	t.Message = fmt.Sprintf("Installation failed: %s", errMsg)
	t.mu.Unlock()
}

func (t *InstallTask) Snapshot() (phase string, percent float64, message, appID, errMsg string) {
	t.mu.RLock()
	defer t.mu.RUnlock()
	return t.Phase, t.Percent, t.Message, t.AppID, t.Error
}

func (s *InstallTaskStore) cleanupLoop() {
	ticker := time.NewTicker(5 * time.Minute)
	defer ticker.Stop()
	for range ticker.C {
		s.mu.Lock()
		for id, t := range s.tasks {
			if time.Since(t.CreatedAt) > 30*time.Minute {
				delete(s.tasks, id)
			}
		}
		s.mu.Unlock()
	}
}
