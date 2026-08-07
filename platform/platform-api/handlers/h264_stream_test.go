package handlers

import (
	"fmt"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// fakeConn is a minimal net.Conn that records bytes written to it.
type fakeConn struct {
	mu    sync.Mutex
	wrote []byte
}

func (f *fakeConn) Write(p []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.wrote = append(f.wrote, p...)
	return len(p), nil
}
func (f *fakeConn) Read(p []byte) (int, error)         { return 0, nil }
func (f *fakeConn) Close() error                       { return nil }
func (f *fakeConn) LocalAddr() net.Addr                { return nil }
func (f *fakeConn) RemoteAddr() net.Addr               { return nil }
func (f *fakeConn) SetDeadline(t time.Time) error      { return nil }
func (f *fakeConn) SetReadDeadline(t time.Time) error  { return nil }
func (f *fakeConn) SetWriteDeadline(t time.Time) error { return nil }

func (f *fakeConn) bytes() []byte {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]byte, len(f.wrote))
	copy(out, f.wrote)
	return out
}

// TestForceKeyframe_ManagerWritesControlByte verifies the manager-level
// ForceKeyframe delegates to the stream and writes the keyframe control byte
// (ctrlRequestKeyframe = 0x4B 'K') over the UDS connection.
func TestForceKeyframe_ManagerWritesControlByte(t *testing.T) {
	m := NewH264StreamManager()
	fc := &fakeConn{}
	m.streams["main"] = &H264Stream{StreamID: "main", conn: fc}

	m.ForceKeyframe("main")

	got := fc.bytes()
	if len(got) != 1 || got[0] != ctrlRequestKeyframe {
		t.Fatalf("expected single control byte 0x4B, got %v", got)
	}
}

// TestForceKeyframe_ManagerMissingStreamIsNoop ensures requesting a keyframe for
// a stream that does not exist is a safe no-op (no panic, no write).
func TestForceKeyframe_ManagerMissingStreamIsNoop(t *testing.T) {
	m := NewH264StreamManager()

	// Must not panic on a missing stream.
	m.ForceKeyframe("nonexistent")

	fc := &fakeConn{}
	m.streams["other"] = &H264Stream{StreamID: "other", conn: fc}
	// No conn set on "main" → requestKeyframe must skip the write silently.
	m.ForceKeyframe("main")

	if len(fc.bytes()) != 0 {
		t.Fatalf("expected no write to unrelated stream, got %v", fc.bytes())
	}
}

// TestForceKeyframe_NilConnIsNoop verifies requestKeyframe skips the write when
// the stream has no live UDS connection (conn == nil).
func TestForceKeyframe_NilConnIsNoop(t *testing.T) {
	m := NewH264StreamManager()
	m.streams["main"] = &H264Stream{StreamID: "main"} // conn == nil

	// Must not panic and must not write.
	m.ForceKeyframe("main")
}

// TestStreamHandlersForceKeyframe_Delegates verifies the StreamHandlers wrapper
// delegates to its H264StreamManager.
func TestStreamHandlersForceKeyframe_Delegates(t *testing.T) {
	fc := &fakeConn{}
	h := &StreamHandlers{
		h264Streams: NewH264StreamManager(),
	}
	h.h264Streams.streams["main"] = &H264Stream{StreamID: "main", conn: fc}

	h.ForceKeyframe("main")

	got := fc.bytes()
	if len(got) != 1 || got[0] != ctrlRequestKeyframe {
		t.Fatalf("expected single control byte 0x4B via wrapper, got %v", got)
	}
}

// TestStreamHandlersForceKeyframe_NilManagerIsSafe ensures the wrapper tolerates
// a nil H264StreamManager (defensive — some test/early-init paths may not set it).
func TestStreamHandlersForceKeyframe_NilManagerIsSafe(t *testing.T) {
	h := &StreamHandlers{}  // h264Streams == nil
	h.ForceKeyframe("main") // must not panic
}

func TestStreamHandlersReserveWSClientReplacesOldest(t *testing.T) {
	h := &StreamHandlers{wsClientsByIP: make(map[string][]*wsClientRef)}
	const ip = "192.168.93.9"

	conns := make([]*websocket.Conn, maxWSPerIP+1)
	for i := range conns {
		conns[i] = &websocket.Conn{}
		victim := h.reserveWSClient(ip, fmt.Sprintf("stream-%d", i), conns[i])
		if i < maxWSPerIP && victim != nil {
			t.Fatalf("reserve %d should not evict, got victim=%p", i, victim.conn)
		}
		if i == maxWSPerIP {
			if victim == nil || victim.conn != conns[0] {
				t.Fatalf("expected oldest connection to be evicted, got %#v", victim)
			}
		}
	}

	clients := h.wsClientsByIP[ip]
	if len(clients) != maxWSPerIP {
		t.Fatalf("expected %d reserved clients, got %d", maxWSPerIP, len(clients))
	}
	if clients[0].conn != conns[1] {
		t.Fatalf("oldest live connection should now be original second connection")
	}
	if clients[len(clients)-1].conn != conns[maxWSPerIP] {
		t.Fatalf("new connection should be retained at the end of the queue")
	}

	h.releaseWSClient(ip, conns[3])
	if got := len(h.wsClientsByIP[ip]); got != maxWSPerIP-1 {
		t.Fatalf("expected release to remove one client, got %d", got)
	}

	h.releaseWSClient(ip, conns[0]) // already evicted; no-op
	if got := len(h.wsClientsByIP[ip]); got != maxWSPerIP-1 {
		t.Fatalf("expected releasing an evicted client to be no-op, got %d", got)
	}

	for _, conn := range conns {
		h.releaseWSClient(ip, conn)
	}
	if _, ok := h.wsClientsByIP[ip]; ok {
		t.Fatalf("expected IP bucket to be removed after all clients release")
	}
}
