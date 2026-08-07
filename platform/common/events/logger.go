package events

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	"aipc/platform/platform-api/model"
	"gorm.io/gorm"
)

// Logger records events to database with async support
type Logger struct {
	db            *gorm.DB
	mu            sync.RWMutex
	asyncChan     chan *Event
	wg            sync.WaitGroup
	ctx           context.Context
	cancel        context.CancelFunc
	closed        atomic.Bool
	batchSize     int
	flushInterval time.Duration
}

// NewLogger creates a new event logger
func NewLogger(db *gorm.DB) *Logger {
	ctx, cancel := context.WithCancel(context.Background())

	l := &Logger{
		db:            db,
		asyncChan:     make(chan *Event, 1000),
		ctx:           ctx,
		cancel:        cancel,
		batchSize:     10,
		flushInterval: 1 * time.Second,
	}

	l.wg.Add(1)
	go l.asyncProcessor()

	return l
}

// Log sync logs an event to database (blocks until written)
func (l *Logger) Log(event *Event) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.logInternal(event)
}

// LogAsync async logs an event (returns immediately, processes in background)
func (l *Logger) LogAsync(event *Event) {
	if l.closed.Load() {
		return
	}
	select {
	case l.asyncChan <- event:
	default:
		// Channel full, log synchronously
		l.mu.Lock()
		_ = l.logInternal(event)
		l.mu.Unlock()
	}
}

// LogEvent is a convenience method for async logging (non-blocking)
func (l *Logger) LogEvent(
	eventType EventType,
	level EventLevel,
	category EventCategory,
	source, user, message string,
	data map[string]interface{},
) {
	event := &Event{
		EventID:   generateEventID(),
		Timestamp: time.Now().Format(time.RFC3339),
		Type:      eventType,
		Level:     level,
		Category:  category,
		Source:    source,
		User:      user,
		Message:   message,
		Data:      data,
	}
	l.LogAsync(event)
}

// LogEventSync is a convenience method for sync logging (blocking)
func (l *Logger) LogEventSync(
	eventType EventType,
	level EventLevel,
	category EventCategory,
	source, user, message string,
	data map[string]interface{},
) error {
	event := &Event{
		EventID:   generateEventID(),
		Timestamp: time.Now().Format(time.RFC3339),
		Type:      eventType,
		Level:     level,
		Category:  category,
		Source:    source,
		User:      user,
		Message:   message,
		Data:      data,
	}
	return l.Log(event)
}

// Close closes the logger and flushes remaining events
func (l *Logger) Close() error {
	if !l.closed.CompareAndSwap(false, true) {
		return nil
	}
	l.cancel()
	l.wg.Wait()
	return nil
}

// asyncProcessor processes async events in background
func (l *Logger) asyncProcessor() {
	defer l.wg.Done()

	const maxBatchSize = 1000 // hard cap to prevent unbounded growth on persistent errors
	batch := make([]*model.EventLog, 0, l.batchSize)
	ticker := time.NewTicker(l.flushInterval)
	defer ticker.Stop()

	flush := func() {
		if len(batch) == 0 {
			return
		}
		// Always clear the batch after flush attempt — losing events is
		// preferable to unbounded memory growth on persistent DB errors.
		_ = l.db.CreateInBatches(batch, 100).Error
		batch = batch[:0]
	}

	for {
		select {
		case <-l.ctx.Done():
			// Drain remaining events from channel before exiting
		drain:
			for {
				select {
				case event := <-l.asyncChan:
					batch = append(batch, l.toModel(event))
				default:
					break drain
				}
			}
			l.mu.Lock()
			flush()
			l.mu.Unlock()
			return

		case event := <-l.asyncChan:
			eventLog := l.toModel(event)
			batch = append(batch, eventLog)

			if len(batch) >= l.batchSize || len(batch) >= maxBatchSize {
				l.mu.Lock()
				flush()
				l.mu.Unlock()
			}

		case <-ticker.C:
			l.mu.Lock()
			flush()
			l.mu.Unlock()
		}
	}
}

// logInternal logs an event to database (must be called with lock held)
func (l *Logger) logInternal(event *Event) error {
	if event.EventID == "" {
		event.EventID = generateEventID()
	}

	var timestamp time.Time
	if event.Timestamp != "" {
		timestamp, _ = time.Parse(time.RFC3339, event.Timestamp)
	}
	if timestamp.IsZero() {
		timestamp = time.Now()
	}

	eventLog := l.toModel(event)
	eventLog.Timestamp = timestamp
	return l.db.Create(eventLog).Error
}

// toModel converts Event to EventLog model
func (l *Logger) toModel(event *Event) *model.EventLog {
	var dataJSON string
	if event.Data != nil && len(event.Data) > 0 {
		dataBytes, _ := json.Marshal(event.Data)
		if dataBytes != nil {
			dataJSON = string(dataBytes)
		}
	}

	// Parse timestamp from event, use current time if not set or invalid
	var timestamp time.Time
	if event.Timestamp != "" {
		timestamp, _ = time.Parse(time.RFC3339, event.Timestamp)
	}
	if timestamp.IsZero() {
		timestamp = time.Now()
	}

	// Format readable messages from event code + params.
	// If the event type is a known template code, generate both English and Chinese
	// messages so that the search (LIKE) can match against human-readable text.
	// If not a known code (e.g. custom message), keep the original text as-is.
	messageEn := event.Message
	messageZh := ""
	code := string(event.Type)
	if tmpl := GetTemplate(code); tmpl != nil {
		messageEn = GetLocalizer().Localize(code, event.Data, "en")
		messageZh = GetLocalizer().Localize(code, event.Data, "zh_CN")
	}

	return &model.EventLog{
		EventID:   event.EventID,
		Timestamp: timestamp,
		EventType: code,
		Level:     string(event.Level),
		Category:  string(event.Category),
		Source:    event.Source,
		User:      event.User,
		Message:   messageEn,
		MessageZh: messageZh,
		Data:      dataJSON,
		CreatedAt: time.Now(),
	}
}

// CleanupOldEvents removes events older than specified days
func (l *Logger) CleanupOldEvents(days int) error {
	l.mu.Lock()
	defer l.mu.Unlock()

	cutoff := time.Now().AddDate(0, 0, -days)
	return l.db.Table("event_logs").Where("created_at < ?", cutoff).Delete(nil).Error
}

// generateEventID generates a unique event ID
func generateEventID() string {
	b := make([]byte, 8)
	rand.Read(b)
	return fmt.Sprintf("evt-%s-%s", time.Now().Format("20060102150405"), hex.EncodeToString(b))
}
