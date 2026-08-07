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
	// EncodedPublisher protocol constants
	headerSizeV1        = 22   // V1 protocol (legacy)
	headerSizeV2        = 30   // V2 protocol (adds DTS support)
	ctrlRequestKeyframe = 0x4B // 'K'
	maxReconnectDelay   = 10 * time.Second
	initialReconnect    = time.Second
	frameHeaderSizeV1   = 10                // V1 FrameHeader size (legacy)
	frameHeaderSizeV2   = 18                // V2 FrameHeader size (adds DTS + flags)
	frameHeaderSize     = frameHeaderSizeV2 // Use V2 by default

	// Timeouts to prevent permanent black screen.
	// wsWriteTimeout is the per-client hard cap on one WriteMessage: a slow/
	// zombie client whose TCP receive window is full must be evicted quickly
	// instead of stalling the relay. With per-client send goroutines a blocked
	// write only affects that one client; we still cap it to reap dead clients.
	wsWriteTimeout  = 1 * time.Second  // Max time per WebSocket client write (was 5s)
	spsWriteTimeout = 3 * time.Second  // Max time for SPS/PPS delivery to new clients
	udsReadTimeout  = 15 * time.Second // Detect dead UDS connections

	// Per-client send queue + slow-client eviction. broadcastNAL does a
	// non-blocking send into each client's channel — a slow consumer only
	// drops its own frames, never stalls the relay or other clients.
	clientSendQueue    = 60                    // ~2s buffer at 30fps before dropping
	slowWriteEvictAt   = 50 * time.Millisecond // count as "slow" above this
	maxConsecutiveSlow = 5                     // evict after N consecutive slow writes
)

// FrameHeaderV2 is a 18-byte header added to each frame for frontend
// V2 format: adds DTS support for B-frames
type FrameHeaderV2 struct {
	Magic     uint32 // Magic number: 0xFFD8FF00 (4 bytes)
	FrameType uint8  // 0=I-frame, 1=P/B-frame (1 byte)
	Flags     uint8  // Additional flags (1 byte) - bit0=has_dts
	Timestamp uint32 // PTS in 90kHz timescale (4 bytes)
	Dts       uint32 // DTS in 90kHz timescale (4 bytes) - V2 only
	Reserved  uint32 // Reserved for future use (4 bytes)
}

// FrameHeaderV1 is the legacy 10-byte header (kept for compatibility)
type FrameHeaderV1 struct {
	Magic     uint32 // Magic number: 0xFFD8FF00 (4 bytes)
	FrameType uint8  // 0=I-frame, 1=P-frame (1 byte)
	Reserved  uint8  // Reserved for future use (1 byte)
	Timestamp uint32 // Timestamp in 90kHz timescale (4 bytes)
}

// Global sequence counters per stream ID (persists across stream recreation)
var globalSequences = struct {
	mu       sync.Mutex
	counters map[string]uint64
}{
	counters: make(map[string]uint64),
}

// getNextSequence returns the next sequence number for a stream (thread-safe)
func getNextSequence(streamID string) uint64 {
	globalSequences.mu.Lock()
	defer globalSequences.mu.Unlock()
	seq := globalSequences.counters[streamID] + 1
	globalSequences.counters[streamID] = seq
	return seq
}

// clientSender owns the per-client send goroutine. Each WS client gets one;
// broadcastNAL does a non-blocking send into ch so a slow consumer drops only
// its own frames. quit is closed by RemoveClient to stop the goroutine.
type clientSender struct {
	conn *websocket.Conn
	ch   chan []byte
	quit chan struct{}
}

// newClientSender builds a clientSender with a bounded send queue.
func newClientSender(conn *websocket.Conn) *clientSender {
	return &clientSender{
		conn: conn,
		ch:   make(chan []byte, clientSendQueue),
		quit: make(chan struct{}),
	}
}

