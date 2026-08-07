package cmd

import (
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/camthink/ct-disc/pkg/discover"
	"github.com/camthink/ct-disc/pkg/output"
	"github.com/spf13/cobra"
)

var (
	scanTimeout int
	scanCount   int
)

var scanCmd = &cobra.Command{
	Use:   "scan",
	Short: "Active scan for devices",
	Long:  "Send probe packets to trigger device announcements, then collect responses.",
	RunE:  runScan,
}

func init() {
	scanCmd.Flags().IntVar(&scanTimeout, "timeout", 3, "scan duration in seconds")
	scanCmd.Flags().IntVar(&scanCount, "count", 3, "number of probe packets to send")
	rootCmd.AddCommand(scanCmd)
}

func runScan(cmd *cobra.Command, args []string) error {
	printer := output.NewPrinter(outputFmt)

	registry := discover.NewRegistry()
	listener, err := discover.NewListener(registry, ifaceName)
	if err != nil {
		return fmt.Errorf("failed to start listener: %w", err)
	}
	defer listener.Close()

	go listener.Listen()

	// Send probe packets
	for i := 0; i < scanCount; i++ {
		if err := discover.SendProbe(ifaceName); err != nil {
			if verbose {
				fmt.Fprintf(os.Stderr, "probe send error: %v\n", err)
			}
		} else if verbose {
			fmt.Fprintf(os.Stderr, "Probe %d/%d sent\n", i+1, scanCount)
		}
		time.Sleep(200 * time.Millisecond)
	}

	timeout := time.Duration(scanTimeout) * time.Second
	fmt.Fprintf(os.Stderr, "Waiting %s for responses...\n", timeout)
	time.Sleep(timeout)

	devices := registry.List()

	if len(devices) == 0 {
		fmt.Fprintln(os.Stderr, "No devices responded.")
		return nil
	}

	switch outputFmt {
	case "json", "yaml":
		return printer.Print(devices)
	default:
		tbl := output.NewTable("MAC", "SN", "PRODUCT", "IP", "PORT", "FW", "CAPS", "LAST_SEEN")
		for _, d := range devices {
			tbl.AddRow(
				d.Announce.MAC,
				d.Announce.SN,
				d.Announce.Product,
				d.Announce.IP,
				fmt.Sprintf("%d", d.Announce.Port),
				d.Announce.FW,
				strings.Join(d.Announce.Caps, ","),
				output.FormatTimestamp(d.LastSeen),
			)
		}
		tbl.Render(os.Stdout)
		fmt.Fprintf(os.Stderr, "\n%d device(s) found.\n", len(devices))
	}

	return nil
}
