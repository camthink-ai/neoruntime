package main

import (
	"context"
	"fmt"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	pb "aipc/platform/event-bus/proto"
)

func newTestServer(queueSize int) *EventBusServer {
	return NewEventBusServer(&Config{
		Bus: struct {
			QueueSize      int  `yaml:"queue_size"`
			PersistEnabled bool `yaml:"persist_enabled"`
			MaxTopics      int  `yaml:"max_topics"`
		}{
			QueueSize: queueSize,
		},
	})
}

// ===================== Basic Tests =====================

func TestEventBusServer(t *testing.T) {
	cfg := &Config{}
	cfg.Service.Listen = "unix:///tmp/test-event-bus.sock"
	cfg.Bus.QueueSize = 100

	server := NewEventBusServer(cfg)

	if server == nil {
		t.Fatal("Failed to create server")
	}
}

func TestPublishNilEvent(t *testing.T) {
	server := newTestServer(100)
	ctx := context.Background()

	resp, err := server.Publish(ctx, &pb.PublishRequest{Event: nil})
	if err != nil {
		t.Fatalf("Unexpected error: %v", err)
	}
	if resp.Status.Success {
		t.Fatal("Expected failure for nil event")
	}
}

func TestPublishWithoutSubscribers(t *testing.T) {
	server := newTestServer(100)
	ctx := context.Background()

	event := &pb.Event{
		Topic:       "test/topic",
		TimestampNs: uint64(time.Now().UnixNano()),
		Source:      "test",
		Payload:     []byte(`{"msg":"hello"}`),
		PayloadType: "json",
	}

	resp, err := server.Publish(ctx, &pb.PublishRequest{Event: event})
	if err != nil {
		t.Fatalf("Publish failed: %v", err)
	}
	if !resp.Status.Success {
		t.Fatalf("Publish status false: %s", resp.Status.Message)
	}
}

// ===================== Topic Matching =====================

func TestTopicMatching(t *testing.T) {
	server := newTestServer(100)

	testCases := []struct {
		topic   string
		pattern string
		match   bool
	}{
		// Exact match
		{"app/test/alert", "app/test/alert", true},
		{"model/person_v1/detection", "model/person_v1/detection", true},

		// Single wildcard
		{"app/test/alert", "app/test/*", true},
		{"app/test/alert", "app/*/alert", true},
		{"app/test/alert", "*/test/alert", true},

		// Single wildcard should not match multiple segments
		{"app/test/sub/alert", "app/*/alert", false},

		// Double wildcard
		{"app/test/alert", "app/**", true},
		{"app/a/b/c/d", "app/**", true},
		{"app/test", "app/**", true},
		{"system/health", "app/**", false},

		// Double wildcard with suffix
		{"app/foo/bar/events", "app/**/events", true},
		{"app/events", "app/**/events", true},

		// No match
		{"app/test/alert", "system/test/alert", false},
		{"app/test", "app/test/alert", false},
	}

	for _, tc := range testCases {
		result := server.topicMatch(tc.topic, tc.pattern)
		if result != tc.match {
			t.Errorf("topicMatch(%q, %q) = %v, want %v",
				tc.topic, tc.pattern, result, tc.match)
		}
	}
}

// ===================== Subscriber Delivery =====================

func TestPublishDeliveredToMatchingSubscribers(t *testing.T) {
	server := newTestServer(100)

	// Manually register subscribers
	sub1 := &Subscriber{
		ID:      "sub-exact",
		Topic:   "test/events",
		EventCh: make(chan *pb.Event, 10),
		QuitCh:  make(chan struct{}),
	}
	sub2 := &Subscriber{
		ID:      "sub-wildcard",
		Topic:   "test/*",
		EventCh: make(chan *pb.Event, 10),
		QuitCh:  make(chan struct{}),
	}
	sub3 := &Subscriber{
		ID:      "sub-unrelated",
		Topic:   "other/events",
		EventCh: make(chan *pb.Event, 10),
		QuitCh:  make(chan struct{}),
	}

	server.subMutex.Lock()
	server.subscribers["test/events"] = []*Subscriber{sub1}
	server.subscribers["test/*"] = []*Subscriber{sub2}
	server.subscribers["other/events"] = []*Subscriber{sub3}
	server.subMutex.Unlock()

	// Publish event
	ctx := context.Background()
	_, err := server.Publish(ctx, &pb.PublishRequest{
		Event: &pb.Event{
			Topic:   "test/events",
			Payload: []byte(`{"data":"hello"}`),
			Source:  "test",
		},
	})
	if err != nil {
		t.Fatalf("Publish failed: %v", err)
	}

	// Check sub1 (exact match) received
	select {
	case e := <-sub1.EventCh:
		if e.Topic != "test/events" {
			t.Errorf("sub1 got wrong topic: %s", e.Topic)
		}
	default:
		t.Error("sub1 (exact match) did not receive event")
	}

	// Check sub2 (wildcard match) received
	select {
	case e := <-sub2.EventCh:
		if e.Topic != "test/events" {
			t.Errorf("sub2 got wrong topic: %s", e.Topic)
		}
	default:
		t.Error("sub2 (wildcard match) did not receive event")
	}

	// Check sub3 (unrelated) did NOT receive
	select {
	case <-sub3.EventCh:
		t.Error("sub3 (unrelated) should not receive event")
	default:
		// expected
	}
}