// H264Stream represents an H264 stream via UDS + WebSocket
type H264Stream struct {
	StreamID   string
	SocketPath string
	Clients    map[*websocket.Conn]*clientSender
	ClientsMux sync.RWMutex
	StopChan   chan struct{}
	Active     bool

	// UDS connection (connMu protects conn; cleared when socket closes so we
	// never treat a closed fd as "still connected" for GetOrCreateStream.)
	conn   net.Conn
	connMu sync.Mutex

	// Cache SPS/PPS for new clients
	spsPPS    [][]byte
	spsPPSMux sync.RWMutex

	// Keyframe request tracking
	// Request keyframe every GOP-1 frames to ensure we have regular IDR frames
	lastKeyframeSeq  uint64
	keyframeInterval uint32 // GOP size
	keyframeMu       sync.Mutex
}

// H264StreamManager manages H264 streams
type H264StreamManager struct {
	streams map[string]*H264Stream
	mu      sync.RWMutex
}

// NewH264StreamManager creates a new H264 stream manager
func NewH264StreamManager() *H264StreamManager {
	return &H264StreamManager{
		streams: make(map[string]*H264Stream),
	}
}

// GetOrCreateStream gets or creates an H264 stream
func (m *H264StreamManager) GetOrCreateStream(streamID, socketPath string) (*H264Stream, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	stream, exists := m.streams[streamID]
	if exists && stream.Active {
		// One readLoop per streamID; UDS reconnect is handled inside readLoop.
		// Do NOT use conn==nil as "recreate stream" — after a disconnect conn is
		// intentionally nil during backoff; the old logic left a stale non-nil
		// closed conn and skipped recreate, so new WS clients got no data (often
		// hit first on main / 1080p).
		return stream, nil
	}

	stream = &H264Stream{
		StreamID:   streamID,
		SocketPath: socketPath,
		Clients:    make(map[*websocket.Conn]*clientSender),
		StopChan:   make(chan struct{}),
		Active:     true,
	}

	m.streams[streamID] = stream
	go stream.readLoop()

	logger.Info("H264 UDS stream started: %s → %s", streamID, socketPath)
	return stream, nil
}

// readLoop connects to UDS and reads packets, with auto-reconnect
func (s *H264Stream) readLoop() {
	defer func() {
		if r := recover(); r != nil {
			logger.Error("H264 UDS readLoop panic for %s: %v", s.StreamID, r)
			s.connMu.Lock()
			if s.conn != nil {
				_ = s.conn.Close()
				s.conn = nil
			}
			s.connMu.Unlock()
			s.Active = false
		}
	}()

	delay := initialReconnect

	for {
		select {
		case <-s.StopChan:
			return
		default:
		}

		connectTime := time.Now()
		err := s.connectAndRead()
		if err != nil {
			logger.Error("H264 UDS read error for %s: %v, reconnecting in %v", s.StreamID, err, delay)
		}

		// Reset backoff after a productive session (>5s means we were streaming fine)
		if time.Since(connectTime) > 5*time.Second {
			delay = initialReconnect
		}

		select {
		case <-s.StopChan:
			return
		case <-time.After(delay):
		}

		// Exponential backoff
		delay = delay * 2
		if delay > maxReconnectDelay {
			delay = maxReconnectDelay
		}
	}
}

// connectAndRead connects to UDS and reads framed packets

