package handlers

import (
	"encoding/binary"
	"io"
	"net"
	"sync"
	"time"

	"aipc/platform/common/logger"
	"github.com/gorilla/websocket"
)

const (
	audioSocketPath = "/run/aipc/encoded/audio_capture.sock"
)

// AudioStream manages a single audio stream from UDS to WebSocket clients.
type AudioStream struct {
	SocketPath string
	Clients    map[*websocket.Conn]bool
	ClientsMux sync.RWMutex
	StopChan   chan struct{}
	Active     bool

	conn   net.Conn
	connMu sync.Mutex
}

// AudioStreamManager manages audio streams.
type AudioStreamManager struct {
	stream *AudioStream
	mu     sync.RWMutex
}

// NewAudioStreamManager creates a new audio stream manager.
func NewAudioStreamManager() *AudioStreamManager {
	return &AudioStreamManager{}
}

// GetOrCreateStream returns the audio stream, creating it if needed.
func (m *AudioStreamManager) GetOrCreateStream() (*AudioStream, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if m.stream != nil && m.stream.Active {
		return m.stream, nil
	}

	s := &AudioStream{
		SocketPath: audioSocketPath,
		Clients:    make(map[*websocket.Conn]bool),
		StopChan:   make(chan struct{}),
		Active:     true,
	}
	m.stream = s
	go s.readLoop()

	logger.Info("Audio UDS stream started → %s", audioSocketPath)
	return s, nil
}

// readLoop connects to UDS and reads audio packets with auto-reconnect.
func (s *AudioStream) readLoop() {
	delay := initialReconnect

	for {
		select {
		case <-s.StopChan:
			return
		default:
		}

		if err := s.connectAndRead(); err != nil {
			logger.Error("Audio UDS read error: %v, reconnecting in %v", err, delay)
		}

		select {
		case <-s.StopChan:
			return
		case <-time.After(delay):
		}

		delay = delay * 2
		if delay > maxReconnectDelay {
			delay = maxReconnectDelay
		}
	}
}

// connectAndRead connects to the audio UDS and forwards packets.
func (s *AudioStream) connectAndRead() error {
	conn, err := net.DialTimeout("unix", s.SocketPath, 5*time.Second)
	if err != nil {
		return err
	}

	s.connMu.Lock()
	s.conn = conn
	s.connMu.Unlock()

	defer func() {
		_ = conn.Close()
		s.connMu.Lock()
		if s.conn == conn {
			s.conn = nil
		}
		s.connMu.Unlock()
	}()

	logger.Info("Audio UDS connected: %s", s.SocketPath)

	headerBuf := make([]byte, headerSizeV2)

	for {
		select {
		case <-s.StopChan:
			return nil
		default:
		}

		// Read V1 header first (22 bytes)
		if _, err := io.ReadFull(conn, headerBuf[:headerSizeV1]); err != nil {
			return err
		}

		totalSize := binary.LittleEndian.Uint32(headerBuf[0:4])

		// Read remaining header bytes if V2
		currentHeaderSize := headerSizeV1
		if totalSize >= headerSizeV2 {
			if _, err := io.ReadFull(conn, headerBuf[headerSizeV1:headerSizeV2]); err != nil {
				return err
			}
			currentHeaderSize = headerSizeV2
		}

		payloadSize := totalSize - uint32(currentHeaderSize)
		if payloadSize == 0 || payloadSize > 512*1024 {
			continue
		}

		payload := make([]byte, payloadSize)
		if _, err := io.ReadFull(conn, payload); err != nil {
			return err
		}

		// Skip if no clients
		s.ClientsMux.RLock()
		clientCount := len(s.Clients)
		s.ClientsMux.RUnlock()

		if clientCount == 0 {
			continue
		}

		s.broadcast(payload)
	}
}

// broadcast sends audio data to all connected WebSocket clients.
func (s *AudioStream) broadcast(data []byte) {
	var failed []*websocket.Conn

	s.ClientsMux.RLock()
	for client := range s.Clients {
		if err := client.WriteMessage(websocket.BinaryMessage, data); err != nil {
			failed = append(failed, client)
		}
	}
	s.ClientsMux.RUnlock()

	if len(failed) > 0 {
		s.ClientsMux.Lock()
		for _, c := range failed {
			c.Close()
			delete(s.Clients, c)
		}
		s.ClientsMux.Unlock()
	}
}

// AddClient adds a WebSocket client.
func (s *AudioStream) AddClient(conn *websocket.Conn) {
	s.ClientsMux.Lock()
	s.Clients[conn] = true
	s.ClientsMux.Unlock()
	logger.Debug("Audio client added, total: %d", len(s.Clients))
}

// RemoveClient removes a WebSocket client.
func (s *AudioStream) RemoveClient(conn *websocket.Conn) {
	s.ClientsMux.Lock()
	delete(s.Clients, conn)
	s.ClientsMux.Unlock()
	logger.Debug("Audio client removed, total: %d", len(s.Clients))
}

// Stop stops the audio stream.
func (s *AudioStream) Stop() {
	close(s.StopChan)
	s.connMu.Lock()
	if s.conn != nil {
		_ = s.conn.Close()
		s.conn = nil
	}
	s.connMu.Unlock()
	s.Active = false
}

// RemoveStream stops and removes the audio stream.
func (m *AudioStreamManager) RemoveStream() {
	m.mu.Lock()
	defer m.mu.Unlock()

	if m.stream != nil {
		m.stream.Stop()
		m.stream = nil
		logger.Info("Audio stream removed")
	}
}
