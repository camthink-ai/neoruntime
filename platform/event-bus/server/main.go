package main

import (
	"context"
	"flag"
	"fmt"
	"net"
	"os"
	"os/signal"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"google.golang.org/grpc"

	"aipc/platform/common/config"
	"aipc/platform/common/logger"
	"aipc/platform/common/socket"
	"aipc/platform/common/utils"
	pb "aipc/platform/event-bus/proto"
)

var (
	configPath = flag.String("config", "/data/aipc/etc/event-bus.yaml", "Path to configuration file")
)

type Config struct {
	Service struct {
		Name      string `yaml:"name"`
		Listen    string `yaml:"listen"`
		TCPListen string `yaml:"tcp_listen"` // optional TCP listener for C++ clients
		LogLevel  string `yaml:"log_level"`
		LogFile   string `yaml:"log_file"`
	} `yaml:"service"`

	Bus struct {
		QueueSize      int  `yaml:"queue_size"`
		PersistEnabled bool `yaml:"persist_enabled"`
		MaxTopics      int  `yaml:"max_topics"`
	} `yaml:"bus"`
}

func (c *Config) Validate() error {
	if c.Service.Listen == "" {
		return fmt.Errorf("service.listen is required")
	}
	return nil
}

// Subscriber represents a client subscription
type Subscriber struct {
	ID      string
	Topic   string
	Stream  pb.EventBus_SubscribeServer
	Filters map[string]string
	EventCh chan *pb.Event
	QuitCh  chan struct{}
}

// EventBusServer implements the EventBus service
type EventBusServer struct {
	pb.UnimplementedEventBusServer
	config       *Config
	subscribers  map[string][]*Subscriber // topic -> subscribers
	subMutex     sync.RWMutex
	stats        map[string]*EventStats
	statsMutex   sync.RWMutex
	eventCounter uint64 // atomic counter for event ID generation
}

type EventStats struct {
	PublishedCount atomic.Uint64
	DeliveredCount atomic.Uint64
	DroppedCount   atomic.Uint64
	LastMessageTs  atomic.Uint64
}

func NewEventBusServer(cfg *Config) *EventBusServer {
	return &EventBusServer{
		config:      cfg,
		subscribers: make(map[string][]*Subscriber),
		stats:       make(map[string]*EventStats),
	}
}

// Publish implements EventBus
func (s *EventBusServer) Publish(ctx context.Context, req *pb.PublishRequest) (*pb.PublishResponse, error) {
	event := req.Event
	if event == nil {
		return &pb.PublishResponse{
			Status: &pb.Status{
				Success: false,
				Message: "Event is nil",
			},
		}, nil
	}

	// Generate event ID if not provided
	eventID := event.EventId
	if eventID == "" {
		seq := atomic.AddUint64(&s.eventCounter, 1)
		eventID = fmt.Sprintf("evt-%d-%d", time.Now().UnixNano()/1000000, seq)
		event.EventId = eventID
	}

	// Set timestamp if not provided
	if event.TimestampNs == 0 {
		event.TimestampNs = uint64(time.Now().UnixNano())
	}

	logger.Info("Publish: topic=%s, source=%s, id=%s", event.Topic, event.Source, eventID)

	// Update statistics
	stats := s.getOrCreateStats(event.Topic)
	stats.PublishedCount.Add(1)
	stats.LastMessageTs.Store(uint64(time.Now().UnixNano()))

	// Find matching subscribers
	s.subMutex.RLock()
	subscribers := s.findMatchingSubscribers(event.Topic)
	s.subMutex.RUnlock()

	logger.Info("Found %d matching subscribers for topic %s", len(subscribers), event.Topic)

	// Deliver to subscribers
	delivered := 0
	dropped := 0
	for _, sub := range subscribers {
		if len(sub.Filters) > 0 {
			match := true
			for k, v := range sub.Filters {
				if event.Metadata == nil {
					match = false
					break
				}
				if ev, ok := event.Metadata[k]; !ok || ev != v {
					match = false
					break
				}
			}
			if !match {
				continue
			}
		}

		select {
		case sub.EventCh <- event:
			delivered++
			logger.Info("Delivered event to subscriber %s", sub.ID)
		default:
			// Channel full, drop message
			logger.Warn("Dropped message for subscriber %s (channel full)", sub.ID)
			dropped++
		}
	}

	if delivered > 0 {
		stats.DeliveredCount.Add(uint64(delivered))
	}
	if dropped > 0 {
		stats.DroppedCount.Add(uint64(dropped))
	}

	logger.Info("Published event to %d subscribers", delivered)

	return &pb.PublishResponse{
		Status: &pb.Status{
			Success: true,
		},
		EventId: event.EventId,
	}, nil
}