// connectAndRead connects to UDS and reads framed packets
// Supports V1 (22 bytes) and V2 (30 bytes) protocol versions
func (s *H264Stream) connectAndRead() error {
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

	logger.Info("H264 UDS connected: %s → %s", s.StreamID, s.SocketPath)

	// Request keyframe on connect for immediate playback
	s.requestKeyframe()

	// Use larger buffer to detect protocol version
	headerBuf := make([]byte, headerSizeV2)
	var dts uint64 // DTS from EncodedPublisher (V2 protocol)
	var pts uint64 // PTS from EncodedPublisher (V2 protocol)
	var pktCount uint64
	lastReport := time.Now()

	// Adaptive frame dropping state
	var consecutiveSlow int
	var droppingPframes bool
	const slowThreshold = 50 * time.Millisecond
	const normalThreshold = 33 * time.Millisecond
	const slowTriggerCount = 5

	for {
		select {
		case <-s.StopChan:
			return nil
		default:
		}

		// Set read deadline to detect dead UDS connections
		_ = conn.SetReadDeadline(time.Now().Add(udsReadTimeout))

		// Read header - start with V1 size, detect V2 from total_size
		if _, err := io.ReadFull(conn, headerBuf[:headerSizeV1]); err != nil {
			return err
		}

		totalSize := binary.LittleEndian.Uint32(headerBuf[0:4])
		// codec := headerBuf[4]          // 0=h264, 1=h265
		flags := headerBuf[5]
		isKeyframe := (flags & 0x01) != 0

		// Detect protocol version and read remaining header if V2
		currentHeaderSize := headerSizeV1

		// Check if this might be V2 protocol (larger total_size indicates V2)
		if totalSize >= headerSizeV2 {
			// Read additional 8 bytes for DTS (V2)
			if _, err := io.ReadFull(conn, headerBuf[headerSizeV1:headerSizeV2]); err != nil {
				return err
			}
			pts = binary.LittleEndian.Uint64(headerBuf[6:14])  // PTS in ns
			dts = binary.LittleEndian.Uint64(headerBuf[22:30]) // DTS in ns
			currentHeaderSize = headerSizeV2
		} else {
			// V1 protocol: no DTS field
			pts = binary.LittleEndian.Uint64(headerBuf[6:14])
			dts = pts // V1: DTS = PTS
		}

		payloadSize := totalSize - uint32(currentHeaderSize)
		if payloadSize == 0 || payloadSize > 4*1024*1024 {
			logger.Warn("H264 UDS: invalid payload size %d (total=%d, header=%d), skipping", payloadSize, totalSize, currentHeaderSize)
			continue
		}

		// Read payload (Annex-B bitstream)
		payload := make([]byte, payloadSize)
		if _, err := io.ReadFull(conn, payload); err != nil {
			return err
		}

		// Measure relay work only. The blocking UDS read above naturally waits
		// for the next frame; at 15fps that wait is ~66ms and must not be
		// treated as backpressure.
		processT0 := time.Now()

		// Get sequence number once per frame (ensures consistency across tracking and broadcasting)
		seq := getNextSequence(s.StreamID)

		// Debug logging: log PTS/DTS values for first 100 frames
		if seq < 100 {
			logger.Debug("H264 UDS [%s]: seq=%d pts=%d dts=%d diff=%d keyframe=%v",
				s.StreamID, seq, pts, dts, int64(pts)-int64(dts), isKeyframe)
		}

		// Always track keyframes for periodic IDR requests (works even without clients)
		isKeyframeFinal := s.trackKeyframes(payload, isKeyframe, (flags&0x01) != 0, seq)

		// Check if we have clients before broadcasting
		s.ClientsMux.RLock()
		clientCount := len(s.Clients)
		s.ClientsMux.RUnlock()

		if clientCount == 0 {
			continue
		}

		// Adaptive frame dropping: skip P-frames when system is overloaded
		if droppingPframes && !isKeyframeFinal {
			pktCount++
			continue
		}

		// Convert PTS/DTS from ns to 90kHz for WebSocket transmission
		pts90kHz := uint32(pts / 11111)
		dts90kHz := uint32(dts / 11111)

		// Split Annex-B bitstream into individual NAL units and broadcast
		s.processAnnexB(payload, isKeyframeFinal, (flags&0x01) != 0, pts90kHz, dts90kHz)

		pktCount++
		processDur := time.Since(processT0)

		// Adaptive frame dropping: track slow cycles
		if processDur > slowThreshold {
			consecutiveSlow++
		} else if processDur < normalThreshold {
			if consecutiveSlow > 0 {
				consecutiveSlow--
			}
		}

		if !droppingPframes && consecutiveSlow >= slowTriggerCount {
			droppingPframes = true
			logger.Warn("H264 UDS: entering degraded mode (dropping P-frames), consecutive_slow=%d process_dur=%v clients=%d",
				consecutiveSlow, processDur, clientCount)
			s.requestKeyframe()
		} else if droppingPframes && consecutiveSlow == 0 {
			droppingPframes = false
			logger.Info("H264 UDS: recovered from degraded mode, resuming normal streaming")
		}

		if processDur > 10*time.Millisecond && !droppingPframes {
			logger.Warn("H264 UDS: SLOW pkt processing %v (pkt=%d, size=%d, key=%v, clients=%d)",
				processDur, pktCount, payloadSize, isKeyframeFinal, clientCount)
		}
		if time.Since(lastReport) >= 10*time.Second {
			logger.Info("H264 UDS: %s stats: %d pkts in %v, dropping_pframes=%v", s.StreamID, pktCount, time.Since(lastReport), droppingPframes)
			lastReport = time.Now()
		}
	}
}
func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// trackKeyframes tracks keyframe/IDR frames and requests periodic IDR frames from encoder
// This runs even without clients to ensure encoder generates IDR frames at regular intervals
// Returns: isKeyframe (true if this frame is an IDR or keyframe)
func (s *H264Stream) trackKeyframes(data []byte, isKeyframe bool, isKeyframeFromFlags bool, seq uint64) bool {
	nals := splitNALUnits(data)
	if len(nals) == 0 {
		return isKeyframe
	}

	// Detect IDR frames (NAL type 5)
	hasIDR := false
	for _, nal := range nals {
		if len(nal) > 0 {
			nalType := nal[0] & 0x1f
			if nalType == 5 { // IDR slice
				hasIDR = true
				break
			}
		}
	}

	// Use camera-daemon's flags as primary indicator (it has encoder feedback)
	// Fallback to NAL type detection if flags are false
	if isKeyframeFromFlags {
		isKeyframe = true
	} else {
		isKeyframe = hasIDR
	}

	// Request keyframe periodically if we don't see natural keyframes
	// This ensures we get IDR frames at regular intervals for MSE to handle GOP>0 correctly
	s.keyframeMu.Lock()
	if s.keyframeInterval == 0 {
		s.keyframeInterval = 30 // Default GOP size
	}
	if isKeyframe {
		s.lastKeyframeSeq = seq
	} else if seq-s.lastKeyframeSeq >= uint64(s.keyframeInterval) {
		// Haven't seen a keyframe in GOP interval, request one
		s.requestKeyframe()
		s.lastKeyframeSeq = seq
	}
	s.keyframeMu.Unlock()

	return isKeyframe
}

