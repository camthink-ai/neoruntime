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
	listTimeout int
	listProduct string
	listSN      string
	listMAC     string
)

var listCmd = &cobra.Command{
	Use:   "list",
	Short: "List discovered devices",
	Long:  "Listen for CT-Disc multicast announcements and list discovered devices.",
	RunE:  runList,
}

func init() {
	listCmd.Flags().IntVar(&listTimeout, "timeout", 5, "listen duration in seconds")
	listCmd.Flags().StringVar(&listProduct, "product", "", "filter by product name (e.g. NE503)")
	listCmd.Flags().StringVar(&listSN, "sn", "", "filter by serial number (substring match)")
	listCmd.Flags().StringVar(&listMAC, "mac", "", "filter by MAC address (substring match)")
	rootCmd.AddCommand(listCmd)
}

func runList(cmd *cobra.Command, args []string) error {
	printer := output.NewPrinter(outputFmt)

	timeout := time.Duration(listTimeout) * time.Second
	if verbose {
		fmt.Fprintf(os.Stderr, "Listening for %s on %s:%d...\n", timeout, discover.MulticastAddr, discover.MulticastPort)
	}

	registry := discover.NewRegistry()
	listener, err := discover.NewListener(registry, ifaceName)
	if err != nil {
		return fmt.Errorf("failed to start listener: %w", err)
	}
	defer listener.Close()

	go listener.Listen()

	time.Sleep(timeout)

	devices := registry.Filter(listProduct, listSN, listMAC)

	if len(devices) == 0 {
		fmt.Fprintln(os.Stderr, "No devices discovered.")
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
