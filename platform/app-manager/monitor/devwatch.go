package monitor

import (
	"context"
	"log"
	"os"
	"path/filepath"
	"sync"
	"syscall"
	"time"

	"github.com/fsnotify/fsnotify"
)

// DevWatcher watches host source directories and triggers container reload on file changes.
type DevWatcher struct {
	appID    string
	watcher  *fsnotify.Watcher
	onChange func(appID string) error
	signal   syscall.Signal
	debounce time.Duration

	mu     sync.Mutex
	timer  *time.Timer
	ctx    context.Context
	cancel context.CancelFunc
}

// NewDevWatcher creates a file watcher for dev mode hot reload.
func NewDevWatcher(appID string, signal syscall.Signal, onChange func(appID string) error) (*DevWatcher, error) {
	w, err := fsnotify.NewWatcher()
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithCancel(context.Background())

	return &DevWatcher{
		appID:    appID,
		watcher:  w,
		onChange: onChange,
		signal:   signal,
		debounce: 300 * time.Millisecond,
		ctx:      ctx,
		cancel:   cancel,
	}, nil
}

// WatchDir recursively adds a host directory to the watcher.
func (dw *DevWatcher) WatchDir(root string) error {
	return filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if !info.IsDir() {
			return nil
		}
		base := filepath.Base(path)
		if len(base) > 0 && base[0] == '.' {
			return filepath.SkipDir
		}
		if base == "__pycache__" || base == "node_modules" {
			return filepath.SkipDir
		}
		return dw.watcher.Add(path)
	})
}

// Start begins listening for file change events.
func (dw *DevWatcher) Start() {
	go dw.loop()
}

func (dw *DevWatcher) loop() {
	for {
		select {
		case event, ok := <-dw.watcher.Events:
			if !ok {
				return
			}
			if event.Op&(fsnotify.Write|fsnotify.Create) == 0 {
				continue
			}
			dw.debouncedTrigger()
		case err, ok := <-dw.watcher.Errors:
			if !ok {
				return
			}
			log.Printf("[DevWatcher %s] error: %v", dw.appID, err)
		case <-dw.ctx.Done():
			return
		}
	}
}

func (dw *DevWatcher) debouncedTrigger() {
	dw.mu.Lock()
	defer dw.mu.Unlock()

	if dw.timer != nil {
		dw.timer.Stop()
	}

	dw.timer = time.AfterFunc(dw.debounce, func() {
		log.Printf("[DevWatcher %s] change detected, reloading", dw.appID)
		if err := dw.onChange(dw.appID); err != nil {
			log.Printf("[DevWatcher %s] reload failed: %v", dw.appID, err)
		}
	})
}

// Stop shuts down the watcher.
func (dw *DevWatcher) Stop() {
	dw.cancel()
	dw.watcher.Close()
}

// Signal returns the configured reload signal.
func (dw *DevWatcher) Signal() syscall.Signal {
	return dw.signal
}
