// Package wsdiscovery implements the device side of WS-Discovery (SOAP-over
// UDP multicast) so that NVR/VMS clients can find the NE503 on the LAN.
//
// The onvif-go library only ships a WS-Discovery *client* (it sends Probe and
// reads ProbeMatch). A Profile S device must additionally:
//
//   - multicast a Hello on startup advertising its XAddr,
//   - answer client Probe messages with a unicast ProbeMatch,
//   - optionally multicast a Bye on shutdown.
//
// This package provides exactly that responder. It listens on the standard
// WS-Discovery multicast group 239.255.255.250:3702, reusing the rp_filter
// disable + interface rebinding pattern proven by platform/device-discovery.
package wsdiscovery

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/google/uuid"
)

// Config configures the WS-Discovery responder.
type Config struct {
	// EndpointUUID is the stable urn:uuid:... device endpoint address.
	EndpointUUID string
	// Types is the advertised device type, e.g. "dp0:NetworkVideoTransmitter".
	Types string
	// Scopes are the ONVIF scope URIs (Profiles, name, location).
	Scopes []string
	// XAddrs are the device service URLs advertised to clients.
	XAddrs []string
	// Interface is the LAN interface to join (e.g. "eth0"). Empty = default.
	Interface string
	// MulticastAddr defaults to 239.255.255.250.
	MulticastAddr string
	// MulticastPort defaults to 3702.
	MulticastPort int
}

// Responder answers WS-Discovery Probe messages and sends Hello/Bye.
type Responder struct {
	cfg    Config
	mu     sync.RWMutex
	xaddrs []string
	conn   *net.UDPConn
	group  *net.UDPAddr
}

// New creates a responder. The network socket is not opened until Start.
func New(cfg Config) *Responder {
	if cfg.MulticastAddr == "" {
		cfg.MulticastAddr = "239.255.255.250"
	}
	if cfg.MulticastPort == 0 {
		cfg.MulticastPort = 3702
	}
	if cfg.Types == "" {
		cfg.Types = "dp0:NetworkVideoTransmitter"
	}
	return &Responder{cfg: cfg, xaddrs: cfg.XAddrs}
}

// SetXAddrs updates the advertised service URLs (e.g. after a DHCP renewal).
func (r *Responder) SetXAddrs(xaddrs []string) {
	r.mu.Lock()
	r.xaddrs = append([]string(nil), xaddrs...)
	r.mu.Unlock()
}

// Start joins the multicast group, sends an initial Hello, then answers Probe
// messages until ctx is cancelled. It sends a Bye on exit.
func (r *Responder) Start(ctx context.Context) error {
	group := &net.UDPAddr{IP: net.ParseIP(r.cfg.MulticastAddr), Port: r.cfg.MulticastPort}
	r.group = group

	var iface *net.Interface
	if r.cfg.Interface != "" {
		if v, err := net.InterfaceByName(r.cfg.Interface); err == nil {
			iface = v
		} else {
			log.Printf("[onvif] interface %s not found, using system default multicast: %v", r.cfg.Interface, err)
		}
	}

	conn, err := net.ListenMulticastUDP("udp", iface, group)
	if err != nil {
		return fmt.Errorf("listen ws-discovery multicast %s:%d: %w", r.cfg.MulticastAddr, r.cfg.MulticastPort, err)
	}
	r.conn = conn

	const udpBufSize = 1 << 16 // 64 KiB
	_ = conn.SetReadBuffer(udpBufSize)

	log.Printf("[onvif] WS-Discovery listening on %s:%d (iface=%s)", r.cfg.MulticastAddr, r.cfg.MulticastPort, ifaceName(iface))

	if err := r.SendHello(); err != nil {
		log.Printf("[onvif] initial Hello failed: %v", err)
	} else {
		log.Printf("[onvif] WS-Discovery Hello sent: %s", strings.Join(r.snapshotXAddrs(), " "))
	}

	err = r.readLoop(ctx)

	_ = r.SendBye()
	_ = conn.Close()
	return err
}

// readLoop reads UDP datagrams and answers Probes until ctx is cancelled.
func (r *Responder) readLoop(ctx context.Context) error {
	buf := make([]byte, udpReadSize)
	for {
		if err := ctx.Err(); err != nil {
			return nil
		}
		// Short deadline so the loop can observe ctx cancellation.
		_ = r.conn.SetReadDeadline(time.Now().Add(readPollInterval))
		n, src, err := r.conn.ReadFromUDP(buf)
		if err != nil {
			if isTimeout(err) {
				continue
			}
			return fmt.Errorf("ws-discovery read: %w", err)
		}
		msg := buf[:n]
		if !isProbe(msg) {
			continue
		}
		relatesTo := extractMessageID(msg)
		resp := r.buildMessage(actionProbeMatches, relatesTo, r.probeMatchesBody())
		if _, werr := r.conn.WriteToUDP(resp, src); werr != nil {
			log.Printf("[onvif] ProbeMatch send to %s failed: %v", src, werr)
			continue
		}
		log.Printf("[onvif] ProbeMatch sent to %s for XAddr %s", src, strings.Join(r.snapshotXAddrs(), " "))
	}
}

// SendHello multicasts a WS-Discovery Hello advertising the current XAddrs.
func (r *Responder) SendHello() error {
	return r.sendToGroup(actionHello, r.helloBody())
}

// SendBye multicasts a WS-Discovery Bye so clients remove the device promptly.
func (r *Responder) SendBye() error {
	return r.sendToGroup(actionBye, r.byeBody())
}

