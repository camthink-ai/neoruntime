package cmd

import (
	"encoding/json"
	"fmt"
	"os"
	"text/tabwriter"

	"github.com/spf13/cobra"
)

const pluginDiscoveryPath = "/run/aipc/plugins/discovery.json"

// pluginCmd is the parent for plugin subcommands
var pluginCmd = &cobra.Command{
	Use:   "plugin",
	Short: "Manage platform plugins",
	Long:  "List, inspect and check plugin capabilities and dependencies.",
}

// plugin list
var pluginListCmd = &cobra.Command{
	Use:   "list",
	Short: "List registered plugins",
	RunE: func(cmd *cobra.Command, args []string) error {
		data, err := loadDiscovery()
		if err != nil {
			return err
		}

		plugins, ok := data["plugins"].(map[string]interface{})
		if !ok || len(plugins) == 0 {
			fmt.Println("No plugins registered.")
			return nil
		}

		w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
		fmt.Fprintln(w, "APP_ID\tVERSION\tSTATE\tCAPABILITIES")
		for _, p := range plugins {
			pm, _ := p.(map[string]interface{})
			appID, _ := pm["app_id"].(string)
			version, _ := pm["version"].(string)
			state, _ := pm["state"].(string)
			caps := capabilityIDs(pm)
			fmt.Fprintf(w, "%s\t%s\t%s\t%s\n", appID, version, state, caps)
		}
		w.Flush()
		return nil
	},
}

// plugin info <app-id>
var pluginInfoCmd = &cobra.Command{
	Use:   "info <app-id>",
	Short: "Show detailed plugin information",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		data, err := loadDiscovery()
		if err != nil {
			return err
		}

		plugins, _ := data["plugins"].(map[string]interface{})
		p, ok := plugins[args[0]]
		if !ok {
			return fmt.Errorf("plugin %q not found", args[0])
		}

		out, _ := json.MarshalIndent(p, "", "  ")
		fmt.Println(string(out))
		return nil
	},
}

// plugin capabilities
var pluginCapabilitiesCmd = &cobra.Command{
	Use:   "capabilities",
	Short: "List all registered capabilities",
	RunE: func(cmd *cobra.Command, args []string) error {
		data, err := loadDiscovery()
		if err != nil {
			return err
		}

		plugins, _ := data["plugins"].(map[string]interface{})
		if len(plugins) == 0 {
			fmt.Println("No capabilities registered.")
			return nil
		}

		w := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
		fmt.Fprintln(w, "CAPABILITY\tVERSION\tTRANSPORT\tPROVIDER\tSTATE")
		for _, p := range plugins {
			pm, _ := p.(map[string]interface{})
			appID, _ := pm["app_id"].(string)
			state, _ := pm["state"].(string)
			caps, _ := pm["capabilities"].([]interface{})
			for _, c := range caps {
				cm, _ := c.(map[string]interface{})
				capID, _ := cm["id"].(string)
				ver, _ := cm["version"].(string)
				transport, _ := cm["transport"].(string)
				fmt.Fprintf(w, "%s\t%s\t%s\t%s\t%s\n", capID, ver, transport, appID, state)
			}
		}
		w.Flush()
		return nil
	},
}

// plugin check <app-id>
var pluginCheckCmd = &cobra.Command{
	Use:   "check <app-id>",
	Short: "Check if a plugin's dependencies are satisfied",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		// This would normally query app-manager via gRPC.
		// For now, read discovery.json and check locally.
		data, err := loadDiscovery()
		if err != nil {
			return err
		}

		plugins, _ := data["plugins"].(map[string]interface{})
		_, ok := plugins[args[0]]
		if ok {
			fmt.Printf("Plugin %s is registered and has discovery entry.\n", args[0])
		} else {
			fmt.Printf("Plugin %s NOT found in discovery.\n", args[0])
		}
		return nil
	},
}

func init() {
	pluginCmd.AddCommand(pluginListCmd)
	pluginCmd.AddCommand(pluginInfoCmd)
	pluginCmd.AddCommand(pluginCapabilitiesCmd)
	pluginCmd.AddCommand(pluginCheckCmd)
	rootCmd.AddCommand(pluginCmd)
}

// helpers

func loadDiscovery() (map[string]interface{}, error) {
	raw, err := os.ReadFile(pluginDiscoveryPath)
	if err != nil {
		if os.IsNotExist(err) {
			return map[string]interface{}{"plugins": map[string]interface{}{}}, nil
		}
		return nil, fmt.Errorf("failed to read discovery file: %w", err)
	}

	var data map[string]interface{}
	if err := json.Unmarshal(raw, &data); err != nil {
		return nil, fmt.Errorf("failed to parse discovery file: %w", err)
	}
	return data, nil
}

func capabilityIDs(pm map[string]interface{}) string {
	caps, _ := pm["capabilities"].([]interface{})
	var ids []string
	for _, c := range caps {
		cm, _ := c.(map[string]interface{})
		if id, ok := cm["id"].(string); ok {
			ids = append(ids, id)
		}
	}
	if len(ids) == 0 {
		return "-"
	}
	result := ids[0]
	for i := 1; i < len(ids); i++ {
		result += ", " + ids[i]
	}
	return result
}
