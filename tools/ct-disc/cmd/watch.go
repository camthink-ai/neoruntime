package cmd

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/camthink/ct-disc/pkg/discover"
	"github.com/camthink/ct-disc/pkg/output"
	"github.com/spf13/cobra"
)

var watchTimeout int

var watchCmd = &cobra.Command{
	Use:   "watch",
	Short: "Watch device events in real-time",
	Long:  "Continuously monitor CT-Disc multicast for device online/offline/update events.",
	RunE:  runWatch,
}

func init() {
	watchCmd.Flags().IntVar(&watchTimeout, "timeout", 0, "watch duration in seconds (0 = until Ctrl+C)")
	rootCmd.AddCommand(watchCmd)
}

func runWatch(cmd *cobra.Command, args []string) error {
	registry := discover.NewRegistry()
	listener, err := discover.NewListener(registry, ifaceName)
	if err != nil {
		return fmt.Errorf("failed to start listener: %w", err)
	}
	defer listener.Close()

	go listener.Listen()

	// Timeout checker
	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			expired := registry.CheckTimeouts(30 * time.Second)
			for _, key := range expired {
				if dev, ok := registry.GetByMAC(key); ok {
					fmt.Printf("[OFFLINE] %s (%s) [%s] timeout after 30s\n", key, dev.Announce.SN, dev.Announce.Product)
				}
			}
		}
	}()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	fmt.Fprintf(os.Stderr, "Watching for device events on %s:%d... (Ctrl+C to stop)\n",
		discover.MulticastAddr, discover.MulticastPort)

	timeout := time.Duration(watchTimeout) * time.Second
	var timer *time.Timer
	if timeout > 0 {
		timer = time.AfterFunc(timeout, cancel)
		defer timer.Stop()
	}

	for {
		select {
		case <-ctx.Done():
			return nil
		case <-sigCh:
			fmt.Fprintln(os.Stderr, "\nStopped.")
			return nil
		case evt := <-listener.Events:
			dev := evt.Device
			switch evt.Type {
			case "online":
				if outputFmt == "json" {
					output.NewPrinter("json").Print(map[string]interface{}{
						"event": "online", "mac": dev.Announce.MAC, "sn": dev.Announce.SN,
						"product": dev.Announce.Product, "ip": dev.Announce.IP, "port": dev.Announce.Port,
					})
				} else {
					output.Success(fmt.Sprintf("[ONLINE]  %s (%s) [%s] at %s:%d",
						dev.Announce.MAC, dev.Announce.SN, dev.Announce.Product, dev.Announce.IP, dev.Announce.Port))
				}
			case "update":
				if outputFmt == "json" {
					output.NewPrinter("json").Print(map[string]interface{}{
						"event": "update", "mac": dev.Announce.MAC, "sn": dev.Announce.SN,
						"product": dev.Announce.Product, "ip": dev.Announce.IP,
					})
				} else {
					fmt.Printf("[UPDATE]  %s (%s) [%s]\n", dev.Announce.MAC, dev.Announce.SN, dev.Announce.Product)
				}
			}
		}
	}
}