// Subscribe implements EventBus
func (s *EventBusServer) Subscribe(req *pb.SubscribeRequest, stream pb.EventBus_SubscribeServer) error {
	logger.Info("Subscribe: topic=%s, subscriber=%s", req.Topic, req.SubscriberId)

	// Create subscriber
	sub := &Subscriber{
		ID:      req.SubscriberId,
		Topic:   req.Topic,
		Stream:  stream,
		Filters: req.Filters,
		EventCh: make(chan *pb.Event, s.config.Bus.QueueSize),
		QuitCh:  make(chan struct{}),
	}

	// Register subscriber
	s.subMutex.Lock()
	s.subscribers[req.Topic] = append(s.subscribers[req.Topic], sub)
	s.subMutex.Unlock()

	// Cleanup on exit
	defer func() {
		logger.Info("Unsubscribe: topic=%s, subscriber=%s", req.Topic, req.SubscriberId)
		s.removeSubscriber(sub)
	}()

	// Send events to client
	for {
		select {
		case event := <-sub.EventCh:
			if err := stream.Send(event); err != nil {
				logger.Warn("Failed to send event to %s: %v", sub.ID, err)
				return err
			}
		case <-sub.QuitCh:
			return nil
		case <-stream.Context().Done():
			return stream.Context().Err()
		}
	}
}

// Unsubscribe implements EventBus
func (s *EventBusServer) Unsubscribe(ctx context.Context, req *pb.SubscribeRequest) (*pb.Status, error) {
	logger.Info("Unsubscribe request: topic=%s, subscriber=%s", req.Topic, req.SubscriberId)

	s.subMutex.Lock()
	defer s.subMutex.Unlock()

	if subs, ok := s.subscribers[req.Topic]; ok {
		for i, sub := range subs {
			if sub.ID == req.SubscriberId {
				close(sub.QuitCh)
				s.subscribers[req.Topic] = append(subs[:i], subs[i+1:]...)
				if len(s.subscribers[req.Topic]) == 0 {
					delete(s.subscribers, req.Topic)
				}
				break
			}
		}
	}

	return &pb.Status{Success: true}, nil
}

// ListTopics implements EventBus
func (s *EventBusServer) ListTopics(ctx context.Context, req *pb.Empty) (*pb.TopicListResponse, error) {
	s.subMutex.RLock()
	defer s.subMutex.RUnlock()

	s.statsMutex.RLock()
	defer s.statsMutex.RUnlock()

	topics := make([]*pb.TopicInfo, 0, len(s.subscribers))
	for topic, subs := range s.subscribers {
		stats := s.stats[topic]
		if stats == nil {
			stats = &EventStats{}
		}

		topics = append(topics, &pb.TopicInfo{
			Topic:           topic,
			SubscriberCount: uint32(len(subs)),
			TotalMessages:   stats.PublishedCount.Load(),
			LastMessageTs:   stats.LastMessageTs.Load(),
		})
	}

	return &pb.TopicListResponse{Topics: topics}, nil
}

// GetStats implements EventBus
func (s *EventBusServer) GetStats(ctx context.Context, req *pb.Empty) (*pb.SystemStats, error) {
	s.subMutex.RLock()
	totalSubscribers := 0
	for _, subs := range s.subscribers {
		totalSubscribers += len(subs)
	}
	s.subMutex.RUnlock()

	return &pb.SystemStats{
		TotalSubscribers: uint32(totalSubscribers),
		TotalTopics:      uint32(len(s.subscribers)),
		UptimeMs:         uint64(time.Since(startTime).Milliseconds()),
	}, nil
}

// Helper functions

func (s *EventBusServer) findMatchingSubscribers(topic string) []*Subscriber {
	var matches []*Subscriber

	for subTopic, subs := range s.subscribers {
		if s.topicMatch(topic, subTopic) {
			matches = append(matches, subs...)
		}
	}

	return matches
}