func TestPublishDropsOnFullQueue(t *testing.T) {
	server := newTestServer(1) // queue size = 1

	sub := &Subscriber{
		ID:      "sub-slow",
		Topic:   "test/events",
		EventCh: make(chan *pb.Event, 1),
		QuitCh:  make(chan struct{}),
	}

	server.subMutex.Lock()
	server.subscribers["test/events"] = []*Subscriber{sub}
	server.subMutex.Unlock()

	ctx := context.Background()

	// Fill the queue
	server.Publish(ctx, &pb.PublishRequest{
		Event: &pb.Event{Topic: "test/events", Payload: []byte(`{"n":1}`)},
	})

	// This should be dropped (queue full)
	server.Publish(ctx, &pb.PublishRequest{
		Event: &pb.Event{Topic: "test/events", Payload: []byte(`{"n":2}`)},
	})

	// Verify stats show a drop
	server.statsMutex.RLock()
	stats := server.stats["test/events"]
	dropped := stats.DroppedCount.Load()
	server.statsMutex.RUnlock()

	if dropped == 0 {
		t.Error("Expected at least 1 dropped message")
	}
	t.Logf("Dropped count: %d", dropped)
}

// ===================== Unsubscribe =====================

func TestUnsubscribe(t *testing.T) {
	server := newTestServer(100)

	sub := &Subscriber{
		ID:      "sub-1",
		Topic:   "test/events",
		EventCh: make(chan *pb.Event, 10),
		QuitCh:  make(chan struct{}),
	}

	server.subMutex.Lock()
	server.subscribers["test/events"] = []*Subscriber{sub}
	server.subMutex.Unlock()

	ctx := context.Background()
	status, err := server.Unsubscribe(ctx, &pb.SubscribeRequest{
		Topic:        "test/events",
		SubscriberId: "sub-1",
	})
	if err != nil {
		t.Fatalf("Unsubscribe failed: %v", err)
	}
	if !status.Success {
		t.Fatal("Unsubscribe returned false")
	}

	// Verify subscriber removed
	server.subMutex.RLock()
	remaining := len(server.subscribers["test/events"])
	server.subMutex.RUnlock()

	if remaining != 0 {
		t.Errorf("Expected 0 subscribers, got %d", remaining)
	}
}

// ===================== Statistics =====================

func TestListTopics(t *testing.T) {
	server := newTestServer(100)

	// Add subscribers on different topics
	server.subMutex.Lock()
	server.subscribers["app/events"] = []*Subscriber{
		{ID: "s1", Topic: "app/events", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})},
		{ID: "s2", Topic: "app/events", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})},
	}
	server.subscribers["model/detections"] = []*Subscriber{
		{ID: "s3", Topic: "model/detections", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})},
	}
	server.subMutex.Unlock()

	ctx := context.Background()
	resp, err := server.ListTopics(ctx, &pb.Empty{})
	if err != nil {
		t.Fatalf("ListTopics failed: %v", err)
	}

	if len(resp.Topics) != 2 {
		t.Fatalf("Expected 2 topics, got %d", len(resp.Topics))
	}

	topicMap := map[string]uint32{}
	for _, ti := range resp.Topics {
		topicMap[ti.Topic] = ti.SubscriberCount
	}

	if topicMap["app/events"] != 2 {
		t.Errorf("app/events: expected 2 subscribers, got %d", topicMap["app/events"])
	}
	if topicMap["model/detections"] != 1 {
		t.Errorf("model/detections: expected 1 subscriber, got %d", topicMap["model/detections"])
	}
}

func TestGetStats(t *testing.T) {
	server := newTestServer(100)

	server.subMutex.Lock()
	server.subscribers["t1"] = []*Subscriber{
		{ID: "s1", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})},
	}
	server.subscribers["t2"] = []*Subscriber{
		{ID: "s2", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})},
		{ID: "s3", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})},
	}
	server.subMutex.Unlock()

	ctx := context.Background()
	stats, err := server.GetStats(ctx, &pb.Empty{})
	if err != nil {
		t.Fatalf("GetStats failed: %v", err)
	}

	if stats.TotalSubscribers != 3 {
		t.Errorf("Expected 3 total subscribers, got %d", stats.TotalSubscribers)
	}
	if stats.TotalTopics != 2 {
		t.Errorf("Expected 2 total topics, got %d", stats.TotalTopics)
	}
}

// ===================== Concurrent Publish =====================