// processAnnexB splits Annex-B bitstream into NAL units, converts to AVCC, and broadcasts
// Optimized: batches all NALs of one frame into a single WebSocket message
func (s *H264Stream) processAnnexB(data []byte, isKeyframe bool, isKeyframeFromFlags bool, pts90kHz uint32, dts90kHz uint32) {
	nals := splitNALUnits(data)
	if len(nals) == 0 {
		logger.Warn("H264 UDS: got payload size %d but no NALs found! data prefix: %x", len(data), data[:min(32, len(data))])
		return
	}

	// Calculate total size for all NALs with AVCC length prefixes
	totalSize := 0
	for _, nal := range nals {
		if len(nal) == 0 {
			continue
		}
		totalSize += 4 + len(nal) // 4-byte length prefix + NAL data
	}

	// Allocate single buffer for the entire frame
	frameData := make([]byte, 0, totalSize)

	// Convert all NALs to AVCC format and concatenate
	for _, nal := range nals {
		if len(nal) == 0 {
			continue
		}

		// AVCC format: 4-byte big-endian length + NAL data
		avcc := make([]byte, 4+len(nal))
		binary.BigEndian.PutUint32(avcc[0:4], uint32(len(nal)))
		copy(avcc[4:], nal)

		frameData = append(frameData, avcc...)
	}

	// Broadcast entire frame as a single message
	if len(frameData) > 0 {
		s.broadcastNAL(frameData, isKeyframe, pts90kHz, dts90kHz)
	}
}

