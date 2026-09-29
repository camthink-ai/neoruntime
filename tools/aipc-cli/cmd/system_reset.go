package cmd

import (
	"fmt"
	"os"
	"os/exec"

	"github.com/spf13/cobra"
)

const defaultFactoryResetScript = "/data/aipc/scripts/aipc-factory-reset.sh"

var (
	factoryResetYes    bool
	factoryResetDryRun bool
	factoryResetScript string
)

var systemFactoryResetCmd = &cobra.Command{
	Use:   "factory-reset",
	Short: "Restore default configuration (production-line reset)",
	Long: `Restore the device to default configuration.

Runs /data/aipc/scripts/aipc-factory-reset.sh which clears, on a strict
whitelist:
  - platform DB (configured apps, settings, web login)  -> re-seeded on start
  - app instances and registry state
  - persisted events and media backup
  - commissioned network configuration                  -> default 10.0.0.1/24

NOT touched: installed binaries/web/models, logs, and factory identity
(SN/MAC live in U-Boot env and MCU EEPROM, outside /data/aipc).

After the reset the device answers at 10.0.0.1 — if your terminal is
connected at the commissioned IP, reconnect there.`,
	RunE: func(cmd *cobra.Command, args []string) error {
		if os.Geteuid() != 0 {
			return fmt.Errorf("factory-reset must run as root (try: sudo aipc-cli system factory-reset)")
		}
		if _, err := os.Stat(factoryResetScript); err != nil {
			return fmt.Errorf("reset script not found at %s (deploy the aipc package or pass --script): %w", factoryResetScript, err)
		}

		scriptArgs := []string{factoryResetScript}
		if factoryResetYes {
			scriptArgs = append(scriptArgs, "--yes")
		}
		if factoryResetDryRun {
			scriptArgs = append(scriptArgs, "--dry-run")
		}

		// The script owns the interactive confirmation; without --yes it
		// prompts before changing anything, so stdin is wired through.
		execCmd := exec.Command("bash", scriptArgs...)
		execCmd.Stdin = os.Stdin
		execCmd.Stdout = os.Stdout
		execCmd.Stderr = os.Stderr
		if err := execCmd.Run(); err != nil {
			if exitErr, ok := err.(*exec.ExitError); ok {
				os.Exit(exitErr.ExitCode())
			}
			return fmt.Errorf("run %s: %w", factoryResetScript, err)
		}
		return nil
	},
}

func init() {
	systemFactoryResetCmd.Flags().BoolVar(&factoryResetYes, "yes", false,
		"skip the script's interactive confirmation")
	systemFactoryResetCmd.Flags().BoolVar(&factoryResetDryRun, "dry-run", false,
		"print planned actions, change nothing")
	systemFactoryResetCmd.Flags().StringVar(&factoryResetScript, "script", defaultFactoryResetScript,
		"path to aipc-factory-reset.sh (override for testing)")

	systemCmd.AddCommand(systemFactoryResetCmd)
}