func TestConcurrentPublish(t *testing.T) {
	server := newTestServer(10000)
	ctx := context.Background()

	// Register subscribers on multiple topics
	const numTopics = 5
	subs := make([]*Subscriber, numTopics)
	for i := 0; i < numTopics; i++ {
		topic := fmt.Sprintf("test/topic/%d", i)
		subs[i] = &Subscriber{
			ID:      fmt.Sprintf("sub-%d", i),
			Topic:   topic,
			EventCh: make(chan *pb.Event, 10000),
			QuitCh:  make(chan struct{}),
		}
		server.subMutex.Lock()
		server.subscribers[topic] = []*Subscriber{subs[i]}
		server.subMutex.Unlock()
	}

	// Concurrent publish
	var wg sync.WaitGroup
	const goroutines = 10
	const msgsPerGoroutine = 100
	var published int64

	for g := 0; g < goroutines; g++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for j := 0; j < msgsPerGoroutine; j++ {
				topic := fmt.Sprintf("test/topic/%d", j%numTopics)
				resp, err := server.Publish(ctx, &pb.PublishRequest{
					Event: &pb.Event{
						Topic:   topic,
						Payload: []byte(fmt.Sprintf(`{"g":%d,"j":%d}`, id, j)),
						Source:  fmt.Sprintf("goroutine-%d", id),
					},
				})
				if err != nil {
					t.Errorf("Publish error: %v", err)
					return
				}
				if resp.Status.Success {
					atomic.AddInt64(&published, 1)
				}
			}
		}(g)
	}

	wg.Wait()

	totalPublished := atomic.LoadInt64(&published)
	if totalPublished != goroutines*msgsPerGoroutine {
		t.Errorf("Expected %d published, got %d", goroutines*msgsPerGoroutine, totalPublished)
	}

	// Count total delivered
	totalDelivered := 0
	for _, sub := range subs {
		totalDelivered += len(sub.EventCh)
	}

	t.Logf("Published: %d, Delivered to channels: %d", totalPublished, totalDelivered)

	if totalDelivered == 0 {
		t.Error("No events delivered to any subscriber")
	}
}

// ===================== Multiple Subscribers Same Topic =====================

func TestMultipleSubscribersSameTopic(t *testing.T) {
	server := newTestServer(100)

	sub1 := &Subscriber{
		ID:      "sub-1",
		Topic:   "shared/topic",
		EventCh: make(chan *pb.Event, 10),
		QuitCh:  make(chan struct{}),
	}
	sub2 := &Subscriber{
		ID:      "sub-2",
		Topic:   "shared/topic",
		EventCh: make(chan *pb.Event, 10),
		QuitCh:  make(chan struct{}),
	}

	server.subMutex.Lock()
	server.subscribers["shared/topic"] = []*Subscriber{sub1, sub2}
	server.subMutex.Unlock()

	ctx := context.Background()
	server.Publish(ctx, &pb.PublishRequest{
		Event: &pb.Event{Topic: "shared/topic", Payload: []byte(`{"v":1}`)},
	})

	// Both should receive
	select {
	case <-sub1.EventCh:
	default:
		t.Error("sub1 did not receive")
	}
	select {
	case <-sub2.EventCh:
	default:
		t.Error("sub2 did not receive")
	}
}

// ===================== Wildcard Subscriber Delivery =====================

func TestWildcardSubscriberDelivery(t *testing.T) {
	server := newTestServer(100)

	// Subscribe with ** wildcard
	subAll := &Subscriber{
		ID:      "sub-all",
		Topic:   "model/**",
		EventCh: make(chan *pb.Event, 10),
		QuitCh:  make(chan struct{}),
	}

	server.subMutex.Lock()
	server.subscribers["model/**"] = []*Subscriber{subAll}
	server.subMutex.Unlock()

	ctx := context.Background()

	// Publish to various sub-topics
	topics := []string{"model/person/detect", "model/car/detect", "model/face/embedding"}
	for _, topic := range topics {
		server.Publish(ctx, &pb.PublishRequest{
			Event: &pb.Event{Topic: topic, Payload: []byte(`{}`)},
		})
	}

	received := 0
	for range len(topics) {
		select {
		case <-subAll.EventCh:
			received++
		default:
		}
	}

	if received != len(topics) {
		t.Errorf("Expected %d events, received %d", len(topics), received)
	}
}

// ===================== Remove Subscriber =====================

func TestRemoveSubscriber(t *testing.T) {
	server := newTestServer(100)

	sub1 := &Subscriber{ID: "s1", Topic: "t", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})}
	sub2 := &Subscriber{ID: "s2", Topic: "t", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})}
	sub3 := &Subscriber{ID: "s3", Topic: "t", EventCh: make(chan *pb.Event, 1), QuitCh: make(chan struct{})}

	server.subMutex.Lock()
	server.subscribers["t"] = []*Subscriber{sub1, sub2, sub3}
	server.subMutex.Unlock()

	// Remove middle one
	server.removeSubscriber(sub2)

	server.subMutex.RLock()
	remaining := server.subscribers["t"]
	server.subMutex.RUnlock()

	if len(remaining) != 2 {
		t.Fatalf("Expected 2, got %d", len(remaining))
	}
	if remaining[0].ID != "s1" || remaining[1].ID != "s3" {
		t.Errorf("Wrong subscribers remaining: %s, %s", remaining[0].ID, remaining[1].ID)
	}
}