// splitNALUnits splits Annex-B bitstream into individual NAL units (without start codes)
func splitNALUnits(data []byte) [][]byte {
	var nals [][]byte
	var starts []int

	// Find all start code positions
	for i := 0; i < len(data)-2; i++ {
		if data[i] == 0 && data[i+1] == 0 {
			if data[i+2] == 1 {
				starts = append(starts, i+3)
			} else if i+3 < len(data) && data[i+2] == 0 && data[i+3] == 1 {
				starts = append(starts, i+4)
			}
		}
	}

	for i, start := range starts {
		var end int
		if i+1 < len(starts) {
			// Find beginning of next start code
			end = starts[i+1]
			// Back up past the start code
			if end >= 4 && data[end-4] == 0 && data[end-3] == 0 && data[end-2] == 0 && data[end-1] == 1 {
				end -= 4
			} else if end >= 3 && data[end-3] == 0 && data[end-2] == 0 && data[end-1] == 1 {
				end -= 3
			}
		} else {
			end = len(data)
		}

		if end > start {
			nals = append(nals, data[start:end])
		}
	}

	return nals
}

// requestKeyframe sends a keyframe request to camera-daemon
func (s *H264Stream) requestKeyframe() {
	s.connMu.Lock()
	c := s.conn
	s.connMu.Unlock()
	if c != nil {
		_, err := c.Write([]byte{ctrlRequestKeyframe})
		if err != nil {
			logger.Warn("H264 UDS: failed to request keyframe for %s: %v", s.StreamID, err)
		} else {
			logger.Info("H264 UDS: requested keyframe for %s", s.StreamID)
		}
	}
}

// AddClient adds a WebSocket client to the stream
func (s *H264Stream) AddClient(conn *websocket.Conn) {
	// Send cached SPS/PPS to new client BEFORE adding to Clients map
	// This prevents concurrent writes to Gorilla websocket by broadcastNAL
	s.spsPPSMux.RLock()
	if len(s.spsPPS) > 0 {
		logger.Debug("Sending %d cached SPS/PPS to new client", len(s.spsPPS))
		for _, nal := range s.spsPPS {
			// Use V2 FrameHeader for SPS/PPS (not keyframes, frameType=1)
			headerBytes := make([]byte, frameHeaderSizeV2)
			binary.BigEndian.PutUint32(headerBytes[0:4], 0xFFD8FF00) // Magic
			headerBytes[4] = 1                                       // Non-keyframe
			headerBytes[5] = 0x01                                    // Flags: V2 protocol indicator (must match broadcastNAL)
			binary.BigEndian.PutUint32(headerBytes[6:10], 0)         // PTS = 0
			binary.BigEndian.PutUint32(headerBytes[10:14], 0)        // DTS = 0
			// Reserved bytes [14:17] are already 0
			frameWithHeader := append(headerBytes[:], nal...)
			_ = conn.SetWriteDeadline(time.Now().Add(spsWriteTimeout))
			if err := conn.WriteMessage(websocket.BinaryMessage, frameWithHeader); err != nil {
				_ = conn.SetWriteDeadline(time.Time{})
				logger.Warn("H264 UDS: failed to send cached SPS/PPS to new client: %v", err)
				s.spsPPSMux.RUnlock()
				return
			}
			_ = conn.SetWriteDeadline(time.Time{})
		}
	}
	s.spsPPSMux.RUnlock()

	cs := newClientSender(conn)
	s.ClientsMux.Lock()
	s.Clients[conn] = cs
	count := len(s.Clients)
	s.ClientsMux.Unlock()
	go s.sendLoop(cs)
	logger.Debug("H264 client added to stream %s, total: %d", s.StreamID, count)

	// Request keyframe for immediate playback
	s.requestKeyframe()
}

// RemoveClient removes a WebSocket client from the stream.
// Idempotent: safe to call from both the WS read-loop defer and the sendLoop
// self-eviction path. Closes quit to stop the send goroutine, deletes from the
// map, and closes the underlying socket.
func (s *H264Stream) RemoveClient(conn *websocket.Conn) {
	s.ClientsMux.Lock()
	cs, ok := s.Clients[conn]
	if ok {
		delete(s.Clients, conn)
		close(cs.quit) // tell sendLoop to stop (non-blocking sends become drops)
	}
	count := len(s.Clients)
	s.ClientsMux.Unlock()
	if ok {
		_ = conn.Close()
	}
	logger.Debug("H264 client removed from stream %s, total: %d", s.StreamID, count)
}

