package cmd

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/spf13/cobra"

	eventpb "aipc/platform/event-bus/proto"
	"aipc/tools/aipc-cli/pkg/output"
)

var eventCmd = &cobra.Command{
	Use:   "event",
	Short: "Manage event bus",
	Long:  `Manage event bus: publish, subscribe, list topics, view stats.`,
}

// connectEventBus ensures we have a connection to event-bus
func connectEventBus() error {
	return grpcCli.ConnectEventBus()
}

// ============ event topics ============

var eventTopicsCmd = &cobra.Command{
	Use:   "topics",
	Short: "List all topics",
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectEventBus(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.EventBus.ListTopics(ctx, &eventpb.Empty{})
		if err != nil {
			return fmt.Errorf("failed to list topics: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoTopicListToMap(result))
		}

		if len(result.Topics) == 0 {
			printer.Info("No topics found")
			return nil
		}

		table := output.NewTable("TOPIC", "SUBSCRIBERS", "MESSAGES", "LAST MESSAGE")
		for _, t := range result.Topics {
			lastMsg := "-"
			if t.LastMessageTs > 0 {
				lastMsg = output.FormatTimestamp(int64(t.LastMessageTs / 1000000000))
			}
			table.AddRow(
				t.Topic,
				fmt.Sprintf("%d", t.SubscriberCount),
				fmt.Sprintf("%d", t.TotalMessages),
				lastMsg,
			)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ event info ============

var eventInfoCmd = &cobra.Command{
	Use:   "info <topic>",
	Short: "Show topic details",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectEventBus(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		info, err := grpcCli.EventBus.GetTopicInfo(ctx, &eventpb.TopicInfo{Topic: args[0]})
		if err != nil {
			return fmt.Errorf("failed to get topic info: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoTopicInfoToMap(info))
		}

		printer.Printf("Topic: %s\n", info.Topic)
		printer.Printf("  Subscribers:    %d\n", info.SubscriberCount)
		printer.Printf("  Total Messages: %d\n", info.TotalMessages)
		if info.LastMessageTs > 0 {
			printer.Printf("  Last Message:   %s\n", output.FormatTimestamp(int64(info.LastMessageTs/1000000000)))
		} else {
			printer.Printf("  Last Message:   -\n")
		}
		return nil
	},
}

// ============ event stats ============

var eventStatsCmd = &cobra.Command{
	Use:   "stats [topic]",
	Short: "Show event bus statistics",
	Long:  `Show system statistics or specific topic statistics if topic is provided.`,
	Args:  cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectEventBus(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		if len(args) == 1 {
			// Topic-specific stats
			stats, err := grpcCli.EventBus.GetTopicStats(ctx, &eventpb.TopicInfo{Topic: args[0]})
			if err != nil {
				return fmt.Errorf("failed to get topic stats: %w", err)
			}

			if outputFmt == "json" || outputFmt == "yaml" {
				return printer.Print(protoEventStatsToMap(stats))
			}

			printer.Printf("Topic Stats: %s\n", stats.Topic)
			printer.Printf("  Published:    %d\n", stats.PublishedCount)
			printer.Printf("  Delivered:    %d\n", stats.DeliveredCount)
			printer.Printf("  Dropped:      %d\n", stats.DroppedCount)
			printer.Printf("  Avg Latency:  %.2f µs\n", stats.AvgLatencyUs)
			return nil
		}

		// System stats
		stats, err := grpcCli.EventBus.GetStats(ctx, &eventpb.Empty{})
		if err != nil {
			return fmt.Errorf("failed to get system stats: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoSystemStatsToMap(stats))
		}

		printer.Printf("Event Bus Statistics\n")
		printer.Printf("  Total Topics:      %d\n", stats.TotalTopics)
		printer.Printf("  Total Subscribers: %d\n", stats.TotalSubscribers)
		printer.Printf("  Uptime:            %s\n", output.FormatDuration(int64(stats.UptimeMs/1000)))

		if len(stats.TopicStats) > 0 {
			printer.Printf("\nTopic Statistics:\n")
			table := output.NewTable("TOPIC", "PUBLISHED", "DELIVERED", "DROPPED", "AVG LATENCY")
			for _, ts := range stats.TopicStats {
				table.AddRow(
					ts.Topic,
					fmt.Sprintf("%d", ts.PublishedCount),
					fmt.Sprintf("%d", ts.DeliveredCount),
					fmt.Sprintf("%d", ts.DroppedCount),
					fmt.Sprintf("%.2f µs", ts.AvgLatencyUs),
				)
			}
			table.RenderTo(printer)
		}
		return nil
	},
}

// ============ event publish ============

var (
	publishSource   string
	publishMetadata string
)

var eventPublishCmd = &cobra.Command{
	Use:   "publish <topic> <payload>",
	Short: "Publish an event",
	Long: `Publish an event to a topic.

Examples:
  # Publish a JSON event
  aipc-cli event publish app/test '{"msg":"hello"}'

  # Publish with source and metadata
  aipc-cli event publish app/test '{"data":1}' --source myapp --metadata '{"key":"value"}'
`,
	Args: cobra.ExactArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectEventBus(); err != nil {
			return err
		}

		topic := args[0]
		payload := args[1]

		// Parse metadata if provided
		metadata := make(map[string]string)
		if publishMetadata != "" {
			if err := json.Unmarshal([]byte(publishMetadata), &metadata); err != nil {
				return fmt.Errorf("invalid metadata JSON: %w", err)
			}
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.EventBus.Publish(ctx, &eventpb.PublishRequest{
			Event: &eventpb.Event{
				Topic:       topic,
				TimestampNs: uint64(time.Now().UnixNano()),
				Source:      publishSource,
				Payload:     []byte(payload),
				PayloadType: "json",
				Metadata:    metadata,
			},
		})
		if err != nil {
			return fmt.Errorf("failed to publish event: %w", err)
		}

		if !result.Status.Success {
			printer.Error("Publish failed: %s", result.Status.Message)
			return fmt.Errorf("publish failed")
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(map[string]interface{}{
				"success":  true,
				"event_id": result.EventId,
				"topic":    topic,
			})
		}

		printer.Success("Event published to %s (id: %s)", topic, result.EventId)
		return nil
	},
}

// ============ event subscribe ============

var (
	subscribeFollow bool
	subscribeID     string
	subscribeRaw    bool
)

var eventSubscribeCmd = &cobra.Command{
	Use:   "subscribe <topic>",
	Short: "Subscribe to events",
	Long: `Subscribe to events from a topic. Supports wildcards.

Examples:
  # Subscribe to specific topic
  aipc-cli event subscribe app/test

  # Subscribe with wildcard
  aipc-cli event subscribe 'model/*/detections'

  # Follow mode (continuous)
  aipc-cli event subscribe app/test --follow
`,
	Args: cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectEventBus(); err != nil {
			return err
		}

		topic := args[0]
		if subscribeID == "" {
			subscribeID = fmt.Sprintf("cli-%d", time.Now().UnixNano())
		}

		// Use background context for streaming
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()

		// Handle Ctrl+C
		sigChan := make(chan os.Signal, 1)
		signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
		go func() {
			<-sigChan
			printer.Info("\nUnsubscribing...")
			cancel()
		}()

		stream, err := grpcCli.EventBus.Subscribe(ctx, &eventpb.SubscribeRequest{
			Topic:        topic,
			SubscriberId: subscribeID,
		})
		if err != nil {
			return fmt.Errorf("failed to subscribe: %w", err)
		}

		printer.Info("Subscribed to %s (subscriber: %s)", topic, subscribeID)
		if subscribeFollow {
			printer.Info("Press Ctrl+C to unsubscribe")
		}

		count := 0
		for {
			event, err := stream.Recv()
			if err == io.EOF {
				break
			}
			if err != nil {
				// Check if cancelled
				if ctx.Err() != nil {
					break
				}
				return fmt.Errorf("receive error: %w", err)
			}

			count++

			if outputFmt == "json" {
				data := protoEventToMap(event)
				jsonBytes, _ := json.Marshal(data)
				fmt.Println(string(jsonBytes))
			} else if subscribeRaw {
				fmt.Println(string(event.Payload))
			} else {
				ts := time.Unix(0, int64(event.TimestampNs))
				fmt.Printf("[%s] %s: %s\n", ts.Format("15:04:05.000"), event.Topic, string(event.Payload))
			}

			if !subscribeFollow {
				break
			}
		}

		printer.Info("Received %d event(s)", count)
		return nil
	},
}

// ============ Proto to map conversion helpers ============

func protoTopicListToMap(list *eventpb.TopicListResponse) map[string]interface{} {
	topics := make([]map[string]interface{}, len(list.Topics))
	for i, t := range list.Topics {
		topics[i] = protoTopicInfoToMap(t)
	}
	return map[string]interface{}{"topics": topics}
}

func protoTopicInfoToMap(t *eventpb.TopicInfo) map[string]interface{} {
	return map[string]interface{}{
		"topic":            t.Topic,
		"subscriber_count": t.SubscriberCount,
		"total_messages":   t.TotalMessages,
		"last_message_ts":  t.LastMessageTs,
	}
}

func protoEventStatsToMap(s *eventpb.EventStats) map[string]interface{} {
	return map[string]interface{}{
		"topic":           s.Topic,
		"published_count": s.PublishedCount,
		"delivered_count": s.DeliveredCount,
		"dropped_count":   s.DroppedCount,
		"avg_latency_us":  s.AvgLatencyUs,
	}
}

func protoSystemStatsToMap(s *eventpb.SystemStats) map[string]interface{} {
	topicStats := make([]map[string]interface{}, len(s.TopicStats))
	for i, ts := range s.TopicStats {
		topicStats[i] = protoEventStatsToMap(ts)
	}
	return map[string]interface{}{
		"total_topics":      s.TotalTopics,
		"total_subscribers": s.TotalSubscribers,
		"uptime_ms":         s.UptimeMs,
		"topic_stats":       topicStats,
	}
}

func protoEventToMap(e *eventpb.Event) map[string]interface{} {
	var payload interface{}
	if e.PayloadType == "json" {
		json.Unmarshal(e.Payload, &payload)
	} else {
		payload = string(e.Payload)
	}
	return map[string]interface{}{
		"topic":        e.Topic,
		"timestamp_ns": e.TimestampNs,
		"source":       e.Source,
		"event_id":     e.EventId,
		"payload":      payload,
		"payload_type": e.PayloadType,
		"metadata":     e.Metadata,
	}
}

func init() {
	// event publish flags
	eventPublishCmd.Flags().StringVar(&publishSource, "source", "cli", "Event source identifier")
	eventPublishCmd.Flags().StringVar(&publishMetadata, "metadata", "", "Event metadata as JSON")

	// event subscribe flags
	eventSubscribeCmd.Flags().BoolVarP(&subscribeFollow, "follow", "f", false, "Follow mode (continuous)")
	eventSubscribeCmd.Flags().StringVar(&subscribeID, "id", "", "Subscriber ID (auto-generated if not set)")
	eventSubscribeCmd.Flags().BoolVar(&subscribeRaw, "raw", false, "Output raw payload only")

	// Register subcommands
	eventCmd.AddCommand(eventTopicsCmd)
	eventCmd.AddCommand(eventInfoCmd)
	eventCmd.AddCommand(eventStatsCmd)
	eventCmd.AddCommand(eventPublishCmd)
	eventCmd.AddCommand(eventSubscribeCmd)
}
