package cmd

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/camthink/ct-disc/pkg/discover"
	"github.com/spf13/cobra"
)

var (
	annProduct string
	annSN      string
	annIP      string
	annPort    int
	annFW      string
	annCaps    string
	annHW      string
	annInterval int
)

var announceCmd = &cobra.Command{
	Use:   "announce",
	Short: "Broadcast device announcement (run on device)",
	Long:  "Periodically send ct-announce multicast packets so other hosts can discover this device.",
	RunE:  runAnnounce,
}

func init() {
	announceCmd.Flags().StringVar(&annProduct, "product", "NE503", "product name")
	announceCmd.Flags().StringVar(&annSN, "sn", "", "serial number (auto-generate if empty)")
	announceCmd.Flags().StringVar(&annIP, "ip", "", "IP address (auto-detect if empty)")
	announceCmd.Flags().IntVar(&annPort, "port", 8080, "HTTP port")
	announceCmd.Flags().StringVar(&annFW, "fw", "v0.1.0", "firmware version")
	announceCmd.Flags().StringVar(&annCaps, "caps", "ai,camera,http,mqtt", "capabilities (comma-separated)")
	announceCmd.Flags().StringVar(&annHW, "hw", "", "hardware platform (auto-detect if empty)")
	announceCmd.Flags().IntVar(&annInterval, "interval", 5, "announce interval in seconds")
	rootCmd.AddCommand(announceCmd)
}

func runAnnounce(cmd *cobra.Command, args []string) error {
	sn := annSN
	if sn == "" {
		hostname, _ := os.Hostname()
		sn = hostname
	}

	a := discover.Announce{
		Product: annProduct,
		SN:      sn,
		IP:      annIP,
		Port:    annPort,
		FW:      annFW,
		Caps:    strings.Split(annCaps, ","),
		HW:      annHW,
	}

	announcer := discover.NewAnnouncer(a, ifaceName, time.Duration(annInterval)*time.Second)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if err := announcer.Start(ctx); err != nil {
		return fmt.Errorf("failed to start announcer: %w", err)
	}
	defer announcer.Stop()

	fmt.Fprintf(os.Stderr, "Announcing %s (%s) every %ds on %s:%d...\nPress Ctrl+C to stop.\n",
		sn, annProduct, annInterval, discover.MulticastAddr, discover.MulticastPort)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	<-sigCh

	fmt.Fprintln(os.Stderr, "\nStopped.")
	return nil
}
