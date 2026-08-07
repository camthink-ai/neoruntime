package cmd

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/spf13/cobra"
	"github.com/spf13/viper"
)

var (
	buildVersion = "dev"
	cfgFile      string
	outputFmt   string
	ifaceName    string
	verbose      bool
)

var rootCmd = &cobra.Command{
	Use:   "ct-disc",
	Short: "CamThink Device Discovery Tool",
	Long: `ct-disc is a cross-platform CLI tool for CamThink device discovery.
It communicates with devices via UDP multicast (CT-Disc protocol) and MQTT.

Supports Windows, Linux, and macOS.`,
	Version: buildVersion,
}

func Execute() {
	if err := rootCmd.Execute(); err != nil {
		os.Exit(1)
	}
}

func SetVersion(v string) {
	buildVersion = v
	rootCmd.Version = v
}

func init() {
	cobra.OnInitialize(initConfig)

	rootCmd.PersistentFlags().StringVar(&cfgFile, "config", "", "config file (default ~/.ct-disc/config.yaml)")
	rootCmd.PersistentFlags().StringVarP(&outputFmt, "output", "o", "table", "output format: table, json, yaml")
	rootCmd.PersistentFlags().StringVar(&ifaceName, "iface", "", "network interface to use (auto-detect if empty)")
	rootCmd.PersistentFlags().BoolVarP(&verbose, "verbose", "v", false, "verbose output")

	viper.BindPFlag("output", rootCmd.PersistentFlags().Lookup("output"))
	viper.BindPFlag("iface", rootCmd.PersistentFlags().Lookup("iface"))
	viper.BindPFlag("verbose", rootCmd.PersistentFlags().Lookup("verbose"))
}

func initConfig() {
	if cfgFile != "" {
		viper.SetConfigFile(cfgFile)
	} else {
		home, err := os.UserHomeDir()
		if err != nil {
			return
		}
		configDir := filepath.Join(home, ".ct-disc")
		os.MkdirAll(configDir, 0755)
		viper.SetConfigFile(filepath.Join(configDir, "config.yaml"))
	}

	viper.SetEnvPrefix("CT_DISC")
	viper.AutomaticEnv()

	viper.SetDefault("multicast.addr", "239.255.255.250")
	viper.SetDefault("multicast.port", 19850)
	viper.SetDefault("timeout", "5s")
	viper.SetDefault("output", "table")

	if err := viper.ReadInConfig(); err == nil && verbose {
		fmt.Fprintln(os.Stderr, "Using config:", viper.ConfigFileUsed())
	}
}

func getTimeout(defaultSec int) time.Duration {
	ts := viper.GetString("timeout")
	if d, err := time.ParseDuration(ts); err == nil {
		return d
	}
	return time.Duration(defaultSec) * time.Second
}
