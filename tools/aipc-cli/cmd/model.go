package cmd

import (
	"context"
	"fmt"
	"os"
	"path/filepath"

	"github.com/spf13/cobra"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/tools/aipc-cli/pkg/output"
)

var modelCmd = &cobra.Command{
	Use:   "model",
	Short: "Manage AI models",
	Long:  `Manage AI models: list, info, register, unregister, stats.`,
}

// connectAIRuntime ensures we have a connection to ai-runtime
func connectAIRuntime() error {
	return grpcCli.ConnectAIRuntime()
}

// ============ model list ============

var modelListCmd = &cobra.Command{
	Use:   "list",
	Short: "List all registered models",
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAIRuntime(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.AIRuntime.ListModels(ctx, &inferencepb.Empty{})
		if err != nil {
			return fmt.Errorf("failed to list models: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoModelListToMap(result))
		}

		if len(result.Models) == 0 {
			printer.Info("No models registered")
			return nil
		}

		table := output.NewTable("ID", "PATH", "VERSION", "MEMORY", "LOAD TIME")
		for _, model := range result.Models {
			loadTime := "-"
			if model.LoadTimestamp > 0 {
				loadTime = output.FormatTimestamp(int64(model.LoadTimestamp))
			}
			table.AddRow(
				model.ModelId,
				model.ModelPath,
				model.Version,
				output.FormatBytes(int64(model.EstimatedMemory)),
				loadTime,
			)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ model info ============

var modelInfoCmd = &cobra.Command{
	Use:   "info <model-id>",
	Short: "Show model details",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAIRuntime(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		// Get model info directly
		model, err := grpcCli.AIRuntime.GetModelInfo(ctx, &inferencepb.ModelInfo{
			ModelId: args[0],
		})
		if err != nil {
			return fmt.Errorf("failed to get model info: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoModelInfoToMap(model))
		}

		printer.Printf("Model: %s\n", model.ModelId)
		printer.Printf("  Path:            %s\n", model.ModelPath)
		printer.Printf("  Version:         %s\n", model.Version)
		printer.Printf("  Estimated TOPS:  %.2f\n", model.EstimatedTops)
		printer.Printf("  Estimated Mem:   %s\n", output.FormatBytes(int64(model.EstimatedMemory)))
		if model.LoadTimestamp > 0 {
			printer.Printf("  Loaded At:       %s\n", output.FormatTimestamp(int64(model.LoadTimestamp)))
		}

		if len(model.Inputs) > 0 {
			printer.Printf("\n  Input Tensors:\n")
			for _, t := range model.Inputs {
				printer.Printf("    - %s: %v (%s)\n", t.Name, t.Shape, t.Dtype.String())
			}
		}

		if len(model.Outputs) > 0 {
			printer.Printf("\n  Output Tensors:\n")
			for _, t := range model.Outputs {
				printer.Printf("    - %s: %v (%s)\n", t.Name, t.Shape, t.Dtype.String())
			}
		}

		return nil
	},
}

// ============ model register ============

var (
	modelRegisterID string
)

var modelRegisterCmd = &cobra.Command{
	Use:   "register <model-path>",
	Short: "Register a new model",
	Long: `Register a new AI model from a file path.

Examples:
  # Register model with auto-generated ID
  aipc-cli model register /opt/aipc/models/yolov8n.hef

  # Register model with custom ID
  aipc-cli model register /opt/aipc/models/yolov8n.hef --id my-detector
`,
	Args: cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		modelPath := args[0]

		// Check file exists
		if _, err := os.Stat(modelPath); os.IsNotExist(err) {
			return fmt.Errorf("model file not found: %s", modelPath)
		}

		// Convert to absolute path
		absPath, _ := filepath.Abs(modelPath)

		if err := connectAIRuntime(); err != nil {
			return err
		}

		printer.Info("Registering model...")

		ctx, cancel := context.WithTimeout(context.Background(), 60*cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.AIRuntime.RegisterModel(ctx, &inferencepb.ModelRegisterRequest{
			ModelPath: absPath,
			ModelId:   modelRegisterID,
		})
		if err != nil {
			return fmt.Errorf("failed to register model: %w", err)
		}

		if result.Status != nil && !result.Status.Success {
			printer.Error("Register failed: %s", result.Status.Message)
			return fmt.Errorf("register failed")
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(map[string]interface{}{
				"model_id": result.ModelId,
				"success":  true,
			})
		}

		printer.Success("Model registered: %s", result.ModelId)
		return nil
	},
}

// ============ model unregister ============

var modelUnregisterCmd = &cobra.Command{
	Use:     "unregister <model-id>",
	Aliases: []string{"remove", "rm"},
	Short:   "Unregister a model",
	Args:    cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAIRuntime(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.AIRuntime.UnregisterModel(ctx, &inferencepb.ModelInfo{
			ModelId: args[0],
		})
		if err != nil {
			return fmt.Errorf("failed to unregister model: %w", err)
		}

		if !result.Success {
			printer.Error("Unregister failed: %s", result.Message)
			return fmt.Errorf("unregister failed")
		}

		printer.Success("Model unregistered: %s", args[0])
		return nil
	},
}

// ============ model stats ============

var modelStatsCmd = &cobra.Command{
	Use:   "stats",
	Short: "Show AI runtime statistics",
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAIRuntime(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		stats, err := grpcCli.AIRuntime.GetStats(ctx, &inferencepb.Empty{})
		if err != nil {
			return fmt.Errorf("failed to get stats: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoSystemStatsToMapAI(stats))
		}

		printer.Printf("AI Runtime Statistics\n")
		printer.Printf("  Device Utilization: %.1f%%\n", stats.DeviceUtilization*100)
		printer.Printf("  Device Temperature: %.1f°C\n", stats.DeviceTemperature)
		printer.Printf("  Memory Usage:       %s / %s\n",
			output.FormatBytes(int64(stats.UsedMemoryBytes)),
			output.FormatBytes(int64(stats.TotalMemoryBytes)))

		if len(stats.ModelStats) > 0 {
			printer.Printf("\nModel Statistics:\n")
			table := output.NewTable("MODEL", "INFERENCES", "ERRORS", "AVG LATENCY", "QPS", "QUEUE")
			for _, ms := range stats.ModelStats {
				table.AddRow(
					ms.ModelId,
					fmt.Sprintf("%d", ms.TotalInferences),
					fmt.Sprintf("%d", ms.TotalErrors),
					fmt.Sprintf("%.2f ms", float64(ms.AvgLatencyUs)/1000.0),
					fmt.Sprintf("%.1f", ms.CurrentQps),
					fmt.Sprintf("%d", ms.QueueDepth),
				)
			}
			table.RenderTo(printer)
		}
		return nil
	},
}

// ============ Proto to map conversion helpers ============

func protoModelListToMap(list *inferencepb.ModelListResponse) map[string]interface{} {
	models := make([]map[string]interface{}, len(list.Models))
	for i, m := range list.Models {
		models[i] = protoModelInfoToMap(m)
	}
	return map[string]interface{}{"models": models}
}

func protoModelInfoToMap(m *inferencepb.ModelInfo) map[string]interface{} {
	inputs := make([]map[string]interface{}, len(m.Inputs))
	for i, t := range m.Inputs {
		inputs[i] = map[string]interface{}{
			"name":  t.Name,
			"shape": t.Shape,
			"dtype": t.Dtype.String(),
		}
	}
	outputs := make([]map[string]interface{}, len(m.Outputs))
	for i, t := range m.Outputs {
		outputs[i] = map[string]interface{}{
			"name":  t.Name,
			"shape": t.Shape,
			"dtype": t.Dtype.String(),
		}
	}
	return map[string]interface{}{
		"model_id":         m.ModelId,
		"model_path":       m.ModelPath,
		"version":          m.Version,
		"estimated_tops":   m.EstimatedTops,
		"estimated_memory": m.EstimatedMemory,
		"load_timestamp":   m.LoadTimestamp,
		"inputs":           inputs,
		"outputs":          outputs,
	}
}

func protoSystemStatsToMapAI(s *inferencepb.SystemStats) map[string]interface{} {
	modelStats := make([]map[string]interface{}, len(s.ModelStats))
	for i, ms := range s.ModelStats {
		modelStats[i] = map[string]interface{}{
			"model_id":         ms.ModelId,
			"total_inferences": ms.TotalInferences,
			"total_errors":     ms.TotalErrors,
			"avg_latency_us":   ms.AvgLatencyUs,
			"current_qps":      ms.CurrentQps,
			"queue_depth":      ms.QueueDepth,
		}
	}
	return map[string]interface{}{
		"model_stats":        modelStats,
		"device_utilization": s.DeviceUtilization,
		"device_temperature": s.DeviceTemperature,
		"total_memory_bytes": s.TotalMemoryBytes,
		"used_memory_bytes":  s.UsedMemoryBytes,
	}
}

func init() {
	// model register flags
	modelRegisterCmd.Flags().StringVar(&modelRegisterID, "id", "", "Custom model ID (auto-generated if not set)")

	// Register subcommands
	modelCmd.AddCommand(modelListCmd)
	modelCmd.AddCommand(modelInfoCmd)
	modelCmd.AddCommand(modelRegisterCmd)
	modelCmd.AddCommand(modelUnregisterCmd)
	modelCmd.AddCommand(modelStatsCmd)
}
