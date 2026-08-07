/*
Package output provides output formatting for CLI.
*/
package output

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strings"
	"text/tabwriter"
	"time"

	"gopkg.in/yaml.v3"
)

// Printer handles output formatting
type Printer struct {
	format string
	color  bool
	writer io.Writer
}

// NewPrinter creates a new printer
func NewPrinter(format string, color bool) *Printer {
	return &Printer{
		format: format,
		color:  color,
		writer: os.Stdout,
	}
}

// SetWriter sets the output writer
func (p *Printer) SetWriter(w io.Writer) {
	p.writer = w
}

// Print outputs data in the configured format
func (p *Printer) Print(data interface{}) error {
	switch p.format {
	case "json":
		return p.printJSON(data)
	case "yaml":
		return p.printYAML(data)
	default:
		return p.printTable(data)
	}
}

// printJSON outputs data as JSON
func (p *Printer) printJSON(data interface{}) error {
	output, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		return fmt.Errorf("failed to marshal JSON: %w", err)
	}
	fmt.Fprintln(p.writer, string(output))
	return nil
}

// printYAML outputs data as YAML
func (p *Printer) printYAML(data interface{}) error {
	output, err := yaml.Marshal(data)
	if err != nil {
		return fmt.Errorf("failed to marshal YAML: %w", err)
	}
	fmt.Fprint(p.writer, string(output))
	return nil
}

// printTable outputs data as a table (default handling)
func (p *Printer) printTable(data interface{}) error {
	// For complex types, fall back to JSON
	output, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		return fmt.Errorf("failed to marshal: %w", err)
	}
	fmt.Fprintln(p.writer, string(output))
	return nil
}

// ============ Table Helpers ============

// Table represents a simple table for output
type Table struct {
	headers []string
	rows    [][]string
	writer  *tabwriter.Writer
}

// NewTable creates a new table
func NewTable(headers ...string) *Table {
	return &Table{
		headers: headers,
		rows:    make([][]string, 0),
	}
}

// AddRow adds a row to the table
func (t *Table) AddRow(values ...string) {
	t.rows = append(t.rows, values)
}

// Render outputs the table
func (t *Table) Render(w io.Writer) {
	tw := tabwriter.NewWriter(w, 0, 0, 2, ' ', 0)

	// Print headers
	fmt.Fprintln(tw, strings.Join(t.headers, "\t"))

	// Print separator
	separators := make([]string, len(t.headers))
	for i, h := range t.headers {
		separators[i] = strings.Repeat("-", len(h))
	}
	fmt.Fprintln(tw, strings.Join(separators, "\t"))

	// Print rows
	for _, row := range t.rows {
		fmt.Fprintln(tw, strings.Join(row, "\t"))
	}

	tw.Flush()
}

// RenderTo renders the table to stdout
func (t *Table) RenderTo(p *Printer) {
	t.Render(p.writer)
}

// ============ Formatting Helpers ============

// FormatBytes formats bytes to human readable string
func FormatBytes(bytes int64) string {
	const unit = 1024
	if bytes < unit {
		return fmt.Sprintf("%d B", bytes)
	}
	div, exp := int64(unit), 0
	for n := bytes / unit; n >= unit; n /= unit {
		div *= unit
		exp++
	}
	units := []string{"Ki", "Mi", "Gi", "Ti"}
	return fmt.Sprintf("%.1f %sB", float64(bytes)/float64(div), units[exp])
}

// FormatDuration formats duration to human readable string
func FormatDuration(seconds int64) string {
	d := time.Duration(seconds) * time.Second

	if d < time.Minute {
		return fmt.Sprintf("%ds", int(d.Seconds()))
	}
	if d < time.Hour {
		return fmt.Sprintf("%dm%ds", int(d.Minutes()), int(d.Seconds())%60)
	}
	if d < 24*time.Hour {
		return fmt.Sprintf("%dh%dm", int(d.Hours()), int(d.Minutes())%60)
	}
	days := int(d.Hours()) / 24
	hours := int(d.Hours()) % 24
	return fmt.Sprintf("%dd%dh", days, hours)
}

// FormatTimestamp formats Unix timestamp to human readable string
func FormatTimestamp(ts int64) string {
	if ts == 0 {
		return "-"
	}
	t := time.Unix(ts, 0)
	return t.Format("2006-01-02 15:04:05")
}

// FormatPercent formats a percentage value
func FormatPercent(value float64) string {
	return fmt.Sprintf("%.1f%%", value)
}

// ============ Color Helpers ============

const (
	colorReset  = "\033[0m"
	colorRed    = "\033[31m"
	colorGreen  = "\033[32m"
	colorYellow = "\033[33m"
	colorBlue   = "\033[34m"
	colorCyan   = "\033[36m"
)

// Colorize adds color to text if color is enabled
func (p *Printer) Colorize(color, text string) string {
	if !p.color {
		return text
	}
	return color + text + colorReset
}

// Success prints a success message
func (p *Printer) Success(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	if p.color {
		fmt.Fprintf(p.writer, "%s✓ %s%s\n", colorGreen, msg, colorReset)
	} else {
		fmt.Fprintf(p.writer, "✓ %s\n", msg)
	}
}

// Error prints an error message
func (p *Printer) Error(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	if p.color {
		fmt.Fprintf(p.writer, "%s✗ %s%s\n", colorRed, msg, colorReset)
	} else {
		fmt.Fprintf(p.writer, "✗ %s\n", msg)
	}
}

// Warning prints a warning message
func (p *Printer) Warning(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	if p.color {
		fmt.Fprintf(p.writer, "%s! %s%s\n", colorYellow, msg, colorReset)
	} else {
		fmt.Fprintf(p.writer, "! %s\n", msg)
	}
}

// Info prints an info message
func (p *Printer) Info(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	if p.color {
		fmt.Fprintf(p.writer, "%si %s%s\n", colorBlue, msg, colorReset)
	} else {
		fmt.Fprintf(p.writer, "i %s\n", msg)
	}
}

// Printf prints formatted output
func (p *Printer) Printf(format string, args ...interface{}) {
	fmt.Fprintf(p.writer, format, args...)
}

// Println prints a line
func (p *Printer) Println(args ...interface{}) {
	fmt.Fprintln(p.writer, args...)
}

// ============ Status Helpers ============

// FormatStatus returns a colored status string
func (p *Printer) FormatStatus(status string) string {
	if !p.color {
		return status
	}

	switch strings.ToLower(status) {
	case "running", "active", "healthy", "success", "online":
		return colorGreen + status + colorReset
	case "stopped", "inactive", "offline":
		return colorYellow + status + colorReset
	case "failed", "error", "unhealthy":
		return colorRed + status + colorReset
	default:
		return status
	}
}

// FormatBool returns a colored boolean string
func (p *Printer) FormatBool(value bool) string {
	if value {
		if p.color {
			return colorGreen + "Yes" + colorReset
		}
		return "Yes"
	}
	if p.color {
		return colorRed + "No" + colorReset
	}
	return "No"
}