func (r *Responder) sendToGroup(action, body string) error {
	if r.conn == nil || r.group == nil {
		return errNotStarted
	}
	_, err := r.conn.WriteToUDP(r.buildMessage(action, "", body), r.group)
	return err
}

// ---- SOAP message construction ----

func (r *Responder) snapshotXAddrs() []string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return append([]string(nil), r.xaddrs...)
}

func (r *Responder) helloBody() string {
	return bodyElement("d:Hello", r.cfg.EndpointUUID, r.cfg.Types, r.cfg.Scopes, r.snapshotXAddrs())
}

func (r *Responder) probeMatchesBody() string {
	inner := bodyElement("d:ProbeMatch", r.cfg.EndpointUUID, r.cfg.Types, r.cfg.Scopes, r.snapshotXAddrs())
	return "<d:ProbeMatches>" + inner + "</d:ProbeMatches>"
}

func (r *Responder) byeBody() string {
	return bodyElement("d:Bye", r.cfg.EndpointUUID, r.cfg.Types, r.cfg.Scopes, r.snapshotXAddrs())
}

// bodyElement renders the inner payload shared by Hello/ProbeMatch/Bye.
func bodyElement(wrapper, endpoint, types string, scopes, xaddrs []string) string {
	return fmt.Sprintf(
		"<%s>"+
			"<a:EndpointReference><a:Address>%s</a:Address></a:EndpointReference>"+
			"<d:Types>%s</d:Types>"+
			"<d:Scopes>%s</d:Scopes>"+
			"<d:XAddrs>%s</d:XAddrs>"+
			"<d:MetadataVersion>1</d:MetadataVersion>"+
			"</%s>",
		wrapper, endpoint, types, strings.Join(scopes, " "), strings.Join(xaddrs, " "), wrapper)
}

// buildMessage wraps a body in a SOAP 1.2 envelope with WS-Addressing headers.
// relatesTo is included when non-empty (used to correlate a ProbeMatch reply).
func (r *Responder) buildMessage(action, relatesTo, body string) []byte {
	var sb strings.Builder
	sb.WriteString(xmlDecl)
	sb.WriteString(envelopeOpen)
	sb.WriteString("<s:Header>")
	fmt.Fprintf(&sb, "<a:Action s:mustUnderstand=\"1\">%s</a:Action>", action)
	fmt.Fprintf(&sb, "<a:MessageID>urn:uuid:%s</a:MessageID>", uuid.NewString())
	if relatesTo != "" {
		fmt.Fprintf(&sb, "<a:RelatesTo>%s</a:RelatesTo>", relatesTo)
	}
	fmt.Fprintf(&sb, "<a:To s:mustUnderstand=\"1\">%s</a:To>", actionTo(action))
	sb.WriteString("</s:Header>")
	sb.WriteString("<s:Body>")
	sb.WriteString(body)
	sb.WriteString("</s:Body>")
	sb.WriteString("</s:Envelope>")
	return []byte(sb.String())
}

// actionTo returns the WS-Addressing To for a given discovery action.
func actionTo(action string) string {
	switch action {
	case actionProbeMatches:
		return anonymousAddress
	default: // Hello and Bye target the discovery multicast urn.
		return discoveryTo
	}
}

// ---- Probe detection & message-ID extraction ----

var (
	// messageIDRE matches <[prefix:]MessageID>value</...> regardless of prefix.
	messageIDRE = regexp.MustCompile(`<(?:[A-Za-z0-9_]+:)?MessageID[^>]*>([^<]+)</`)
)

// isProbe reports whether a datagram is a WS-Discovery Probe (not ProbeMatches).
// The Probe action URI ends with "/Probe<"; ProbeMatches ends with "/ProbeMatches".
func isProbe(msg []byte) bool {
	return strings.Contains(string(msg), probeActionTail)
}

// extractMessageID pulls the WS-Addressing MessageID from a SOAP datagram so the
// ProbeMatch reply can correlate it via RelatesTo. Returns "" if absent.
func extractMessageID(msg []byte) string {
	m := messageIDRE.FindSubmatch(msg)
	if len(m) < 2 {
		return ""
	}
	return strings.TrimSpace(string(m[1]))
}

func isTimeout(err error) bool {
	var ne net.Error
	return errors.As(err, &ne) && ne.Timeout()
}

func ifaceName(iface *net.Interface) string {
	if iface == nil {
		return "default"
	}
	return iface.Name
}

// ---- constants ----

const (
	readPollInterval = 1 * time.Second
	udpReadSize      = 8192

	probeActionTail = "discovery/Probe<"

	actionHello        = "http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello"
	actionBye          = "http://schemas.xmlsoap.org/ws/2005/04/discovery/Bye"
	actionProbeMatches = "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches"

	discoveryTo      = "urn:schemas-xmlsoap-org:ws:2005:04:discovery"
	anonymousAddress = "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"

	xmlDecl = `<?xml version="1.0" encoding="UTF-8"?>` + "\n"

	// envelopeOpen declares SOAP 1.2, WS-Addressing 2004/08, WS-Discovery 2005/04,
	// and the ONVIF network WSDL (dp0) used by the NetworkVideoTransmitter type.
	envelopeOpen = `<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" ` +
		`xmlns:a="http://schemas.xmlsoap.org/ws/2004/08/addressing" ` +
		`xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" ` +
		`xmlns:dp0="http://www.onvif.org/ver10/network/wsdl">`
)

// errNotStarted is returned when Hello/Bye is sent before Start opens the socket.
var errNotStarted = fmt.Errorf("wsdiscovery: responder not started")