// sendLoop is the per-client writer goroutine. It owns all writes to conn so
// there is no concurrent-write race on the Gorilla websocket. Frames arrive on
// ch from broadcastNAL (non-blocking); if the consumer is slow the channel
// fills and broadcastNAL drops frames for this client only — the relay is
// never blocked. A client whose writes stay slow or error out is evicted.
func (s *H264Stream) sendLoop(cs *clientSender) {
	var slow int
	for {
		select {
		case <-cs.quit:
			return
		case frame := <-cs.ch:
			_ = cs.conn.SetWriteDeadline(time.Now().Add(wsWriteTimeout))
			t0 := time.Now()
			err := cs.conn.WriteMessage(websocket.BinaryMessage, frame)
			_ = cs.conn.SetWriteDeadline(time.Time{})
			if err != nil {
				// Dead/unreachable client — evict so the map doesn't leak.
				s.RemoveClient(cs.conn)
				return
			}
			d := time.Since(t0)
			if d > slowWriteEvictAt {
				slow++
				s.ClientsMux.RLock()
				total := len(s.Clients)
				s.ClientsMux.RUnlock()
				logger.Warn("H264 WS: SLOW WriteMessage %v (client=%p, %d bytes, total_clients=%d)",
					d, cs.conn, len(frame), total)
				if slow >= maxConsecutiveSlow {
					logger.Warn("H264 WS: evicting slow client %p after %d consecutive slow writes", cs.conn, slow)
					s.RemoveClient(cs.conn)
					return
				}
			} else if slow > 0 {
				slow--
			}
		}
	}
}

// broadcastNAL sends an H264 NAL unit to all clients
// Uses V2 FrameHeader (18 bytes) with DTS support
func (s *H264Stream) broadcastNAL(frameData []byte, isKeyframe bool, pts90kHz uint32, dts90kHz uint32) {
	// V2 FrameHeader (18 bytes)
	headerBytes := make([]byte, frameHeaderSizeV2)
	binary.BigEndian.PutUint32(headerBytes[0:4], 0xFFD8FF00)            // Magic
	headerBytes[4] = uint8(map[bool]int{true: 0, false: 1}[isKeyframe]) // FrameType
	headerBytes[5] = 0x01                                               // Flags: has_dts=1 (V2 protocol indicator)
	binary.BigEndian.PutUint32(headerBytes[6:10], pts90kHz)             // PTS
	binary.BigEndian.PutUint32(headerBytes[10:14], dts90kHz)            // DTS
	// Reserved bytes [14:18] are already 0

	// Prepend header to frame data
	frameWithHeader := append(headerBytes[:], frameData...)

	// Check NAL type and cache SPS/PPS
	// When a new SPS is detected, clear the old cache entirely to prevent
	// stale init segments after resolution/bitrate/codec changes.
	if len(frameData) >= 5 {
		nalType := frameData[4] & 0x1f
		if nalType == 7 { // SPS — start of a new parameter set
			s.spsPPSMux.Lock()
			cached := make([]byte, len(frameData))
			copy(cached, frameData)
			s.spsPPS = [][]byte{cached} // Replace, don't append
			s.spsPPSMux.Unlock()
		} else if nalType == 8 { // PPS — append to current set
			s.spsPPSMux.Lock()
			cached := make([]byte, len(frameData))
			copy(cached, frameData)
			s.spsPPS = append(s.spsPPS, cached)
			if len(s.spsPPS) > 2 {
				s.spsPPS = s.spsPPS[len(s.spsPPS)-2:]
			}
			s.spsPPSMux.Unlock()
		}
	}

	// Non-blocking fan-out: enqueue frame into each client's bounded channel.
	// A slow consumer whose channel is full drops THIS frame only — the relay
	// loop is never blocked, and other clients keep streaming. The per-client
	// send goroutine (sendLoop) owns the actual WriteMessage, so there is no
	// concurrent-write race on the Gorilla websocket.
	//
	// frameWithHeader is built fresh per call and only read (never mutated)
	// by the sendLoop writers, so sharing the slice across clients is safe.
	var dropped int
	s.ClientsMux.RLock()
	total := len(s.Clients)
	for _, cs := range s.Clients {
		select {
		case cs.ch <- frameWithHeader:
		default:
			dropped++ // slow consumer: channel full, drop this client's frame
		}
	}
	s.ClientsMux.RUnlock()
	if dropped > 0 && (isKeyframe || dropped >= total/2) {
		logger.Warn("H264 WS: dropped %d/%d client frames (key=%v, stream=%s) — slow consumers",
			dropped, total, isKeyframe, s.StreamID)
	}
}

