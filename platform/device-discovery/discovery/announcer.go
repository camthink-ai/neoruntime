package discovery

import (
	"context"
	"fmt"
	"log"
	"net"
	"time"
)

type AnnouncerConfig struct {
	Product   string
	SN        string
	FW        string
	HW        string
	Port      int
	Caps      []string
	Interface string
	Interval  time.Duration
}

type Announcer struct {
	cfg      AnnouncerConfig
	announce Announce
	conn     *net.UDPConn
	ctx      context.Context
	cancel   context.CancelFunc
}

func NewAnnouncer(cfg AnnouncerConfig) *Announcer {
	return &Announcer{
		cfg: cfg,
		announce: Announce{
			Product: cfg.Product,
			SN:      cfg.SN,
			FW:      cfg.FW,
			HW:      cfg.HW,
			Port:    cfg.Port,
			Caps:    cfg.Caps,
		},
	}
}

func (an *Announcer) Start(ctx context.Context) error {
	childCtx, cancel := context.WithCancel(ctx)
	an.ctx = childCtx
	an.cancel = cancel

	an.announce.Type = AnnounceType
	if an.announce.IP == "" {
		an.announce.IP = an.detectIP()
	}
	if an.announce.MAC == "" {
		an.announce.MAC = an.detectMAC()
	}

	// Bind to detected IP so kernel knows which interface to use for multicast
	srcIP := net.ParseIP(an.announce.IP)
	if srcIP == nil {
		srcIP = net.ParseIP("0.0.0.0")
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: srcIP, Port: 0})
	if err != nil {
		cancel()
		return fmt.Errorf("create udp socket: %w", err)
	}
	an.conn = conn

	go an.loop()
	return nil
}

func (an *Announcer) Stop() {
	if an.cancel != nil {
		an.cancel()
	}
	if an.conn != nil {
		an.conn.Close()
	}
}

func (an *Announcer) loop() {
	addr := MulticastUDPAddr()
	ticker := time.NewTicker(an.cfg.Interval)
	defer ticker.Stop()

	an.refreshIPAndSend(addr)
	for {
		select {
		case <-an.ctx.Done():
			return
		case <-ticker.C:
			an.refreshIPAndSend(addr)
		}
	}
}

// refreshIPAndSend re-detects IP before each send and rebinds socket if changed.
func (an *Announcer) refreshIPAndSend(addr *net.UDPAddr) {
	newIP := an.detectIP()
	if newIP != an.announce.IP {
		log.Printf("[announcer] IP changed: %s -> %s, rebinding socket", an.announce.IP, newIP)
		an.announce.IP = newIP
		an.announce.MAC = an.detectMAC()

		if an.conn != nil {
			an.conn.Close()
		}
		srcIP := net.ParseIP(newIP)
		if srcIP == nil {
			srcIP = net.ParseIP("0.0.0.0")
		}
		conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: srcIP, Port: 0})
		if err != nil {
			log.Printf("[announcer] rebind failed: %v", err)
			return
		}
		an.conn = conn
	}
	an.send(addr)
}

func (an *Announcer) send(addr *net.UDPAddr) {
	data, err := EncodeAnnounce(an.announce)
	if err != nil {
		log.Printf("[announcer] encode error: %v", err)
		return
	}
	_, err = an.conn.WriteToUDP(data, addr)
	if err != nil {
		log.Printf("[announcer] send error: %v", err)
		return
	}
	log.Printf("[announcer] sent: %s (%s) at %s:%d", an.announce.SN, an.announce.Product, an.announce.IP, an.announce.Port)
}

// Refresh re-detects IP and MAC from network interfaces and rebinds socket.
// Call after network configuration changes.
func (an *Announcer) Refresh() {
	newIP := an.detectIP()
	newMAC := an.detectMAC()
	if newIP != an.announce.IP {
		log.Printf("[announcer] IP changed: %s -> %s, rebinding socket", an.announce.IP, newIP)
		an.announce.IP = newIP
		an.announce.MAC = newMAC
		if an.conn != nil {
			an.conn.Close()
		}
		srcIP := net.ParseIP(newIP)
		if srcIP == nil {
			srcIP = net.ParseIP("0.0.0.0")
		}
		conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: srcIP, Port: 0})
		if err != nil {
			log.Printf("[announcer] rebind failed: %v", err)
			return
		}
		an.conn = conn
	}
}

func (an *Announcer) detectIP() string {
	ifaces, err := net.Interfaces()
	if err != nil {
		return "0.0.0.0"
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		if an.cfg.Interface != "" && iface.Name != an.cfg.Interface {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, addr := range addrs {
			if ipnet, ok := addr.(*net.IPNet); ok && ipnet.IP.To4() != nil {
				return ipnet.IP.String()
			}
		}
	}
	return "0.0.0.0"
}

func (an *Announcer) detectMAC() string {
	ifaces, err := net.Interfaces()
	if err != nil {
		return ""
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		if an.cfg.Interface != "" && iface.Name != an.cfg.Interface {
			continue
		}
		if iface.HardwareAddr != nil {
			return iface.HardwareAddr.String()
		}
	}
	return ""
}
