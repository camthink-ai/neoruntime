package logger

import (
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"aipc/platform/common/constants"
)

// Level represents log level
type Level int

const (
	DEBUG Level = iota
	INFO
	WARN
	ERROR
	FATAL
)

// Logger is the main logger struct
type Logger struct {
	level  Level
	logger *log.Logger
	mu     sync.Mutex
}

var defaultLogger *Logger

func init() {
	defaultLogger = New(INFO, os.Stdout)
}

// New creates a new logger
func New(level Level, out io.Writer) *Logger {
	return &Logger{
		level:  level,
		logger: log.New(out, "", log.LstdFlags|log.Lshortfile),
	}
}

// SetLevel sets the log level
func SetLevel(level Level) {
	defaultLogger.level = level
}

// SetLevelFromString sets level from string
func SetLevelFromString(levelStr string) {
	switch strings.ToUpper(levelStr) {
	case "DEBUG":
		SetLevel(DEBUG)
	case "INFO":
		SetLevel(INFO)
	case "WARN", "WARNING":
		SetLevel(WARN)
	case "ERROR":
		SetLevel(ERROR)
	case "FATAL":
		SetLevel(FATAL)
	default:
		SetLevel(INFO)
	}
}

// SetOutput sets the logger output writer
func SetOutput(w io.Writer) {
	defaultLogger.mu.Lock()
	defer defaultLogger.mu.Unlock()
	defaultLogger.logger.SetOutput(w)
}

// multiWriter wraps multiple writers for tee output
type multiWriter struct {
	writers []io.Writer
}

func (mw *multiWriter) Write(p []byte) (n int, err error) {
	for _, w := range mw.writers {
		n, err = w.Write(p)
		if err != nil {
			return
		}
	}
	return len(p), nil
}

// SetOutputFile configures logging to both stdout and a file.
// If path is empty, only stdout is used.
// The directory of the file will be created if it doesn't exist.
//
// Path remapping: if path starts with "/var/log/aipc/", the log file
// is redirected to constants.LogPath()/ (e.g. /data/logs/ on devices
// where the install prefix is /data). This ensures logs survive reboots
// on embedded devices where /var/log lives on tmpfs.
func SetOutputFile(path string) error {
	if path == "" {
		return nil
	}

	// Remap /var/log/aipc/<name> → <prefix>/logs/<name>
	// On embedded devices /var/log is tmpfs (lost on reboot), so redirect
	// to the persistent install prefix.
	//
	// Target directory resolution (in priority order):
	//  1. If the caller explicitly overrode the install prefix via
	//     constants.SetRootPath (platform-api, app-manager, ai-runtime),
	//     honor LogPath() verbatim — they know where they want logs.
	//  2. Otherwise (rootPath is still the default /opt/aipc — e.g. event-bus,
	//     device-control, which don't call SetRootPath) prefer /data/logs when
	//     it exists: on devices /opt/aipc lives on the small root partition
	//     while /data is the large persistent volume. Services' ExecStartPre
	//     create /opt/aipc/logs, so probing only when LogPath() is absent would
	//     pin logs to root — this prefer-/data rule makes the fallback robust.
	//  3. Fall back to LogPath() (default /opt/aipc/logs) on dev installs with
	//     no /data volume.
	if strings.HasPrefix(path, "/var/log/aipc/") {
		fileName := filepath.Base(path)
		targetDir := constants.LogPath()
		if targetDir == constants.DefaultLogPath {
			// Caller did not override the prefix — prefer the persistent volume.
			if fi, err := os.Stat("/data/logs"); err == nil && fi.IsDir() {
				targetDir = "/data/logs"
			} else if _, err := os.Stat(targetDir); os.IsNotExist(err) {
				for _, candidate := range []string{"/data/logs", "/opt/aipc/logs"} {
					if fi, err2 := os.Stat(candidate); err2 == nil && fi.IsDir() {
						targetDir = candidate
						break
					}
				}
			}
		}
		path = filepath.Join(targetDir, fileName)
	}

	// Ensure directory exists
	dir := filepath.Dir(path)
	if dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0755); err != nil {
			return fmt.Errorf("failed to create log directory: %w", err)
		}
	}

	// Open log file (append mode, create if not exists)
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return fmt.Errorf("failed to open log file: %w", err)
	}

	// Tee: write to both stdout and file
	mw := &multiWriter{writers: []io.Writer{os.Stdout, file}}
	SetOutput(mw)

	return nil
}

func (l *Logger) log(level Level, format string, args ...interface{}) {
	if level < l.level {
		return
	}

	prefix := ""
	switch level {
	case DEBUG:
		prefix = "[DEBUG] "
	case INFO:
		prefix = "[INFO]  "
	case WARN:
		prefix = "[WARN]  "
	case ERROR:
		prefix = "[ERROR] "
	case FATAL:
		prefix = "[FATAL] "
	}

	msg := fmt.Sprintf(format, args...)
	l.logger.Output(3, prefix+msg)

	if level == FATAL {
		os.Exit(1)
	}
}

// Debug logs debug message
func Debug(format string, args ...interface{}) {
	defaultLogger.log(DEBUG, format, args...)
}

// Info logs info message
func Info(format string, args ...interface{}) {
	defaultLogger.log(INFO, format, args...)
}

// Warn logs warning message
func Warn(format string, args ...interface{}) {
	defaultLogger.log(WARN, format, args...)
}

// Error logs error message
func Error(format string, args ...interface{}) {
	defaultLogger.log(ERROR, format, args...)
}

// Fatal logs fatal message and exits
func Fatal(format string, args ...interface{}) {
	defaultLogger.log(FATAL, format, args...)
}