// ClearSpsPpsCache clears the cached SPS/PPS data.
// Should be called when encoder parameters change (resolution, codec, bitrate, fps)
// to prevent stale init segments being sent to new clients.
func (s *H264Stream) ClearSpsPpsCache() {
	s.spsPPSMux.Lock()
	s.spsPPS = nil
	s.spsPPSMux.Unlock()
	logger.Info("H264 UDS: cleared SPS/PPS cache for %s", s.StreamID)
}

// SetKeyframeInterval updates the GOP-based keyframe request interval.
func (s *H264Stream) SetKeyframeInterval(interval uint32) {
	s.keyframeMu.Lock()
	defer s.keyframeMu.Unlock()
	if interval > 0 {
		s.keyframeInterval = interval
	}
}

// Stop stops the stream
func (s *H264Stream) Stop() {
	close(s.StopChan)
	s.connMu.Lock()
	if s.conn != nil {
		_ = s.conn.Close()
		s.conn = nil
	}
	s.connMu.Unlock()
	s.Active = false

	// Restarting a relay must also close its WebSocket clients. Otherwise they
	// remain connected to the stopped H264Stream while the replacement relay has
	// no clients, leaving the browser on a permanently silent connection.
	s.ClientsMux.Lock()
	clients := make([]*websocket.Conn, 0, len(s.Clients))
	for conn, sender := range s.Clients {
		delete(s.Clients, conn)
		close(sender.quit)
		clients = append(clients, conn)
	}
	s.ClientsMux.Unlock()

	for _, conn := range clients {
		_ = conn.WriteControl(
			websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.CloseNormalClosure, "stream restarted"),
			time.Now().Add(time.Second),
		)
		_ = conn.Close()
	}
}

// ClearSpsPpsCacheForStream clears the SPS/PPS cache for a specific stream ID
func (m *H264StreamManager) ClearSpsPpsCacheForStream(streamID string) {
	m.mu.RLock()
	stream, exists := m.streams[streamID]
	m.mu.RUnlock()
	if exists {
		stream.ClearSpsPpsCache()
	}
}

// SetKeyframeInterval updates the keyframe request interval for a specific stream.
func (m *H264StreamManager) SetKeyframeInterval(streamID string, interval uint32) {
	m.mu.RLock()
	stream, exists := m.streams[streamID]
	m.mu.RUnlock()
	if exists {
		stream.SetKeyframeInterval(interval)
	}
}

// RemoveStream removes a stream
func (m *H264StreamManager) RemoveStream(streamID string) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if stream, exists := m.streams[streamID]; exists {
		stream.Stop()
		delete(m.streams, streamID)
		logger.Info("H264 stream removed: %s", streamID)
	}
}

// ForceKeyframe requests an immediate IDR from the encoder for a specific stream.
// Used after an encoder/pipeline reconfigure so the first frame clients receive
// after the parameter change is a clean keyframe, avoiding decoder artifacts
// (花屏/黑屏) from transitional P-frames referencing the pre-reconfig reference.
func (m *H264StreamManager) ForceKeyframe(streamID string) {
	m.mu.RLock()
	stream, exists := m.streams[streamID]
	m.mu.RUnlock()
	if exists {
		stream.requestKeyframe()
	}
}