func (s *EventBusServer) topicMatch(topic, pattern string) bool {
	// Use common topic matching utility for wildcard support
	// Supports: exact match, "*", "**", and multi-segment patterns
	return utils.MatchTopic(topic, pattern)
}

func (s *EventBusServer) removeSubscriber(sub *Subscriber) {
	s.subMutex.Lock()
	defer s.subMutex.Unlock()

	if subs, ok := s.subscribers[sub.Topic]; ok {
		for i, subscriber := range subs {
			if subscriber == sub { // Compare pointer, not ID
				s.subscribers[sub.Topic] = append(subs[:i], subs[i+1:]...)
				if len(s.subscribers[sub.Topic]) == 0 {
					delete(s.subscribers, sub.Topic)
				}
				break
			}
		}
	}
}

func (s *EventBusServer) getOrCreateStats(topic string) *EventStats {
	s.statsMutex.RLock()
	stats, ok := s.stats[topic]
	s.statsMutex.RUnlock()
	if ok {
		return stats
	}

	s.statsMutex.Lock()
	defer s.statsMutex.Unlock()
	stats, ok = s.stats[topic]
	if !ok {
		stats = &EventStats{}
		s.stats[topic] = stats
	}
	return stats
}

var startTime time.Time

func main() {
	flag.Parse()
	startTime = time.Now()

	// Load configuration
	var cfg Config
	if err := config.LoadYAML(*configPath, &cfg); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load config: %v\n", err)
		os.Exit(1)
	}

	// Setup logger
	logger.SetLevelFromString(cfg.Service.LogLevel)

	// Configure log file output if specified
	if cfg.Service.LogFile != "" {
		if err := logger.SetOutputFile(cfg.Service.LogFile); err != nil {
			fmt.Fprintf(os.Stderr, "Failed to set log file: %v\n", err)
		} else {
			logger.Info("Logging to file: %s", cfg.Service.LogFile)
		}
	}

	logger.Info("Starting %s", cfg.Service.Name)
	logger.Info("Config file: %s", *configPath)
	logger.Info("Listen address: %s", cfg.Service.Listen)
	logger.Info("TCP listen address: %s", cfg.Service.TCPListen)

	// Parse listen address (handle unix:// URLs)
	listenAddr, err := utils.ParseListenAddress(cfg.Service.Listen)
	if err != nil {
		logger.Fatal("Failed to parse listen address: %v", err)
	}

	// Remove trailing socket file if it exists from a previous ungraceful exit
	os.Remove(listenAddr)
	// Create gRPC server
	lis, err := net.Listen("unix", listenAddr)
	if err != nil {
		logger.Fatal("Failed to listen on unix %s: %v", listenAddr, err)
	}
	defer os.Remove(listenAddr)

	// Set socket permissions for container access
	if err := socket.SetSocketGroupPermission(listenAddr); err != nil {
		logger.Warn("Failed to set socket permissions: %v (containers may not be able to connect)", err)
	} else {
		logger.Info("Socket permissions set for container access: %s", listenAddr)
	}

	grpcServer := grpc.NewServer()
	eventBusServer := NewEventBusServer(&cfg)
	pb.RegisterEventBusServer(grpcServer, eventBusServer)

	// Optional TCP listener (for C++ gRPC clients lacking unix: resolver)
	if cfg.Service.TCPListen != "" {
		tcpLis, err := net.Listen("tcp", cfg.Service.TCPListen)
		if err != nil {
			logger.Fatal("Failed to listen on tcp %s: %v", cfg.Service.TCPListen, err)
		}
		logger.Info("TCP listener on %s", cfg.Service.TCPListen)
		go func() {
			if err := grpcServer.Serve(tcpLis); err != nil {
				logger.Warn("TCP serve ended: %v", err)
			}
		}()
	}

	// Handle shutdown gracefully
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	go func() {
		<-sigChan
		logger.Info("Shutting down...")
		stopped := make(chan struct{})
		go func() {
			grpcServer.GracefulStop()
			close(stopped)
		}()
		select {
		case <-stopped:
			logger.Info("Graceful stop completed")
		case <-time.After(3 * time.Second):
			logger.Info("Graceful stop timed out (open subscriptions?), force stopping...")
			grpcServer.Stop()
		}
	}()

	logger.Info("Event Bus started successfully")

	if err := grpcServer.Serve(lis); err != nil {
		logger.Fatal("Failed to serve: %v", err)
	}
}
