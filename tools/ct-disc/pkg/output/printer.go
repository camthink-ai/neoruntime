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

type Printer struct {
	Format string
	Color  bool
	Writer io.Writer
}

func NewPrinter(format string) *Printer {
	return &Printer{
		Format: format,
		Color:  true,
		Writer: os.Stdout,
	}
}

func (p *Printer) Print(data interface{}) error {
	switch p.Format {
	case "json":
		return p.printJSON(data)
	case "yaml":
		return p.printYAML(data)
	default:
		return p.printTable(data)
	}
}

func (p *Printer) printJSON(data interface{}) error {
	out, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		return err
	}
	fmt.Fprintln(p.Writer, string(out))
	return nil
}

func (p *Printer) printYAML(data interface{}) error {
	out, err := yaml.Marshal(data)
	if err != nil {
		return err
	}
	fmt.Fprint(p.Writer, string(out))
	return nil
}

func (p *Printer) printTable(data interface{}) error {
	out, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		return err
	}
	fmt.Fprintln(p.Writer, string(out))
	return nil
}

type Table struct {
	headers []string
	rows    [][]string
}

func NewTable(headers ...string) *Table {
	return &Table{headers: headers}
}

func (t *Table) AddRow(values ...string) {
	t.rows = append(t.rows, values)
}

func (t *Table) Render(w io.Writer) {
	tw := tabwriter.NewWriter(w, 0, 0, 2, ' ', 0)
	fmt.Fprintln(tw, strings.Join(t.headers, "\t"))
	seps := make([]string, len(t.headers))
	for i, h := range t.headers {
		seps[i] = strings.Repeat("-", len(h))
	}
	fmt.Fprintln(tw, strings.Join(seps, "\t"))
	for _, row := range t.rows {
		fmt.Fprintln(tw, strings.Join(row, "\t"))
	}
	tw.Flush()
}

const (
	colorReset  = "\033[0m"
	colorGreen  = "\033[32m"
	colorRed    = "\033[31m"
	colorYellow = "\033[33m"
	colorCyan   = "\033[36m"
)

func Colorize(color, text string) string {
	return color + text + colorReset
}

func FormatTimestamp(t time.Time) string {
	if t.IsZero() {
		return "-"
	}
	return t.Format("2006-01-02 15:04:05")
}

func FormatStatus(status string, color bool) string {
	if !color {
		return status
	}
	switch strings.ToLower(status) {
	case "online", "running", "active":
		return colorGreen + status + colorReset
	case "offline", "stopped":
		return colorYellow + status + colorReset
	case "error", "failed":
		return colorRed + status + colorReset
	default:
		return status
	}
}

func Success(msg string) {
	fmt.Fprintf(os.Stdout, "%s✓ %s%s\n", colorGreen, msg, colorReset)
}

func Error(msg string) {
	fmt.Fprintf(os.Stderr, "%s✗ %s%s\n", colorRed, msg, colorReset)
}

func Info(msg string) {
	fmt.Fprintf(os.Stdout, "%sℹ %s%s\n", colorCyan, msg, colorReset)
}
