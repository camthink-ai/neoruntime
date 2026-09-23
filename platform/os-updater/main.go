package main

import (
	"flag"
	"fmt"
	"os"

	"aipc/platform/osupgrade"
	"golang.org/x/sys/unix"
)

func main() {
	root := flag.String("root", envOr("AIPC_OS_UPGRADE_DIR", osupgrade.DefaultRoot), "upgrade state directory")
	recoveryDir := flag.String("recovery-dir", envOr("AIPC_RECOVERY_DIR", osupgrade.DefaultRecoveryDir), "bundled Recovery directory")
	machine := flag.String("machine", envOr("AIPC_OS_MACHINE", "hailo15-ne503"), "device machine")
	flag.Parse()
	if flag.NArg() < 1 {
		fmt.Fprintln(os.Stderr, "usage: aipc-os-updater [OPTIONS] install|reboot|needs-verify|verify|check-recovery|exchange-dirs OLD NEW")
		os.Exit(2)
	}
	runner := osupgrade.NewRunner(osupgrade.NewStore(*root))
	var err error
	switch flag.Arg(0) {
	case "install":
		err = runner.Install()
	case "reboot":
		err = runner.Reboot()
	case "needs-verify":
		needed, checkErr := runner.NeedsVerification()
		if checkErr != nil {
			fmt.Fprintln(os.Stderr, checkErr)
			// ExecCondition treats 255 as a real unit failure. Other non-zero
			// values skip the service without marking it failed.
			os.Exit(255)
		}
		if !needed {
			os.Exit(1)
		}
	case "verify":
		err = runner.Verify()
	case "check-recovery":
		_, err = osupgrade.LoadRecoveryBundle(*recoveryDir, *machine)
	case "exchange-dirs":
		if flag.NArg() != 3 {
			err = fmt.Errorf("exchange-dirs requires OLD and NEW directory paths")
		} else {
			err = unix.Renameat2(unix.AT_FDCWD, flag.Arg(1), unix.AT_FDCWD, flag.Arg(2), unix.RENAME_EXCHANGE)
		}
	default:
		err = fmt.Errorf("unknown command %q", flag.Arg(0))
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func envOr(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}
