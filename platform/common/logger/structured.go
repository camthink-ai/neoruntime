package logger

import (
	"encoding/json"
	"fmt"
	"time"
)

// Fields represents structured log fields
type Fields map[string]interface{}

// StructuredLogger provides structured logging capabilities
type StructuredLogger struct {
	*Logger
	fields Fields
}

// WithFields creates a new logger with additional fields
func (l *Logger) WithFields(fields Fields) *StructuredLogger {
	// Merge fields
	merged := make(Fields)
	for k, v := range fields {
		merged[k] = v
	}

	return &StructuredLogger{
		Logger: l,
		fields: merged,
	}
}

// WithField adds a single field to the logger
func (l *Logger) WithField(key string, value interface{}) *StructuredLogger {
	return l.WithFields(Fields{key: value})
}

// logStructured logs a message with structured fields
func (sl *StructuredLogger) logStructured(level Level, format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)

	// Build structured log entry
	entry := map[string]interface{}{
		"timestamp": time.Now().Format(time.RFC3339),
		"level":     levelToString(level),
		"message":   msg,
	}

	// Add fields
	for k, v := range sl.fields {
		entry[k] = v
	}

	// Serialize to JSON if structured logging is enabled
	// For now, format as key=value pairs for readability
	fieldsStr := ""
	for k, v := range sl.fields {
		if fieldsStr != "" {
			fieldsStr += " "
		}
		fieldsStr += fmt.Sprintf("%s=%v", k, v)
	}

	if fieldsStr != "" {
		msg = fmt.Sprintf("%s [%s]", msg, fieldsStr)
	}

	// Use base logger
	sl.Logger.log(level, "%s", msg)
}

func levelToString(level Level) string {
	switch level {
	case DEBUG:
		return "DEBUG"
	case INFO:
		return "INFO"
	case WARN:
		return "WARN"
	case ERROR:
		return "ERROR"
	case FATAL:
		return "FATAL"
	default:
		return "UNKNOWN"
	}
}

// Debug logs debug message with fields
func (sl *StructuredLogger) Debug(format string, args ...interface{}) {
	sl.logStructured(DEBUG, format, args...)
}

// Info logs info message with fields
func (sl *StructuredLogger) Info(format string, args ...interface{}) {
	sl.logStructured(INFO, format, args...)
}

// Warn logs warning message with fields
func (sl *StructuredLogger) Warn(format string, args ...interface{}) {
	sl.logStructured(WARN, format, args...)
}

// Error logs error message with fields
func (sl *StructuredLogger) Error(format string, args ...interface{}) {
	sl.logStructured(ERROR, format, args...)
}

// Fatal logs fatal message with fields
func (sl *StructuredLogger) Fatal(format string, args ...interface{}) {
	sl.logStructured(FATAL, format, args...)
}

// JSONLog formats log entry as JSON
func (sl *StructuredLogger) JSONLog(level Level, format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)

	entry := map[string]interface{}{
		"timestamp": time.Now().UnixNano(),
		"level":     levelToString(level),
		"message":   msg,
	}

	for k, v := range sl.fields {
		entry[k] = v
	}

	jsonData, err := json.Marshal(entry)
	if err != nil {
		sl.Logger.log(ERROR, "Failed to marshal JSON log: %v", err)
		return
	}

	sl.Logger.log(level, "%s", string(jsonData))
}
